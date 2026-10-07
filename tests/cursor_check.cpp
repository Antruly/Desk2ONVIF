/**
 * @file tests/cursor_check.cpp
 * @brief 指针合成的三条像素规则，逐像素钉死。
 *
 * 为什么要在没有 DXGI 的情况下单独测这个：**合成规则错了，画面上的表现是
 * "一片花花绿绿的东西"或者"指针缺一块"**，而这两件事都会被归到"编码坏了"
 * 上去查。单色指针那条尤其危险 —— 位序反了是左右镜像、AND/XOR 顺序反了是
 * 只剩黑白两色，两种都只在单色指针上出现，而彩色指针完全正常，于是很容易
 * 被当成"某个用户环境的问题"。
 *
 * 这里不碰 DXGI，也不碰窗口：直接把形状和画布喂进去，读回来的像素和手算的
 * 值逐字节比。单色那四种组合是**从 Windows 的 DDB 语义推出来的**
 * （先 SRCAND 再 SRCINVERT），推导过程写在 cursor_overlay.h 里。
 *
 * 编译（不需要任何第三方库）：
 *   g++ -std=c++17 -Isrc tests/cursor_check.cpp src/capture/cursor_overlay.cpp \
 *       -o build/cursor_check.exe
 */

#include "capture/cursor_overlay.h"
#include "common/log.h"

#include <cstdint>
#include <cstdio>
#include <string>
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

/// 8x8 的 BGRA 画布，底色给定。
struct Canvas {
  int w = 8;
  int h = 8;
  std::vector<std::uint8_t> px;

  Canvas(std::uint8_t b, std::uint8_t g, std::uint8_t r, std::uint8_t a = 255)
      : px(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u) {
    for (std::size_t i = 0; i < px.size(); i += 4) {
      px[i] = b;
      px[i + 1] = g;
      px[i + 2] = r;
      px[i + 3] = a;
    }
  }

  std::uint8_t const* at(int x, int y) const {
    return px.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                        static_cast<std::size_t>(x)) * 4u;
  }
  bool is(int x, int y, std::uint8_t b, std::uint8_t g, std::uint8_t r) const {
    std::uint8_t const* p = at(x, y);
    return p[0] == b && p[1] == g && p[2] == r;
  }
  std::string dump(int x, int y) const {
    std::uint8_t const* p = at(x, y);
    return fmt("(%u,%u,%u,%u)", p[0], p[1], p[2], p[3]);
  }
};

CursorShape color_shape(int w, int h, std::vector<std::uint8_t> bits,
                        int hot_x = 0, int hot_y = 0,
                        CursorShapeType type = CursorShapeType::Color) {
  CursorShape s;
  s.type = type;
  s.width = w;
  s.height = h;
  s.pitch = w * 4;
  s.hotspot_x = hot_x;
  s.hotspot_y = hot_y;
  s.bits = std::move(bits);
  return s;
}

/// 单色形状：`and_row` / `xor_row` 各一行（宽度必须是 8 的倍数，这里就是 8）。
CursorShape mono_shape(std::uint8_t and_row, std::uint8_t xor_row) {
  CursorShape s;
  s.type = CursorShapeType::Monochrome;
  s.width = 8;
  s.height = 2;  // 上半 AND，下半 XOR —— 不是两行像素
  s.pitch = 1;
  s.hotspot_x = 0;
  s.hotspot_y = 0;
  s.bits = {and_row, xor_row};
  return s;
}

// ---------------------------------------------------------------------------

void test_color_alpha() {
  // 一个 2x2 的彩色指针：左上全不透明、右上半透明、左下全透明、右下全不透明。
  std::vector<std::uint8_t> bits = {
      0x10, 0x20, 0x30, 0xFF,   // (0,0) B,G,R,A
      0xC8, 0x00, 0x00, 0x80,   // (1,0) a=128
      0x00, 0x00, 0x00, 0x00,   // (0,1) 全透明
      0x01, 0x02, 0x03, 0xFF,   // (1,1)
  };
  Canvas c(10, 20, 30);  // 底色 B=10 G=20 R=30
  CursorOverlay ov;
  ov.set_shape(color_shape(2, 2, bits));
  ov.set_state(2, 2, true);
  ov.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);

  check(c.is(2, 2, 0x10, 0x20, 0x30), "彩色：alpha=255 的像素原样覆盖");
  check(c.is(3, 3, 0x01, 0x02, 0x03), "彩色：右下角也落到位");
  // a=128：B=(200*128 + 10*127 + 127)/255 = 105；G=(0 + 20*127 + 127)/255 = 10；
  //       R=(0 + 30*127 + 127)/255 = 15
  bool const half = c.is(3, 2, 105, 10, 15);
  check(half, "彩色：alpha=128 按比例混合（手算值 105,10,15）—— 实际 " +
                  c.dump(3, 2));
  check(c.is(2, 3, 10, 20, 30), "彩色：alpha=0 的像素一个字节都不动");
  check(c.is(0, 0, 10, 20, 30) && c.is(4, 4, 10, 20, 30),
        "彩色：形状以外的区域不受影响");
}

void test_monochrome_four_rules() {
  // AND=0b10101010（MSB 在左）→ [1,0,1,0,1,0,1,0]
  // XOR=0b11001100             → [1,1,0,0,1,1,0,0]
  // 四种组合正好各出现两次。
  Canvas c(10, 20, 30);
  CursorOverlay ov;
  ov.set_shape(mono_shape(0xAA, 0xCC));
  ov.set_state(0, 0, true);
  ov.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);

  check(c.is(0, 0, static_cast<std::uint8_t>(~10), static_cast<std::uint8_t>(~20),
             static_cast<std::uint8_t>(~30)),
        "单色：AND=1 XOR=1 → 反相（I 形光标压在文字上用的就是这一档）");
  check(c.is(1, 0, 0xFF, 0xFF, 0xFF), "单色：AND=0 XOR=1 → 白");
  check(c.is(2, 0, 10, 20, 30), "单色：AND=1 XOR=0 → 保持背景（透明）");
  check(c.is(3, 0, 0x00, 0x00, 0x00), "单色：AND=0 XOR=0 → 黑");
  check(c.is(4, 0, static_cast<std::uint8_t>(~10), static_cast<std::uint8_t>(~20),
             static_cast<std::uint8_t>(~30)),
        "单色：色位序是高位在左（第 4 位又是反相）");
  check(c.is(5, 0, 0xFF, 0xFF, 0xFF), "单色：第 5 位是白");
  check(c.is(6, 0, 10, 20, 30), "单色：第 6 位保持背景");
  check(c.is(7, 0, 0x00, 0x00, 0x00), "单色：第 7 位是黑");
  check(c.is(0, 1, 10, 20, 30),
        "单色：形状只有 1 行像素（height=2 是 AND+XOR 两半，不是两行）");
}

void test_masked_color() {
  // alpha=0 → 反相；alpha=255 → 覆盖；两者都不是"透明"。
  std::vector<std::uint8_t> bits = {
      0x11, 0x22, 0x33, 0x00,
      0xAA, 0xBB, 0xCC, 0xFF,
  };
  Canvas c(10, 20, 30);
  CursorOverlay ov;
  ov.set_shape(color_shape(2, 1, bits, 0, 0, CursorShapeType::MaskedColor));
  ov.set_state(0, 0, true);
  ov.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);

  check(c.is(0, 0, static_cast<std::uint8_t>(10 ^ 0x11),
             static_cast<std::uint8_t>(20 ^ 0x22),
             static_cast<std::uint8_t>(30 ^ 0x33)),
        "掩码色：alpha=0 是**反相**，不是透明 —— 白底上的白指针靠它才看得见");
  check(c.is(1, 0, 0xAA, 0xBB, 0xCC), "掩码色：alpha=255 正常覆盖");
}

void test_hotspot_and_clipping() {
  // hotspot=(1,1) 表示"形状的 (1,1) 像素落在鼠标位置上"。鼠标在 (0,0) 时，
  // 形状左上角被推到 (-1,-1)：只有右下三个像素应该落进画布。
  std::vector<std::uint8_t> bits(2 * 2 * 4, 0xFF);
  Canvas c(10, 20, 30);
  CursorOverlay ov;
  ov.set_shape(color_shape(2, 2, bits, /*hot_x=*/1, /*hot_y=*/1));
  ov.set_state(0, 0, true);
  ov.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);

  check(c.is(0, 0, 0xFF, 0xFF, 0xFF), "hotspot：形状的 (1,1) 落在鼠标位置上");
  check(c.is(1, 0, 10, 20, 30), "hotspot：右边没有被越界写到");
  check(c.is(0, 1, 10, 20, 30), "hotspot：下边没有被越界写到");

  // 完全跑到画布外：一个字节都不能碰，更不能越界写。
  Canvas c2(10, 20, 30);
  CursorOverlay ov2;
  ov2.set_shape(color_shape(2, 2, bits, 100, 100));
  ov2.set_state(0, 0, true);
  ov2.compose(c2.px.data(), c2.w, c2.h, static_cast<std::size_t>(c2.w) * 4u);
  bool untouched = true;
  for (int y = 0; y < c2.h; ++y) {
    for (int x = 0; x < c2.w; ++x) {
      if (!c2.is(x, y, 10, 20, 30)) untouched = false;
    }
  }
  check(untouched, "hotspot：整个形状落在画布外时不写任何像素");
}

void test_visibility_and_change_detection() {
  CursorOverlay ov;
  check(ov.set_state(1, 2, true), "首次报状态算“变了”（否则第一帧不会合成）");
  check(!ov.set_state(1, 2, true), "位置和可见性都没变 → 不算变");
  check(ov.set_state(1, 3, true), "位置变了 → 算变");
  check(ov.set_state(1, 3, false), "可见性变了 → 算变");

  ov.set_shape(mono_shape(0xFF, 0x00));
  check(!ov.drawable(), "不可见时不画（隐藏指针要能把上一次的擦掉）");

  Canvas c(10, 20, 30);
  ov.set_state(0, 0, false);
  ov.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);
  check(c.is(0, 0, 10, 20, 30), "不可见时 compose 是空操作");

  CursorOverlay no_shape;
  no_shape.set_state(0, 0, true);
  check(!no_shape.drawable(), "形状还没取到时不画");
  no_shape.compose(c.px.data(), c.w, c.h, static_cast<std::size_t>(c.w) * 4u);
  check(c.is(0, 0, 10, 20, 30), "没有形状时 compose 是空操作");
}

void test_shape_validation() {
  CursorShape s = color_shape(2, 2, {1, 2, 3, 4});
  check(!s.valid(), "形状缓冲比 width*height*pitch 短 → 判为不可用（否则越界读）");
  CursorShape ok = color_shape(2, 2, std::vector<std::uint8_t>(16, 0x7F));
  check(ok.valid(), "长度对得上的形状可用");
  check(ok.pixel_height() == 2, "彩色形状的实际高度就是 height");
  CursorShape m = mono_shape(0xFF, 0x00);
  check(m.pixel_height() == 1, "单色形状的实际高度是 height/2");
  check(m.valid(), "单色形状 2 字节就够（一行 AND + 一行 XOR）");
}

}  // namespace

int main() {
  std::printf("指针合成规则检查\n----------------\n");
  test_color_alpha();
  test_monochrome_four_rules();
  test_masked_color();
  test_hotspot_and_clipping();
  test_visibility_and_change_detection();
  test_shape_validation();
  std::printf("----------------\n%d 项检查，%d 项失败。\n", g_checks, g_failed);
  return g_failed == 0 ? 0 : 1;
}
