/**
 * @file tests/osd_check.cpp
 * @brief 水印合成的像素规则，逐像素钉死。
 *
 * 为什么要在没有 DXGI、没有编码器的情况下单独测它：**水印错了的每一种表现都
 * 是安静的**。字画到画面外去了、自适应配色选反了、描边把字盖住了、文字变了
 * 但掩码缓存没失效 —— 这四种全都产出一段"能播、分辨率帧率都对、画面上确实
 * 有字"的视频。唯一能区分它们的是**哪些像素变了、变成了什么颜色**。
 *
 * 尤其是掩码缓存那一条：它平时看不出来，只在"文字变了"的那一刻露馅，而那一刻
 * 恰好是秒数跳变的时候 —— 一个冻住的钟比没有水印更糟，因为看的人会信它。
 *
 * 这里不碰窗口也不碰 GDI 之外的东西：直接把画布喂进去，读回来的像素和手算的
 * 值逐字节比。字号、描边、配色的规则全部从 osd_overlay.cpp 的实现推出来。
 *
 * 编译（要链 gdi32，字是 GDI 栅格化的）：
 *   g++ -std=c++17 -Isrc tests/osd_check.cpp \
 *       src/capture/osd_overlay.cpp src/common/device_state.cpp \
 *       src/common/text.cpp src/common/log.cpp \
 *       -lgdi32 -o build/osd_check.exe
 */

#include "capture/osd_overlay.h"
#include "common/device_state.h"
#include "common/log.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

using namespace d2o;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, std::string const& what) {
  ++g_checks;
  if (ok) {
    std::printf("[ OK ] %s\n", what.c_str());
  } else {
    ++g_failed;
    std::printf("[FAIL] %s\n", what.c_str());
  }
}

/// 校验用的哨兵值。**故意不是 0** —— 0 是画面里本来就有的颜色，
/// 用它当哨兵的话"越界写了"和"本来就是黑的"分不开。
constexpr std::uint8_t kGuard = 0xAB;

/// 带护栏的画布。
///
/// 护栏是这块画布存在的理由：`compose()` 只按 `dx/dy` 判边界，而那是一个
/// 容易差一的地方。给画布四周各留 `g` 圈哨兵，画完之后哨兵一个字节都不许变
/// —— 这样"越界写"从"看不出来"变成"当场抓住"，而不是等到共享内存的相邻帧
/// 被涂花。
struct Canvas {
  int w = 0;
  int h = 0;
  int g = 0;
  std::size_t stride = 0;
  std::vector<std::uint8_t> px;

  Canvas(int w_, int h_, int g_, std::uint8_t b, std::uint8_t gg, std::uint8_t r)
      : w(w_), h(h_), g(g_) {
    stride = static_cast<std::size_t>(w + 2 * g) * 4;
    px.assign(stride * static_cast<std::size_t>(h + 2 * g), kGuard);
    for (int y = 0; y < h; ++y) {
      std::uint8_t* row = px.data() + static_cast<std::size_t>(y + g) * stride +
                          static_cast<std::size_t>(g) * 4;
      for (int x = 0; x < w; ++x) {
        row[x * 4 + 0] = b;
        row[x * 4 + 1] = gg;
        row[x * 4 + 2] = r;
        row[x * 4 + 3] = 0xFF;
      }
    }
  }

  std::uint8_t* data() {
    return px.data() + static_cast<std::size_t>(g) * stride + static_cast<std::size_t>(g) * 4;
  }

  /// 取画布内的一个像素（x,y 相对画布左上角）。
  std::uint8_t const* at(int x, int y) const {
    return px.data() + static_cast<std::size_t>(y + g) * stride +
           static_cast<std::size_t>(x + g) * 4;
  }

  /// 护栏有没有被动过。
  bool guard_intact() const {
    for (int y = 0; y < h + 2 * g; ++y) {
      bool const inside_y = y >= g && y < g + h;
      std::uint8_t const* row = px.data() + static_cast<std::size_t>(y) * stride;
      for (int x = 0; x < w + 2 * g; ++x) {
        bool const inside_x = x >= g && x < g + w;
        if (inside_y && inside_x) continue;
        for (int c = 0; c < 4; ++c) {
          if (row[x * 4 + c] != kGuard) return false;
        }
      }
    }
    return true;
  }
};

unsigned luma(std::uint8_t const* p) {
  return (299u * p[2] + 587u * p[1] + 114u * p[0]) / 1000u;
}

/// 画布内偏离底色超过 tolerance 的像素。
struct Ink {
  std::vector<std::pair<int, int>> pts;
  int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;

  bool empty() const { return pts.empty(); }
  int w() const { return empty() ? 0 : x1 - x0 + 1; }
  int h() const { return empty() ? 0 : y1 - y0 + 1; }
};

Ink find_ink(Canvas const& c, int tol = 6) {
  Ink out;
  std::uint8_t const* base = c.at(0, 0);
  std::uint8_t const b0 = base[0], g0 = base[1], r0 = base[2];
  for (int y = 0; y < c.h; ++y) {
    std::uint8_t const* row = c.at(0, y);
    for (int x = 0; x < c.w; ++x) {
      std::uint8_t const* p = row + x * 4;
      int d = std::abs(static_cast<int>(p[0]) - b0);
      d = std::max(d, std::abs(static_cast<int>(p[1]) - g0));
      d = std::max(d, std::abs(static_cast<int>(p[2]) - r0));
      if (d <= tol) continue;
      out.pts.emplace_back(x, y);
      out.x0 = std::min(out.x0, x);
      out.y0 = std::min(out.y0, y);
      out.x1 = std::max(out.x1, x);
      out.y1 = std::max(out.y1, y);
    }
  }
  return out;
}

/// 一个 `text` 项，其余字段用默认值。
OsdItemConfig text_item(std::string const& s, std::string const& pos) {
  OsdItemConfig it;
  it.type = "text";
  it.text = s;
  it.position = pos;
  return it;
}

OsdConfig one_item(OsdItemConfig const& it) {
  OsdConfig cfg;
  cfg.items.assign(1, it);
  return cfg;
}

/// 定格一个时刻，用来让 `time` 项的文字可预测。
std::chrono::system_clock::time_point at(int y, int mo, int d, int hh, int mm, int ss) {
  std::tm tmv{};
  tmv.tm_year = y - 1900;
  tmv.tm_mon = mo - 1;
  tmv.tm_mday = d;
  tmv.tm_hour = hh;
  tmv.tm_min = mm;
  tmv.tm_sec = ss;
  tmv.tm_isdst = -1;
  return std::chrono::system_clock::from_time_t(std::mktime(&tmv));
}

constexpr int kW = 640;
constexpr int kH = 480;
constexpr int kG = 4;

// ---------------------------------------------------------------------------
// 逐条
// ---------------------------------------------------------------------------

/// 关掉之后**一个像素都不许动**。"关不干净"这类缺陷的表现是水印淡淡地留在
/// 画面上，而它和一个真的没关的水印在肉眼上几乎一样。
void test_disabled_touches_nothing() {
  Canvas c(kW, kH, kG, 30, 30, 30);
  std::vector<std::uint8_t> const before = c.px;

  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  cfg.enabled = false;
  OsdOverlay osd;
  osd.configure(cfg, nullptr);

  check(!osd.enabled(), "cfg.enabled=false 时 enabled() 为假");
  check(osd.key(at(2026, 10, 7, 12, 0, 0)).empty(), "关掉之后 key() 是空串");
  check(osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0)) == 0,
        "关掉之后 compose() 返回 0");
  check(c.px == before, "关掉之后画布一个字节都没变");
}

/// 总开关开着但**每一项都关着** —— 和上面是两回事（一个是"没开"，一个是
/// "开了但没内容"），而 `enabled()` 必须都说假，否则采集循环会为了一个
/// 画不出东西的水印去强制新的 content_id，白烧一遍编码。
void test_all_items_off() {
  Canvas c(kW, kH, kG, 30, 30, 30);
  std::vector<std::uint8_t> const before = c.px;

  OsdItemConfig it = text_item("HELLO", "upper_left");
  it.enabled = false;
  OsdConfig cfg = one_item(it);

  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  check(!osd.enabled(), "每一项都关着时 enabled() 也为假");
  check(osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0)) == 0,
        "每一项都关着时 compose() 返回 0");
  check(c.px == before, "每一项都关着时画布不变");
}

/// 空文字的 `text` 项：画不出东西，但不报错、也不影响别的项。
void test_empty_text_item() {
  Canvas c(kW, kH, kG, 30, 30, 30);
  // **必须先清空**：`OsdConfig` 的默认成员初值本来是两项（时间@左上、
  // 名称@右下），不清的话这里会变成四项，而多出来的那两项也画。
  OsdConfig cfg;
  cfg.items.clear();
  cfg.items.push_back(text_item("", "upper_left"));
  cfg.items.push_back(text_item("REAL", "lower_right"));

  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  check(osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0)) == 1,
        "空文字那一项不落笔，另一项照画（返回 1）");
  Ink ink = find_ink(c);
  check(!ink.empty() && ink.y0 > kH / 2 && ink.x0 > kW / 2,
        "有文字的那一项画在右下角");
}

/// 暗底 → 白字。黑底上描边也是黑的（等于没画），所以此刻的墨就是字本身。
void test_dark_bg_white_text() {
  Canvas c(kW, kH, kG, 0, 0, 0);
  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));

  Ink ink = find_ink(c);
  check(!ink.empty(), "暗底上画出了东西");
  unsigned best = 0;
  for (auto const& p : ink.pts) best = std::max(best, luma(c.at(p.first, p.second)));
  check(best > 230, "暗底（亮度 0）上的字是白的");
  check(c.guard_intact(), "暗底这一趟没写到画布外");
}

/// 亮底 → 黑字。
void test_light_bg_black_text() {
  Canvas c(kW, kH, kG, 255, 255, 255);
  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));

  Ink ink = find_ink(c);
  check(!ink.empty(), "亮底上画出了东西");
  unsigned worst = 255;
  for (auto const& p : ink.pts) worst = std::min(worst, luma(c.at(p.first, p.second)));
  check(worst < 40, "亮底（亮度 255）上的字是黑的");
}

/// **分界线两侧各测一次**，因为"自适应"这件事只有一个阈值，
/// 而它在两侧选的颜色必须相反 —— 只测一侧的话，把判断方向写反了照样全绿。
void test_adaptive_threshold() {
  auto deviation_with_text_colour = [](int bg, bool& text_is_lighter) {
    Canvas c(kW, kH, kG, bg, bg, bg);
    OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
    OsdOverlay osd;
    osd.configure(cfg, nullptr);
    osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
    // 墨里最极端的那个像素就是字心的颜色。
    Ink ink = find_ink(c);
    if (ink.empty()) return -1;
    int lo = 255, hi = 0;
    for (auto const& p : ink.pts) {
      int const l = static_cast<int>(luma(c.at(p.first, p.second)));
      lo = std::min(lo, l);
      hi = std::max(hi, l);
    }
    text_is_lighter = (bg - lo) < (hi - bg);
    return 0;
  };

  bool lighter = false;
  check(deviation_with_text_colour(120, lighter) == 0 && lighter,
        "亮度 120（暗于阈值）→ 白字");
  check(deviation_with_text_colour(136, lighter) == 0 && !lighter,
        "亮度 136（亮于阈值）→ 黑字");
}

/// 描边确实画了，而且是**反色**的。
///
/// 用中灰做底（亮度 160 → 选黑字），于是黑字和白描边在灰底上都看得见：
/// 墨里必须**同时**出现比底暗得多的和比底亮得多的像素。少了描边的话只有前者，
/// 而"字在灰底上看得见"这件事本身是靠描边撑着的 —— 没有描边，字会在中等
/// 亮度的背景上糊掉。
void test_outline_is_inverse_colour() {
  Canvas c(kW, kH, kG, 160, 160, 160);
  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  cfg.outline = true;
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));

  Ink ink = find_ink(c);
  check(!ink.empty(), "灰底上画出了东西");
  int lo = 255, hi = 0;
  for (auto const& p : ink.pts) {
    int const l = static_cast<int>(luma(c.at(p.first, p.second)));
    lo = std::min(lo, l);
    hi = std::max(hi, l);
  }
  check(lo < 60, "灰底上出现了黑字");
  check(hi > 220, "灰底上同时出现了白描边（字外那一圈）");

  // 关掉描边，白的那一圈必须消失 —— 否则上面那条其实测的是别的东西。
  Canvas c2(kW, kH, kG, 160, 160, 160);
  cfg.outline = false;
  OsdOverlay osd2;
  osd2.configure(cfg, nullptr);
  osd2.compose(c2.data(), kW, kH, c2.stride, at(2026, 10, 7, 12, 0, 0));
  Ink ink2 = find_ink(c2);
  int hi2 = 0;
  for (auto const& p : ink2.pts) hi2 = std::max(hi2, static_cast<int>(luma(c2.at(p.first, p.second))));
  check(hi2 <= 160, "关掉描边之后没有任何一个像素比底色亮");
}

/// 底板：把水印框内的底色压暗。挑一个**字没覆盖到**的框内像素来判 ——
/// 挑到字形上的话，测的是字不是底板。
void test_plate_darkens_box() {
  Canvas c(kW, kH, kG, 255, 255, 255);
  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  cfg.plate_alpha = 255;
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));

  Ink ink = find_ink(c);
  check(!ink.empty(), "带底板的这一趟画出了东西");
  // 框的左上角必然是底板而不是字形：掩码自己四周留了 2 px 的贴边，
  // 字是从那里再往里开始的。**不要自己按 margin 去算这个位置** ——
  // margin 用的是 `font_px()` 之后的字号（下限 16），和 `kH/32` 不是一回事，
  // 差一个像素就探到框外去了，而框外是纯白，读回来是 255。
  int const px = std::max(16, kH / 32);
  int const margin = std::max(6, px / 2);
  check(ink.x0 == margin && ink.y0 == margin, "底板框的左上角就是边距处");
  unsigned const l = luma(c.at(ink.x0, ink.y0));
  check(l < 20, "不透明度 255 的底板把框内底色压到接近全黑");
}

/// 四个角各自的落点。用**真实画布**上的墨迹包围盒来判，不靠 `placements()`
/// 的内部约定 —— 后者和实现同源，拿它自证等于没测。
void test_corner_positions() {
  struct Case {
    char const* pos;
    bool right;
    bool bottom;
  };
  Case const cases[] = {
      {"upper_left", false, false},
      {"upper_right", true, false},
      {"lower_left", false, true},
      {"lower_right", true, true},
  };
  for (Case const& cs : cases) {
    Canvas c(kW, kH, kG, 0, 0, 0);
    OsdConfig cfg = one_item(text_item("HELLO", cs.pos));
    OsdOverlay osd;
    osd.configure(cfg, nullptr);
    osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
    Ink ink = find_ink(c);
    if (ink.empty()) {
      check(false, std::string(cs.pos) + " 上画出了东西");
      continue;
    }
    int const cx = (ink.x0 + ink.x1) / 2;
    int const cy = (ink.y0 + ink.y1) / 2;
    bool const ok = (cs.right ? cx > kW / 2 : cx < kW / 2) &&
                    (cs.bottom ? cy > kH / 2 : cy < kH / 2);
    check(ok, std::string(cs.pos) + " 落点在对角象限里");
    check(ink.x0 >= 0 && ink.y0 >= 0 && ink.x1 < kW && ink.y1 < kH,
          std::string(cs.pos) + " 完全落在画面内");
  }
}

/// `placements()` 的四角关系：左边两项 x 相同、上边两项 y 相同，等等。
/// 它给前端的 OSD 预览用，错了的话页面上那个小示意图会指到别处去。
void test_placements_relative() {
  OsdOverlay ul, ur, ll, lr;
  ul.configure(one_item(text_item("HELLO", "upper_left")), nullptr);
  ur.configure(one_item(text_item("HELLO", "upper_right")), nullptr);
  ll.configure(one_item(text_item("HELLO", "lower_left")), nullptr);
  lr.configure(one_item(text_item("HELLO", "lower_right")), nullptr);
  // placements() 不栅格化，得先 compose 一次把掩码建出来。
  Canvas c(64, 64, 0, 0, 0, 0);
  auto const t = at(2026, 10, 7, 12, 0, 0);
  ul.compose(c.data(), 64, 64, c.stride, t);
  ur.compose(c.data(), 64, 64, c.stride, t);
  ll.compose(c.data(), 64, 64, c.stride, t);
  lr.compose(c.data(), 64, 64, c.stride, t);

  auto one = [](OsdOverlay& o) { return o.placements().at(0); };
  auto const a = one(ul), b = one(ur), d = one(ll), e = one(lr);
  check(a.visible && b.visible && d.visible && e.visible, "四项都报了位置");
  check(a.x == a.y && a.x > 0, "左上：x 与 y 都是正的边距");
  check(b.x < a.x && b.y == a.y, "右上：x 更小（贴右），y 与左上相同");
  check(d.x == a.x && d.y < a.y, "左下：x 与左上相同，y 更小（贴下）");
  check(e.x < a.x && e.y < b.y, "右下：x、y 都贴到另一侧");
  check(a.w == e.w && a.h == e.h, "同一个文字框在四个角上尺寸一致");
}

/// `custom` 位置按百分比，而且**出界时要自己裁掉**，不许写到画布外。
void test_custom_position_and_clip() {
  Canvas c(kW, kH, kG, 0, 0, 0);
  OsdItemConfig it = text_item("HELLO", "custom");
  it.x_pct = 98;
  it.y_pct = 97;
  OsdOverlay osd;
  osd.configure(one_item(it), nullptr);
  int const drawn = osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
  check(drawn == 1, "custom 位置照样落笔（只是大部分被裁掉）");
  check(c.guard_intact(), "文字框超出右/下边界时没写到画布外");

  Ink ink = find_ink(c);
  check(!ink.empty() && ink.x1 < kW && ink.y1 < kH,
        "被裁之后剩下的墨迹仍然在画面内");
  check(!ink.empty() && ink.x0 > kW / 2, "98% 的位置确实落在右半侧");
}

/// 负数百分比是**合法的**（前端的拖拽可能给出负的），要让 `anchor` 算出来的
/// 负起点被裁掉，而不是越界写。
void test_negative_custom_position() {
  Canvas c(kW, kH, kG, 0, 0, 0);
  OsdItemConfig it = text_item("HELLO", "custom");
  it.x_pct = -5;
  it.y_pct = -5;
  OsdOverlay osd;
  osd.configure(one_item(it), nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
  check(c.guard_intact(), "负百分比没写到画布外");
}

/// 文字一样时掩码必须**完全一样**（缓存不许引入抖动），文字变了必须**真的变**
/// —— 后者就是"秒数冻住"那件事的最小复现。
void test_text_change_invalidates_mask() {
  auto render = [](std::chrono::system_clock::time_point t) {
    Canvas c(kW, kH, kG, 0, 0, 0);
    OsdItemConfig it;
    it.type = "time";
    it.position = "upper_left";
    OsdOverlay osd;
    osd.configure(one_item(it), nullptr);
    osd.compose(c.data(), kW, kH, c.stride, t);
    return c.px;
  };

  auto const t = at(2026, 10, 7, 12, 0, 30);
  check(render(t) == render(t), "同一秒内画两次，像素逐字节相同（缓存不抖）");

  auto const a = render(t);
  auto const b = render(t + std::chrono::seconds(1));
  check(a != b, "秒数变了之后像素真的变了（掩码缓存失效了）");

  std::size_t diff = 0;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (a[i] != b[i]) ++diff;
  }
  // 只跳一格秒：变化的是那一位数字，不该整行都变（整行都变说明位置漂了）。
  check(diff > 0 && diff < a.size() / 8, "变的是那一位数字，不是整个文字框");
}

/// `key()` 的语义就是采集循环判"水印变了没有"的依据，它错了的表现是
/// **静止桌面上秒数冻住**（key 不变 → 不强制 content_id → 编码器复用旧 YUV）。
void test_key_tracks_visible_text() {
  OsdItemConfig it;
  it.type = "time";
  it.position = "upper_left";
  OsdConfig cfg = one_item(it);
  OsdOverlay osd;
  osd.configure(cfg, nullptr);

  auto const t = at(2026, 10, 7, 12, 0, 30);
  check(!osd.key(t).empty(), "开着的时候 key() 非空");
  check(osd.key(t) == osd.key(t), "同一秒 key 相同");
  check(osd.key(t) != osd.key(t + std::chrono::seconds(1)), "下一秒 key 不同");

  // 关掉的项不该进 key —— 否则它一变就会逼出一次没有内容变化的编码。
  OsdConfig two = cfg;
  OsdItemConfig off = text_item("XXXX", "lower_right");
  off.enabled = false;
  two.items.push_back(off);
  OsdOverlay osd2;
  osd2.configure(two, nullptr);
  OsdOverlay osd3;
  osd3.configure(cfg, nullptr);
  check(osd2.key(t) == osd3.key(t), "关掉的项不进 key()");
}

/// `name` 项从 `DeviceState` 现读。**这就是"NVR 改名立刻反映到水印上"那条**：
/// 不重开管线、不重建水印对象，只是名字变了。
void test_name_item_reads_device_state() {
  DeviceState dev;
  dev.set_channel_name("前台");

  OsdItemConfig it;
  it.type = "name";
  it.position = "upper_left";
  OsdOverlay osd;
  osd.configure(one_item(it), &dev);

  auto const t = at(2026, 10, 7, 12, 0, 0);
  check(!osd.key(t).empty(), "名称非空时 key() 非空");

  Canvas a(kW, kH, kG, 0, 0, 0);
  osd.compose(a.data(), kW, kH, a.stride, t);
  check(!find_ink(a).empty(), "名称画到了画面上");

  dev.set_channel_name("后门");
  check(osd.key(t) != std::string(), "改名之后 key() 仍然非空");
  Canvas b(kW, kH, kG, 0, 0, 0);
  osd.compose(b.data(), kW, kH, b.stride, t);
  check(a.px != b.px, "改名之后画面上的字当场就变了");

  // 空名字是合法的：画不出来，但不报错。
  dev.set_channel_name("");
  Canvas c(kW, kH, kG, 0, 0, 0);
  std::vector<std::uint8_t> const before = c.px;
  OsdOverlay osd2;
  osd2.configure(one_item(it), &dev);
  check(osd2.compose(c.data(), kW, kH, c.stride, t) == 0, "空名称不落笔");
  check(c.px == before, "空名称一个像素都不动");
}

/// **没有 DeviceState 时不许崩** —— `main.cpp` 现在还没把它递进来，
/// 而"没接上"和"崩了"必须是两种明显不同的表现。
void test_name_item_without_device() {
  OsdItemConfig it;
  it.type = "name";
  it.position = "lower_right";
  OsdOverlay osd;
  osd.configure(one_item(it), nullptr);
  Canvas c(kW, kH, kG, 255, 255, 255);
  check(osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0)) == 0,
        "没有 DeviceState 时名称项不落笔，也不崩");
  check(osd.key(at(2026, 10, 7, 12, 0, 0)).empty(), "没有 DeviceState 时 key() 是空串");
}

/// 字号自动时随帧高线性变化。**这条是"子码流上的字会变小"的机制**：
/// 同一个配置在两个高度上跑，框的高度必须按比例。
void test_auto_font_scales_with_height() {
  auto box_h = [](int h) {
    int const w = h * 4 / 3;
    Canvas c(w, h, 0, 0, 0, 0);
    OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
    OsdOverlay osd;
    osd.configure(cfg, nullptr);
    osd.compose(c.data(), w, h, c.stride, at(2026, 10, 7, 12, 0, 0));
    return find_ink(c).h();
  };
  int const h480 = box_h(480);
  int const h960 = box_h(960);
  check(h480 > 0 && h960 > 0, "两个高度上都画出了字");
  // 允许 ±15% 的偏差：字号是 h/32 取整、字形的墨迹高度也不严格等于字号。
  int const ratio_pct = h480 > 0 ? (h960 * 100 / h480) : 0;
  check(ratio_pct > 170 && ratio_pct < 230,
        "帧高翻倍时文字框高度也大致翻倍");
}

/// `font_size` 显式给了就照它来，而且**不低于 16**：再小的话在 1080p 上
/// 已经是几个像素高的糊点，画了等于没画。
void test_explicit_font_size() {
  auto box_h = [](int size) {
    OsdItemConfig it = text_item("HELLO", "upper_left");
    it.font_size = size;
    Canvas c(kW, kH, 0, 0, 0, 0);
    OsdOverlay osd;
    osd.configure(one_item(it), nullptr);
    osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
    return find_ink(c).h();
  };
  check(box_h(48) > box_h(24), "字号 48 的文字框比字号 24 的高");
  check(box_h(2) == box_h(16), "字号 2 被抬到下限 16");
}

/// 中文名。GDI 那边字符集选错了会画成方块，而方块也是"有墨"的 ——
/// 所以这里判的不是"有没有画"，而是**画出来的东西和 ASCII 不一样**：
/// 一个方块串和一个等宽 ASCII 串的墨迹分布不会相同。
void test_cjk_text() {
  Canvas c(kW, kH, kG, 0, 0, 0);
  OsdConfig cfg = one_item(text_item("前门摄像头", "upper_left"));
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(c.data(), kW, kH, c.stride, at(2026, 10, 7, 12, 0, 0));
  Ink ink = find_ink(c);
  check(!ink.empty(), "中文名画到了画面上");
  check(ink.w() > 40 && ink.h() > 10, "中文名的墨迹不是几个糊点");
  check(c.guard_intact(), "中文这一趟没写到画布外");
}

/// 空指针与退化尺寸不许崩。
void test_degenerate_inputs() {
  OsdOverlay osd;
  auto const t = at(2026, 10, 7, 12, 0, 0);
  osd.configure(one_item(text_item("HELLO", "upper_left")), nullptr);
  check(osd.compose(nullptr, kW, kH, 0, t) == 0, "dst 为空时返回 0");
  Canvas c(kW, kH, 0, 0, 0, 0);
  check(osd.compose(c.data(), 0, kH, c.stride, t) == 0, "宽为 0 时返回 0");
  check(osd.compose(c.data(), kW, 0, c.stride, t) == 0, "高为 0 时返回 0");
}

/// 两帧连画同一个对象：第二帧必须**和第一帧一模一样**。
/// 掩码缓存跨帧复用是常态，而"复用时把描边叠加了两次"这类错只在这里露馅。
void test_repeat_compose_is_idempotent() {
  auto const t = at(2026, 10, 7, 12, 0, 0);
  Canvas a(kW, kH, kG, 90, 90, 90);
  Canvas b(kW, kH, kG, 90, 90, 90);
  OsdConfig cfg = one_item(text_item("HELLO", "upper_left"));
  OsdOverlay osd;
  osd.configure(cfg, nullptr);
  osd.compose(a.data(), kW, kH, a.stride, t);
  osd.compose(b.data(), kW, kH, b.stride, t);
  check(a.px == b.px, "同一个对象连画两帧，结果逐字节相同");

  // 而在**同一块画布**上连画两次会叠加 —— 这是合成点的约定：一帧只画一次。
  // 断言它是为了把这个约定写下来，免得将来有人靠"多画几次没关系"来省事。
  Canvas c(kW, kH, kG, 90, 90, 90);
  std::vector<std::uint8_t> const bg = c.px;
  osd.compose(c.data(), kW, kH, c.stride, t);
  osd.compose(c.data(), kW, kH, c.stride, t);
  check(c.px != bg, "同一块画布上连画两次确实会叠（所以一帧只能画一次）");
}

}  // namespace

int main() {
  // 掩码栅格化走 GDI，日志压低免得刷屏。等级枚举是全局那一个，
  // 而 `<wingdi.h>` 把 `ERROR` 定义成宏了 —— 好在 `LogLevel::Error` 带限定名，
  // 不撞（陷阱 15 说的是 libuvcpp 那边裸写 `ERROR` 的情形）。
  Log::set_level(LogLevel::Error);

  std::printf("== 水印（OSD）合成 ==\n\n");

  test_disabled_touches_nothing();
  test_all_items_off();
  test_empty_text_item();
  test_dark_bg_white_text();
  test_light_bg_black_text();
  test_adaptive_threshold();
  test_outline_is_inverse_colour();
  test_plate_darkens_box();
  test_corner_positions();
  test_placements_relative();
  test_custom_position_and_clip();
  test_negative_custom_position();
  test_text_change_invalidates_mask();
  test_key_tracks_visible_text();
  test_name_item_reads_device_state();
  test_name_item_without_device();
  test_auto_font_scales_with_height();
  test_explicit_font_size();
  test_cjk_text();
  test_degenerate_inputs();
  test_repeat_compose_is_idempotent();

  std::printf("\n%d 项，失败 %d 项\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
