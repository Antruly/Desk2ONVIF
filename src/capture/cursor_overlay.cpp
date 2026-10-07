#include "capture/cursor_overlay.h"

#include "common/log.h"

namespace d2o {

namespace {

/// 按 alpha 混合一个 BGRA 像素。a 取 0..255。
///
/// 用 `(src*a + dst*(255-a) + 127) / 255` 而不是移位近似：后者的误差在
/// 半透明边缘上会累积成可见的色边，而这个式子只多一次加法和一次整数除法。
inline void blend(std::uint8_t* p, std::uint8_t b, std::uint8_t g, std::uint8_t r,
                  unsigned a) {
  unsigned const ia = 255u - a;
  p[0] = static_cast<std::uint8_t>((static_cast<unsigned>(b) * a +
                                    static_cast<unsigned>(p[0]) * ia + 127u) / 255u);
  p[1] = static_cast<std::uint8_t>((static_cast<unsigned>(g) * a +
                                    static_cast<unsigned>(p[1]) * ia + 127u) / 255u);
  p[2] = static_cast<std::uint8_t>((static_cast<unsigned>(r) * a +
                                    static_cast<unsigned>(p[2]) * ia + 127u) / 255u);
}

/// 1bpp 掩码里第 n 位的值。GDI 的位序是**高位在左** —— 按低位取会得到一张
/// 水平镜像的指针，而且只在单色指针上出现（彩色指针不走这条路），
/// 很容易被当成"某个驱动的问题"。
inline bool mask_bit(std::uint8_t const* row, int n) {
  return (row[n >> 3] >> (7 - (n & 7))) & 1u;
}

}  // namespace

bool CursorOverlay::set_state(int x, int y, bool visible) {
  bool const changed = !have_state_ || x != x_ || y != y_ || visible != visible_;
  x_ = x;
  y_ = y;
  visible_ = visible;
  have_state_ = true;
  return changed;
}

void CursorOverlay::compose(std::uint8_t* dst, int dst_w, int dst_h,
                            std::size_t stride) const {
  if (!drawable() || dst == nullptr) return;

  // hotspot 是"指针的哪个像素落在鼠标位置上"，减掉它才是形状的左上角。
  int const left = x_ - shape_.hotspot_x;
  int const top = y_ - shape_.hotspot_y;

  // 单色形状里 AND 和 XOR 是**上下叠着**的两半，所以循环只跑一半的行数，
  // 另一半按偏移去取。彩色形状两半是一回事，直接用 height。
  bool const mono = shape_.type == CursorShapeType::Monochrome;
  int const rows = mono ? shape_.height / 2 : shape_.height;
  if (rows <= 0) return;

  for (int sy = 0; sy < rows; ++sy) {
    int const dy = top + sy;
    if (dy < 0 || dy >= dst_h) continue;

    std::uint8_t const* src_row = shape_.bits.data() +
                                  static_cast<std::size_t>(sy) *
                                      static_cast<std::size_t>(shape_.pitch);
    std::uint8_t const* and_row = src_row;
    std::uint8_t const* xor_row =
        mono ? shape_.bits.data() +
                   static_cast<std::size_t>(rows + sy) *
                       static_cast<std::size_t>(shape_.pitch)
             : nullptr;

    std::uint8_t* dst_row = dst + static_cast<std::size_t>(dy) * stride;

    for (int sx = 0; sx < shape_.width; ++sx) {
      int const dx = left + sx;
      if (dx < 0 || dx >= dst_w) continue;
      std::uint8_t* p = dst_row + static_cast<std::size_t>(dx) * 4u;

      switch (shape_.type) {
        case CursorShapeType::Color: {
          std::uint8_t const* s = src_row + static_cast<std::size_t>(sx) * 4u;
          unsigned const a = s[3];
          if (a == 0) break;  // 全透明，一个字节都不用动
          if (a == 255) {
            p[0] = s[0];
            p[1] = s[1];
            p[2] = s[2];
            break;
          }
          blend(p, s[0], s[1], s[2], a);
          break;
        }
        case CursorShapeType::MaskedColor: {
          std::uint8_t const* s = src_row + static_cast<std::size_t>(sx) * 4u;
          if (s[3] == 0) {
            // alpha=0 是"反相"而不是"透明" —— 这是这一支和 COLOR 唯一的区别，
            // 也是它存在的理由：白底上的白指针要靠反相才看得见。
            p[0] ^= s[0];
            p[1] ^= s[1];
            p[2] ^= s[2];
            break;
          }
          if (s[3] == 255) {
            p[0] = s[0];
            p[1] = s[1];
            p[2] = s[2];
            break;
          }
          blend(p, s[0], s[1], s[2], s[3]);
          break;
        }
        case CursorShapeType::Monochrome: {
          bool const and_bit = mask_bit(and_row, sx);
          bool const xor_bit = mask_bit(xor_row, sx);
          if (and_bit) {
            if (xor_bit) {
              p[0] = static_cast<std::uint8_t>(~p[0]);  // 反相
              p[1] = static_cast<std::uint8_t>(~p[1]);
              p[2] = static_cast<std::uint8_t>(~p[2]);
            }
            // AND=1 XOR=0：保持不变
          } else {
            std::uint8_t const v = xor_bit ? 0xFF : 0x00;
            p[0] = v;
            p[1] = v;
            p[2] = v;
          }
          break;
        }
      }
    }
  }
}

std::string CursorOverlay::describe() const {
  char const* kind = "彩色";
  switch (shape_.type) {
    case CursorShapeType::Monochrome:
      kind = "单色";
      break;
    case CursorShapeType::MaskedColor:
      kind = "掩码色";
      break;
    case CursorShapeType::Color:
      break;
  }
  if (!shape_.valid()) {
    return fmt("指针形状未知（可见=%d，位置 %d,%d）", visible_ ? 1 : 0, x_, y_);
  }
  return fmt("%s %dx%d，热点 (%d,%d)，位置 (%d,%d)，%s", kind, shape_.width,
             shape_.pixel_height(), shape_.hotspot_x, shape_.hotspot_y, x_, y_,
             visible_ ? "可见" : "隐藏");
}

}  // namespace d2o
