#include "capture/osd_overlay.h"

#include "common/device_state.h"
#include "common/text.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace d2o {

namespace {

/// 掩码四周留的边，给描边膨胀用。少了它，贴边那一列的描边会被裁掉。
constexpr int kHaloPad = 2;

/// 字号自动时的除数：帧高 / 32。1920 高 → 60 px。
constexpr int kAutoFontDiv = 32;
constexpr int kMinFontPx = 16;

/// 自动配色时判定"亮底还是暗底"的分界。
constexpr unsigned kDarkBelow = 128;

/// 一张文字覆盖率掩码。
///
/// `cov` 是文字的覆盖率，`ring` 是**膨胀一圈再减掉文字本身** —— 只有 ring
/// 才能当描边用。不减去文字的话，半透明的抗锯齿边上描边和文字会叠在同一像素
/// 上，得到一圈比字还深的糊边。
struct Mask {
  int w = 0;
  int h = 0;
  std::vector<std::uint8_t> cov;
  std::vector<std::uint8_t> ring;
  bool empty() const { return w <= 0 || h <= 0 || cov.empty(); }
};

/// 按覆盖率把颜色混进一个 BGRA 像素。和 CursorOverlay 里那个同式。
inline void blend_px(std::uint8_t* p, std::uint8_t b, std::uint8_t g, std::uint8_t r,
                     unsigned a) {
  if (a == 0) return;
  unsigned const ia = 255u - a;
  p[0] = static_cast<std::uint8_t>((static_cast<unsigned>(b) * a +
                                    static_cast<unsigned>(p[0]) * ia + 127u) / 255u);
  p[1] = static_cast<std::uint8_t>((static_cast<unsigned>(g) * a +
                                    static_cast<unsigned>(p[1]) * ia + 127u) / 255u);
  p[2] = static_cast<std::uint8_t>((static_cast<unsigned>(r) * a +
                                    static_cast<unsigned>(p[2]) * ia + 127u) / 255u);
}

struct Rgb {
  std::uint8_t b, g, r;
};

constexpr Rgb kWhite{255, 255, 255};
constexpr Rgb kBlack{0, 0, 0};

/// 用 GDI 把一行文字栅格成 8 位覆盖率。
///
/// 绕开 GDI 的 alpha 混合：建一张 32bpp 的 DIB、铺纯黑，用纯白把字画上去，
/// 再把 **R 通道**当覆盖率读回来。字体的 ANTIALIASED_QUALITY 提供了软边，
/// 于是读回来的就是一张现成的抗锯齿掩码 —— 不用自己写字形光栅化。
///
/// 三个必须记住的点：
///   - **高度取负**。正数被 GDI 当成"字符单元高度"（含内部行距），负数才是
///     字形高度；而且只有负高度才让 DIB 变成自上而下 —— 用正数的话按行读回来
///     的字是**上下颠倒**的。
///   - **必须 `SetBkMode(TRANSPARENT)`**。默认的 OPAQUE 会在整块 DIB 上刷一遍
///     背景色，读回来是一条实心带而不是字形。
///   - **读之前 `GdiFlush()`**。GDI 的绘制是批处理的，不刷就可能在它落盘之前
///     读到全黑的 DIB —— 表现为"水印时有时无"，取决于机器快慢。
bool rasterize(std::string const& utf8, int px, Mask& out) {
  out = Mask{};
  std::wstring const text = from_utf8(utf8);
  if (text.empty() || px <= 0) return false;

  HDC dc = CreateCompatibleDC(nullptr);
  if (dc == nullptr) return false;

  // "Microsoft YaHei UI" 在 Win10/11 上都在（中英文 SKU 都有）。DEFAULT_CHARSET
  // 让 GDI 按字符实际所属的字符集选字形，中文名才不会变成方块。
  HFONT font = CreateFontW(
      -px, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
      OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
  if (font == nullptr) {
    DeleteDC(dc);
    return false;
  }
  HGDIOBJ const old_font = SelectObject(dc, font);

  SIZE sz{};
  bool ok = false;
  HBITMAP bmp = nullptr;
  if (GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &sz) &&
      sz.cx > 0 && sz.cy > 0) {
    int const mw = sz.cx + kHaloPad * 2;
    int const mh = sz.cy + kHaloPad * 2;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = mw;
    bmi.bmiHeader.biHeight = -mh;  // 负 = 自上而下
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    bmp = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bmp != nullptr && bits != nullptr) {
      HGDIOBJ const old_bmp = SelectObject(dc, bmp);
      std::memset(bits, 0, static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh) * 4);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(255, 255, 255));
      TextOutW(dc, kHaloPad, kHaloPad, text.c_str(), static_cast<int>(text.size()));
      GdiFlush();

      out.w = mw;
      out.h = mh;
      out.cov.assign(static_cast<std::size_t>(mw) * static_cast<std::size_t>(mh), 0);
      auto const* p = static_cast<std::uint8_t const*>(bits);
      for (int y = 0; y < mh; ++y) {
        for (int x = 0; x < mw; ++x) {
          // BGRA：R 在 +2。白字黑底 ⇒ R 就是覆盖率。
          std::size_t const si = (static_cast<std::size_t>(y) * mw + x) * 4 + 2;
          out.cov[static_cast<std::size_t>(y) * mw + x] = p[si];
        }
      }
      SelectObject(dc, old_bmp);
      ok = true;
    }
  }

  if (bmp != nullptr) DeleteObject(bmp);
  SelectObject(dc, old_font);
  DeleteObject(font);
  DeleteDC(dc);
  if (!ok) return false;

  int const mw = out.w;
  int const mh = out.h;
  out.ring.assign(out.cov.size(), 0);
  for (int y = 0; y < mh; ++y) {
    for (int x = 0; x < mw; ++x) {
      std::uint8_t m = 0;
      for (int dy = -1; dy <= 1 && m != 255; ++dy) {
        int const ny = y + dy;
        if (ny < 0 || ny >= mh) continue;
        for (int dx = -1; dx <= 1; ++dx) {
          int const nx = x + dx;
          if (nx < 0 || nx >= mw) continue;
          m = std::max(m, out.cov[static_cast<std::size_t>(ny) * mw + nx]);
        }
      }
      std::size_t const i = static_cast<std::size_t>(y) * mw + x;
      out.ring[i] = m > out.cov[i] ? static_cast<std::uint8_t>(m - out.cov[i]) : 0;
    }
  }
  return true;
}

/// 把 strftime 格式跑一遍，失败就返回空（格式写错时不要输出半个日期）。
std::string format_time(std::string const& pattern, std::tm const& tmv) {
  if (pattern.empty()) return std::string();
  char buf[128];
  std::size_t const n = std::strftime(buf, sizeof(buf), pattern.c_str(), &tmv);
  return n == 0 ? std::string() : std::string(buf, n);
}

/// 解析一项此刻该显示的文字。
std::string item_text(OsdItemConfig const& it, DeviceState const* dev,
                      std::chrono::system_clock::time_point wall) {
  if (it.type == "name") {
    return dev == nullptr ? std::string() : dev->channel_name();
  }
  if (it.type == "text") return it.text;

  // `time`：显示的是**本地钟面**上的设备时间。设备时间本身是"主机时刻 + NVR
  // 下发的偏移"，所以这里 localtime 一下得到的就是摄像机该显示的那个钟面。
  std::tm const tmv =
      local_time(std::chrono::system_clock::to_time_t(wall));
  std::string const d = format_time(it.date_format, tmv);
  std::string const t = format_time(it.time_format, tmv);
  if (d.empty()) return t;
  if (t.empty()) return d;
  return d + " " + t;
}

int font_px(OsdItemConfig const& it, int dst_h) {
  int px = it.font_size > 0 ? it.font_size : dst_h / kAutoFontDiv;
  return std::max(kMinFontPx, px);
}

/// 这一项在画面上的左上角。`custom` 用百分比，其余四个角对称地贴边。
struct Origin {
  int x, y;
};

Origin anchor(OsdItemConfig const& it, int dst_w, int dst_h, int mw, int mh,
              int px) {
  int const margin = std::max(6, px / 2);
  if (it.position == "custom") {
    return Origin{static_cast<int>(static_cast<long>(dst_w) * it.x_pct / 100),
                  static_cast<int>(static_cast<long>(dst_h) * it.y_pct / 100)};
  }
  int const x = it.position == "upper_right" || it.position == "lower_right"
                    ? dst_w - mw - margin
                    : margin;
  int const y = it.position == "lower_left" || it.position == "lower_right"
                    ? dst_h - mh - margin
                    : margin;
  return Origin{x, y};
}

/// 目标区域的平均亮度（BT.601 整数权重）。按步长取样，几十乘几十个点就够 ——
/// 这里要回答的只是"这块背景是亮的还是暗的"，不需要精确均值。
unsigned mean_luma(std::uint8_t const* dst, int dst_w, int dst_h, std::size_t stride,
                   int ox, int oy, int mw, int mh) {
  std::uint64_t sum = 0;
  std::uint64_t n = 0;
  int const step = std::max(1, std::min(mw, mh) / 24);
  for (int y = oy; y < oy + mh; y += step) {
    if (y < 0 || y >= dst_h) continue;
    std::uint8_t const* row = dst + static_cast<std::size_t>(y) * stride;
    for (int x = ox; x < ox + mw; x += step) {
      if (x < 0 || x >= dst_w) continue;
      std::uint8_t const* p = row + static_cast<std::size_t>(x) * 4;
      sum += (299u * p[2] + 587u * p[1] + 114u * p[0]) / 1000u;
      ++n;
    }
  }
  return n == 0 ? 0u : static_cast<unsigned>(sum / n);
}

}  // namespace

struct OsdOverlay::Item {
  OsdItemConfig cfg;
  Mask mask;
  std::string cached_text;
  int cached_px = -1;

  /// 文字或字号变了才重新栅格。**失败也要记下**，否则空文字会每帧重试一次
  /// 完整的 GDI 绘制流程（每帧几毫秒，@30fps 就是白白烧掉十几个百分点的 CPU）。
  void ensure(std::string const& text, int px) {
    if (text == cached_text && px == cached_px) return;
    cached_text = text;
    cached_px = px;
    if (!rasterize(text, px, mask)) mask = Mask{};
  }
};

OsdOverlay::OsdOverlay() = default;
OsdOverlay::~OsdOverlay() = default;

void OsdOverlay::configure(OsdConfig const& cfg, DeviceState const* device) {
  cfg_ = cfg;
  device_ = device;
  items_.clear();
  items_.reserve(cfg_.items.size());
  for (auto const& it : cfg_.items) {
    Item one;
    one.cfg = it;
    items_.push_back(std::move(one));  // 缓存刻意不保留：配置变了就重建
  }
}

bool OsdOverlay::enabled() const {
  if (!cfg_.enabled) return false;
  for (auto const& it : items_) {
    if (it.cfg.enabled) return true;
  }
  return false;
}

std::string OsdOverlay::key(std::chrono::system_clock::time_point wall) const {
  if (!enabled()) return std::string();
  std::string k;
  for (auto const& it : items_) {
    if (!it.cfg.enabled) continue;
    std::string const t = item_text(it.cfg, device_, wall);
    // **解析不出文字的项不进指纹**，否则它们会贡献一个恒定的分隔符，于是
    // "什么都没画"的 key 变成一个非空串，而调用方要判的正是"有没有东西可画"。
    // 触发它的现实情形是 `name` 项而 `DeviceState` 还没接上（或者名字是空的）
    // —— 那一刻 key() 必须为空，才能和"水印画得出东西"区分开。
    if (t.empty()) continue;
    if (!k.empty()) k.push_back('\x1f');  // 分隔符：不能和文字本身撞
    k += t;
  }
  return k;
}

int OsdOverlay::compose(std::uint8_t* dst, int dst_w, int dst_h, std::size_t stride,
                        std::chrono::system_clock::time_point wall) {
  if (dst == nullptr || dst_w <= 0 || dst_h <= 0) return 0;
  if (!enabled()) return 0;

  int drawn = 0;
  for (auto& item : items_) {
    if (!item.cfg.enabled) continue;

    int const px = font_px(item.cfg, dst_h);
    item.ensure(item_text(item.cfg, device_, wall), px);
    if (item.mask.empty()) continue;

    Mask const& m = item.mask;
    Origin const o = anchor(item.cfg, dst_w, dst_h, m.w, m.h, px);

    // 自适应配色：黑底写白字、白底写黑字。**每帧重算**而不是每次文字变化时算
    // —— 背景是画面的一部分，它会变（窗口一拖过去，同一个位置就从亮变暗了）。
    Rgb fg = kWhite;
    if (cfg_.adaptive_color) {
      unsigned const luma = mean_luma(dst, dst_w, dst_h, stride, o.x, o.y, m.w, m.h);
      fg = luma < kDarkBelow ? kWhite : kBlack;
    }
    Rgb const halo = (fg.r == 255) ? kBlack : kWhite;
    unsigned const plate_a =
        static_cast<unsigned>(std::clamp(cfg_.plate_alpha, 0, 255));

    for (int my = 0; my < m.h; ++my) {
      int const dy = o.y + my;
      if (dy < 0 || dy >= dst_h) continue;
      std::uint8_t* row = dst + static_cast<std::size_t>(dy) * stride;
      for (int mx = 0; mx < m.w; ++mx) {
        int const dx = o.x + mx;
        if (dx < 0 || dx >= dst_w) continue;
        std::size_t const mi = static_cast<std::size_t>(my) * m.w + mx;
        std::uint8_t* p = row + static_cast<std::size_t>(dx) * 4;

        // 顺序：底板 → 描边 → 文字。反过来描边会把字盖住。
        if (plate_a > 0) blend_px(p, 0, 0, 0, plate_a);
        if (cfg_.outline) {
          unsigned const a = std::min<unsigned>(m.ring[mi], 220);
          if (a > 0) blend_px(p, halo.b, halo.g, halo.r, a);
        }
        if (m.cov[mi] > 0) blend_px(p, fg.b, fg.g, fg.r, m.cov[mi]);
      }
    }
    ++drawn;
  }
  return drawn;
}

std::vector<OsdOverlay::Placement> OsdOverlay::placements() const {
  std::vector<Placement> out;
  out.reserve(items_.size());
  for (auto const& item : items_) {
    Placement p;
    if (item.cfg.enabled && !item.mask.empty()) {
      // 位置要用**当时的帧高**算字号，而这里没有帧高 —— 用掩码自己的高度反推
      // 不到字号。所以 anchor() 需要 px，而 px 只影响 margin；用掩码高度减去
      // 两倍留边作为字号的近似（GetTextExtent 的高度≈字形高度，差得很少）。
      int const px = std::max(kMinFontPx, item.mask.h - kHaloPad * 2);
      // dst 用掩码自己的尺寸 → 位置只有 margin 那一项有意义。调用方真正要
      // 断言的是"宽高"和"相对角"，所以这里给出相对量。
      Origin const o = anchor(item.cfg, 0, 0, item.mask.w, item.mask.h, px);
      p.w = item.mask.w;
      p.h = item.mask.h;
      p.x = o.x;
      p.y = o.y;
      p.visible = true;
    }
    out.push_back(p);
  }
  return out;
}

}  // namespace d2o
