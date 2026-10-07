#pragma once

/**
 * @file src/capture/cursor_overlay.h
 * @brief 把鼠标指针合成进画面。
 *
 * **桌面复制给出的画面里没有鼠标指针。** DXGI 把指针单独交出来：位置和可见性
 * 在帧信息里，形状要用 `GetFramePointerShape` 另外取。所以"录出来的视频里
 * 没有鼠标"不是 bug，是默认行为 —— 而对一个要冒充摄像机的桌面流来说，
 * 看不见鼠标就等于看不见人在做什么。
 *
 * 这一层**只做像素运算，不认识 DXGI**：形状由调用方（desktop_capture.cpp）
 * 从 DXGI 取出来、翻译成 `CursorShape` 再交进来。分开的理由是它因此可以被
 * 直接喂合成数据验证 —— 三种形状各自的规则都能用几行断言钉死，而不用真的
 * 去动鼠标。
 *
 * 三种形状的合成规则（**从 Windows 的 DDB 绘制语义推出来的，不是抄的**）：
 *
 *   彩色 COLOR
 *     逐像素按自己的 alpha 混合：`dst = src*a + dst*(1-a)`。
 *
 *   掩码色 MASKED_COLOR
 *     `alpha == 0` 表示"把屏幕反相"（`dst.rgb ^= src.rgb`），`alpha > 0` 才是
 *     普通混合。Windows 10 的反色指针（白底上变黑那种）走的就是这一支。
 *
 *   单色 MONOCHROME
 *     1bpp，**高度是实际高度的两倍**：上半是 AND 掩码，下半是 XOR 掩码。
 *     它的绘制语义就是两次 BitBlt —— 先 SRCAND（`dst &= mask`），再 SRCINVERT
 *     （`dst ^= xor`）。把这两步展开就是四种组合：
 *
 *       AND=1 XOR=0 → 保持不变（透明）
 *       AND=1 XOR=1 → 反相（I 形光标压在文字上时用的就是它）
 *       AND=0 XOR=0 → 黑
 *       AND=0 XOR=1 → 白
 *
 *     **顺序不能反**：先 AND 后 XOR 才有"反相"这一档；反过来的话结果永远
 *     只有黑和白两种，I 形光标就会变成一块实心的黑白块。
 */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace d2o {

/// 取值与 `DXGI_OUTDUPL_POINTER_SHAPE_TYPE_*` 一一对应，但**不引 DXGI 的头**。
enum class CursorShapeType {
  Color = 1,        ///< BGRA，自带 alpha
  Monochrome = 2,   ///< 1bpp，AND 掩码 + XOR 掩码
  MaskedColor = 4,  ///< BGRA，alpha=0 表示反相
};

struct CursorShape {
  CursorShapeType type = CursorShapeType::Color;
  int width = 0;
  /// 单色时是**实际高度的两倍**（上半 AND、下半 XOR），见文件头。
  int height = 0;
  /// 形状缓冲里每行占多少字节。**用 DXGI 给的值**，不要自己按 width 算：
  /// 单色的掩码行是 DWORD 对齐的，彩色行也可能带填充。
  int pitch = 0;
  int hotspot_x = 0;
  int hotspot_y = 0;
  std::vector<std::uint8_t> bits;

  /// 形状能不能用。**长度检查必须在这里**：`compose()` 会按 height × pitch
  /// 去读 `bits`，把长度不够的形状放进来就是一次越界读 —— 而越界读的表现
  /// 取决于那块内存里恰好是什么，最坏的情况是画面上出现一块来路不明的东西。
  bool valid() const {
    if (width <= 0 || height <= 0 || pitch <= 0) return false;
    return bits.size() >= static_cast<std::size_t>(height) *
                             static_cast<std::size_t>(pitch);
  }
  /// 单色形状换算成实际像素高度，诊断信息用。
  int pixel_height() const {
    return type == CursorShapeType::Monochrome ? height / 2 : height;
  }
};

class CursorOverlay {
 public:
  /// 换一份形状。形状只在 DXGI 说它变了的时候才取，所以这里不做任何比较。
  void set_shape(CursorShape s) { shape_ = std::move(s); }
  void clear_shape() { shape_ = CursorShape{}; }

  /// 设位置/可见性，返回**和上一次相比是否真的变了**。
  ///
  /// 这个返回值不是装饰：DXGI 的 `LastMouseUpdateTime` 只要鼠标有任何动静就
  /// 会更新（包括按下按键而指针没动），拿它当"指针动了"会白白多编一堆帧。
  /// 所以判断放在这里 —— 只有这里同时知道新旧两种状态。
  bool set_state(int x, int y, bool visible);

  int x() const { return x_; }
  int y() const { return y_; }
  bool visible() const { return visible_; }

  /// 现在画会不会真的落下像素（可见、形状已知、且形状有面积）。
  bool drawable() const { return visible_ && shape_.valid(); }

  /// 手里有没有一份能用的形状。没有时调用方应当主动去取一次。
  bool has_shape() const { return shape_.valid(); }

  /// 把指针画到一块 BGRA 图上。`stride` 是每行字节数（紧凑行距传 width*4）。
  ///
  /// 越界部分自动裁掉：指针贴在屏幕边缘时 hotspot 会把它推到负坐标，
  /// 而负坐标下最容易写出来的 bug 就是往缓冲外面写。
  void compose(std::uint8_t* dst, int dst_w, int dst_h, std::size_t stride) const;

  /// 人话描述，日志用。
  std::string describe() const;

 private:
  CursorShape shape_;
  int x_ = 0;
  int y_ = 0;
  bool visible_ = false;
  bool have_state_ = false;
};

}  // namespace d2o
