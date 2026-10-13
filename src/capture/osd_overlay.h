#pragma once

/**
 * @file src/capture/osd_overlay.h
 * @brief 把水印（OSD）烧进 BGRA 帧。
 *
 * 和 `CursorOverlay` 同构：**只做像素运算**，给它一个可写的 BGRA 缓冲和它的
 * 步长，它负责画。它不认识 DXGI、编码器、RTSP。
 *
 * 为什么只在这一个点合成：采集线程每拍只产出一份 `BgraFrame`，主/子两个编码器、
 * MP4、RTSP、浏览器预览、快照吃的都是**同一份内存**（见 capture_session.cpp 里
 * 主编码器和子编码器共用 `*f.image` 那两处）。在这里画一次，六路输出全都带上，
 * 而且不可能漂移 —— 分开画的话，任何一路的文字晚一个版本都会变成"哪个是对的"
 * 这种查不清的问题。
 *
 * 代价是**子码流上的字会等比变小变糊**（它是同一帧缩下去的）。这是刻意的取舍，
 * 写在 README 的"没做的"里。
 */

#include "common/config.h"

#include <chrono>
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace d2o {

class DeviceState;

class OsdOverlay {
 public:
  OsdOverlay();
  /// **必须在 .cpp 里定义**：`items_` 的元素类型带 GDI 掩码和缓存，
  /// 只在这里前向声明。类内 `= default` 的析构会当场要求它完整，
  /// 报出来是一串 `unique_ptr.h` 里的 `sizeof` 不完整类型（陷阱 43）。
  ~OsdOverlay();

  OsdOverlay(OsdOverlay const&) = delete;
  OsdOverlay& operator=(OsdOverlay const&) = delete;

  /// 应用一份配置。**只在采集线程上调用**（它会重建文字掩码缓存）。
  ///
  /// `device` 只是被借用（读取名称），必须活得比本对象久；可以为空 ——
  /// 那时 `name` 类型的项画不出东西，但不报错。
  void configure(OsdConfig const& cfg, DeviceState const* device);

  bool enabled() const;

  /// "这一帧水印上会写什么"的指纹（已按 `wall` 解析成最终文字）。
  ///
  /// 调用方**必须**在它变化时把这一帧当成一次内容变化。理由：桌面静止时
  /// `got_content` 为假，编码器会复用上一帧的 YUV（x264_encoder.cpp 里那条
  /// `content_changed` 短路），于是水印上的秒会**冻住** —— 一个走了十分钟、
  /// 时间还停在起点的水印，比没有水印更糟。
  ///
  /// **空串 = 这一刻一项都画不出来**（和 `compose()` 返回 0 等价）。
  /// `enabled()` 问的是"这个功能开没开"，它问的是"这一刻有没有东西可画"；
  /// 两者不是一回事 —— 名称项开着而名字是空的，就是前者真、后者假。
  std::string key(std::chrono::system_clock::time_point wall) const;

  /// 把水印画上去。返回真正落笔的项数。
  int compose(std::uint8_t* dst, int dst_w, int dst_h, std::size_t stride,
              std::chrono::system_clock::time_point wall);

  /// 每一项**已经栅格好的**文字框在主码流里的位置（像素）。
  /// `visible = false` 表示这一项此刻画不出东西（文字为空、或还没 compose 过）。
  /// 给单元测试断言"位置对不对、有没有出界"用。
  struct Placement {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    bool visible = false;
  };
  std::vector<Placement> placements() const;

 private:
  struct Item;

  OsdConfig cfg_;
  DeviceState const* device_ = nullptr;
  std::vector<Item> items_;
};

}  // namespace d2o
