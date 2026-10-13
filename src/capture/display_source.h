#pragma once

/**
 * @file src/capture/display_source.h
 * @brief `IFrameSource` 的桌面实现：把 `DesktopCapture` 包一层。
 *
 * 它做的那件"多出来的事"只有一件，但很关键：**把"挑哪一个输出"从
 * `CaptureSession::run()` 搬进 `open()`**。
 *
 * 从前 run() 里是这么写的：enumerate_outputs() → 检查下标 → probe_capturable()
 * → DesktopCapture::open() → 检查尺寸偶数。前面三步现在归这里，因为它们回答的
 * 是"这个源到底是哪一个"；最后一步留在 run()，因为**它是所有源共用的**
 * （摄像头也可能给出奇数边长，而 H.264 对奇数边长是直接拒绝）。
 *
 * 挑输出这件事里有两处不能省：
 *
 *   * **`probe_capturable()`**：真的建一次复制会话再关掉。看适配器名字去猜
 *     （"含 idd / virtual 就当虚拟屏"）只能覆盖已知的几个厂商，而且会误伤 ——
 *     本机装着的向日葵就是一个 IDD 输出，它的 DuplicateOutput 返回
 *     `DXGI_ERROR_UNSUPPORTED`。名字猜不出来，只有真建一次才知道。
 *   * **不静默地换一个输出**：下标越界、或者那个输出探不通，都是**报错**。
 *     悄悄退到 0 号输出的话，用户看到的是"我指的是第二块屏，录出来的是第一块"，
 *     而日志里一个字都不提。管多通道时这一条更硬：通道 2 指着不存在的屏，
 *     它就必须**红着**，而不是偷偷去录主屏。
 */

#include "capture/desktop_capture.h"
#include "capture/display_info.h"
#include "capture/frame_source.h"

#include <string>

namespace d2o {

class DisplaySource : public IFrameSource {
 public:
  /// `display_index` 是 `enumerate_outputs()` 里的序号（`--list-displays` 报的
  /// 那个）。**不是 DISPLAY1/DISPLAY2 里的那个数字** —— 那两个数在本机实测就
  /// 不一致（枚举顺序按适配器分组，而设备名是系统分配的另一套编号）。
  explicit DisplaySource(unsigned display_index) : want_(display_index) {}

  bool open(std::string& err) override;
  bool reopen(std::string& err) override;
  void close() override;

  SourceStatus acquire(std::uint32_t timeout_ms, BgraFrame& dst, bool& changed,
                       std::string& err) override;
  SourceStatus reemit(BgraFrame& dst) override;

  int width() const override { return capture_.width(); }
  int height() const override { return capture_.height(); }
  std::string describe() const override { return describe_; }

  /// 这一路实际落在哪个输出上（`open()` 之后才有效）。多通道的日志和
  /// `/api/status` 要用它回答"通道 2 到底录的是哪块屏"。
  std::string const& device_name() const { return out_.device_name; }
  unsigned display_index() const { return want_; }

  /// 指针那几项计数器。它们**按通道分开**（见 `SessionStats`），所以只能从这里取。
  std::uint64_t pointer_only_frames() const {
    return capture_.pointer_only_frames();
  }
  std::uint64_t composited_frames() const { return capture_.composited_frames(); }

 private:
  unsigned want_ = 0;
  OutputInfo out_{};
  std::string describe_;
  DesktopCapture capture_;
};

}  // namespace d2o
