#pragma once

/**
 * @file src/capture/webcam_capture.h
 * @brief `IFrameSource` 的摄像头实现：Media Foundation 的设备源（Device Source）。
 *
 * 桌面那一侧是 `display_source.h`。两者实现的是同一个接口，所以采集会话里那套
 * "超时不算错误 / 会话失效就重建 / 别的当故障"只有一份。这里只说摄像头特有的事。
 *
 * ## 四条实测出来的硬约束（原始输出见 out\ae-probe\EVIDENCE.md）
 *
 * **① 入口不在 `mfplat.dll` 里。** `MFEnumDeviceSources` / `MFCreateDeviceSource`
 * 在 `libmfplat.a` 里没有 stub，`objdump -p mfplat.dll` 也找不到它们 —— 它们定义
 * 在 `mfcore.dll`，由 `mf.dll` 转发。所以这里不去改构建系统、不写 .def，而是
 * `LoadLibraryW(L"mf.dll")` + `GetProcAddress` 绕过去。**链接库缺 stub 不等于
 * 这个功能不存在**，这两件事看起来一模一样，量之前很容易得错结论。
 *
 * **② 请求 `MFVideoFormat_RGB32` 必须带 `MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING`。**
 * 这台摄像头原生只给 NV12 / MJPG / YUY2，裸的 SourceReader 请求 RGB32 直接
 * `MF_E_INVALIDMEDIATYPE`（实测 0xc00d36b4）。加上那条属性之后 MF 会在中间插一个
 * Video Processor MFT 替我们转 —— **我们一行像素转换代码都不用写**，而且它给出的
 * BGRA 与 DXGI 那条路**同字节序、同行宽**（实测首字节 `54 2d 2a ff` = B,G,R,A），
 * 于是下游的 YUV 转换、水印、快照一行都不用改。
 *
 * **③ 行宽只能从缓冲区长度推。** 这台摄像头对任何格式都**不报**
 * `MF_MT_DEFAULT_STRIDE`（实测 present=0，值 0）。拿它当行宽会得到 0，算出来的每个
 * 像素都落在第 0 行上，画面斜着糊成一条 —— 而代码里看起来"我读了官方给的行宽字段"。
 * 判据是 `pitch = 缓冲区字节数 / 高度`，并要求 `pitch >= 4*宽度`。
 *
 * **④ reader 拥有 source，一个通道一个 reader，整个生命周期里不重建。**
 * `MFCreateSourceReaderFromMediaSource` 造出来的 reader 在**释放时会把它拿到的
 * `IMFMediaSource` 一起 `Shutdown()`**（除非给它
 * `MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN`）。所以"每帧建一个 reader"
 * 或者"用同一个 source 建第二个 reader"都会拿到 0xc00d3e85（MF_E_SHUTDOWN），
 * 而现场看起来像是"加了那条属性反而坏了"。本类因此**绝不**设那个断开标志：
 * reader 就是这一路摄像头的会话，`close()` 时才放。
 *
 * ## 为什么不是 DirectShow，也不是自己搭 Native 层
 *
 * 本轮的需求是"把摄像头单独加成一个通道"，而通道要的只有三件事：拿到 BGRA 帧、
 * 尺寸、以及"设备没了"这个信号。SourceReader 正好就是这三件事的最小封装：它自己
 * 管协商、自己插转换 MFT、自己管线程。DirectShow 那一套（Capture Graph Builder）
 * 要多写一整个图的搭建与拆除，而且在 Windows 11 上仍然是同一批底层驱动。
 */

#include "capture/frame.h"
#include "capture/frame_source.h"
#include "common/com_ptr.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef _WIN32
#  include <mfidl.h>        // IMFMediaSource / IMFActivate
#  include <mfreadwrite.h>  // IMFSourceReader
#endif

namespace d2o {

/// 一台摄像头设备（枚举结果）。
///
/// 只有两个字段是"这台设备是谁"，其余一概不猜：**能力（支持哪些格式、最大分辨率）
/// 不在这里问**。问一次能力就要真的把设备打开一次（MF 的设备源不像显示器那样有
/// 一份静态描述），而"打开"这件事一次只能被一个程序做 —— 枚举清单不该有副作用。
/// 真正要用的那台在 `open()` 里打开，尺寸那时自然会知道。
struct CameraInfo {
  std::string name;           ///< MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME
  std::string symbolic_link;  ///< MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK
  bool is_default = false;    ///< 枚举里的第一台
};

/// 枚举本机摄像头。进程内第一次调用会做 MFStartup。
///
/// 失败（MF 不可用、枚举报错）返回空数组，并把理由写进 err（err 可以为空指针）。
/// **"一台都没有"不算失败**：那台机器就没有摄像头，返回空数组、err 为空 ——
/// 和 `list_audio_endpoints` 同一条规矩，把"没有设备"和"查不了设备"混成一句话，
/// 用户会去修那个根本没坏的东西。
std::vector<CameraInfo> enumerate_cameras(std::string* err = nullptr);

/// 一路摄像头的画面来源。
///
/// **不是线程安全的**，和 `IFrameSource` 的全部实现一样：一个通道一个实例，
/// 只有那个通道的采集线程碰它。
class WebcamSource : public IFrameSource {
 public:
  WebcamSource() = default;
  ~WebcamSource() override;

  /// selector 为空 → 第一台；否则先按 symbolic_link 精确匹配、再按 name 精确匹配、
  /// 最后按 name 的子串（不区分大小写）匹配。都匹配不上 → 报错列出可选项。
  /// 匹配不唯一时**报错**而不是挑第一个（静默挑一个的症状是"选错了摄像头"）。
  explicit WebcamSource(std::string selector) : selector_(std::move(selector)) {}

  bool open(std::string& err) override;
  bool reopen(std::string& err) override;
  void close() override;

  SourceStatus acquire(std::uint32_t timeout_ms, BgraFrame& dst, bool& changed,
                       std::string& err) override;
  SourceStatus reemit(BgraFrame& dst) override;

  int width() const override { return width_; }
  int height() const override { return height_; }

  /// 人话描述。打开之后是"FHD Camera（1920x1080 @ 30fps，Media Foundation）"，
  /// **打开之前也不会空着** —— 那时至少还知道配置里写的是哪一台。见 .cpp。
  std::string describe() const override;

  bool is_open() const { return reader_.get() != nullptr; }

  /// 这一路实际落在哪台设备上（`open()` 之后才有效）。多通道的日志和
  /// `/api/status` 要用它回答"通道 2 到底录的是哪个摄像头"。
  std::string const& device_name() const { return name_; }
  std::string const& symbolic_link() const { return link_; }

 private:
  /// 配置里写的那一份（空 = 第一台）。`open()` 每次拿它重新解析 —— 摄像头拔了
  /// 再插上符号链接会变，而用户配置里存的多半是**友好名**或干脆是空的。
  std::string selector_;

  /// 本次会话落在哪台设备上。**`close()` 时清掉**：它们是"这一次会话"的记录，
  /// 会话没了它就过期了（`display_source.h` 对 `out_` 是同一条规矩）。
  std::string name_;
  std::string link_;

  /// reader 与它的 source。**顺序有意义**：`close()` 里必须先放 reader（它的
  /// 释放会对 source 调 `Shutdown`）。见文件头第 ④ 条。
  ComPtr<IMFSourceReader> reader_;
  ComPtr<IMFMediaSource> source_;

  int width_ = 0;
  int height_ = 0;
  /// 协商出来的帧率（`MF_MT_FRAME_RATE` 是分数，PAL 那种 30000/1001 就在这一对里）。
  /// 只用于 `describe()`，**不拿去当"通道的帧率"** —— 那一个是配置说了算的。
  unsigned fps_num_ = 0;
  unsigned fps_den_ = 0;
  std::string describe_;

  /// 最近一帧**没有烧过水印的**原始像素，给 `reemit()` 用。见 `reemit()` 的说明。
  BgraFrame last_;
  bool have_last_ = false;

  /// 首帧的尺寸 / 缓冲区字节数 / 推出的行宽只报一次。这几行是"画面斜着"这类
  /// 问题的**唯一**现场证据：只有把实测的缓冲区长度和推出的行宽摆出来，才能一眼
  /// 看出"摄像头给的行是带填充的"还是"我们算错了"。
  bool logged_first_frame_ = false;
};

}  // namespace d2o
