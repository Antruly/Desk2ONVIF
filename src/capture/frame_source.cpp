/**
 * @file src/capture/frame_source.cpp
 * @brief "要哪一个源"这件事的唯一一份答案。
 *
 * 这个文件小得不成比例，但它存在的理由很硬：在这之前，"显示器还是摄像头"
 * 这个分叉会同时出现在三个地方 —— 配置解析（`channels.items[].source`）、
 * 命令行覆盖（`--display` / `--camera`）、以及采集会话里那句"建哪种源"。
 * 三处各写一次 `if (kind == "webcam")` 的话，加第三种源时必然漏掉一处，
 * 而漏掉的那一处表现为"命令行说的是摄像头、实际录的是桌面"，看不出是代码问题。
 */

#include "capture/frame_source.h"

#include "capture/display_source.h"

namespace d2o {

std::unique_ptr<IFrameSource> make_frame_source(SourceSpec const& spec) {
  if (spec.kind == "webcam") return make_webcam_source(spec.camera);
  // 未知取值走这里（见 SourceSpec::kind 的说明）。
  return std::unique_ptr<IFrameSource>(new DisplaySource(spec.display_index));
}

}  // namespace d2o
