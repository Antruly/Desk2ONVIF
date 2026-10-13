#pragma once

/**
 * @file src/capture/frame_source.h
 * @brief 一帧画面的来源。桌面（DXGI）和摄像头（Media Foundation）是它的两种实现。
 *
 * ## 为什么要有这一层
 *
 * 本轮之前，采集会话里写死了"枚举显示输出 → 挑一个能捕获的 → 建 DXGI 复制会话"
 * 这一串，而它是长在 `CaptureSession::run()` 里的。于是"换一个画面来源"就等于
 * "改采集会话"，而采集会话是本项目最大的一文件（编码、封装、推流、统计全在里面）。
 *
 * 抽出来之后，`run()` 只认下面这四个动词。它不认识 DXGI，也不认识 Media
 * Foundation —— 那两样各自活在自己的 .cpp 里。
 *
 * ## 状态词汇表是**共用**的，不是每边一套
 *
 * `Ok` / `Timeout` / `AccessLost` / `Fatal` 这四个词的全部意义在于让 `run()` 里
 * 那套"超时不算错误 / 会话失效就重建 / 别的就当故障"的**退避逻辑只有一份**。
 * 摄像头拔线和显示器改分辨率在语义上是同一件事（会话没了，重建即可），如果
 * 两边各报各的，`run()` 就得分叉成两套重试策略 —— 而两套里必有一套是没人测过的。
 *
 * `display_source.cpp` 里那个 `switch` 故意**不写 default**：将来给
 * `DesktopCapture::Status` 加一个取值时，编译器会在那里报错，逼着人回答
 * "这个新的失败方式该映射成哪一个"。写成 default 的话，新取值会静默地落进
 * 兜底分支 —— 而失败方式被静默地归错类，正是最难查的一类问题。
 */

#include "capture/frame.h"

#include <cstdint>
#include <memory>
#include <string>

namespace d2o {

/// 取一帧的结果。见文件头"状态词汇表是共用的"。
enum class SourceStatus {
  Ok,          ///< 拿到一帧（`changed` 说明画面是否真的变了）
  Timeout,     ///< 超时内没有新内容。**是正常状态，不是错误**
  AccessLost,  ///< 会话失效（分辨率变了 / 锁屏 / 拔线 / 被别的程序占用），reopen() 重建
  Fatal,       ///< 别的问题，err 里是人话
};

/// 画面来源。**不是线程安全的** —— 一个通道一个实例，只有那个通道的线程碰它。
class IFrameSource {
 public:
  virtual ~IFrameSource() = default;

  IFrameSource(IFrameSource const&) = delete;
  IFrameSource& operator=(IFrameSource const&) = delete;

  /// 建立会话。失败时 err 填**人话**（不是 HRESULT）。
  ///
  /// 这里包含"挑哪一个"的全部逻辑：桌面要枚举输出、探一次可捕获性；摄像头要
  /// 按名字或序号找设备。`run()` 传进来的只是一个**选择**（下标 / 名字），
  /// 它不知道这个选择会被怎么解释。
  virtual bool open(std::string& err) = 0;

  /// 会话失效后重建。设备本身通常还有效。
  virtual bool reopen(std::string& err) = 0;

  virtual void close() = 0;

  /// 取下一帧。`dst` 按需 resize。
  ///
  /// @param changed 收到帧时置位：像素内容是否与上一次不同。**指针移动、水印
  ///                文字变化也算**——报成"没变"会让编码器复用旧画面。
  virtual SourceStatus acquire(std::uint32_t timeout_ms, BgraFrame& dst,
                               bool& changed, std::string& err) = 0;

  /// 把**手里那份底图**重新合成一张帧。存在的理由只有一个：画面没动，但画上去
  /// 的东西要变（水印配置改了）。返回 `Timeout` = 还没有底图，调用方照常当
  /// "这一拍没有内容"处理即可 —— 它不是一个错误状态。
  virtual SourceStatus reemit(BgraFrame& dst) = 0;

  virtual int width() const = 0;
  virtual int height() const = 0;

  /// 人话描述，日志和 `--list-*` 用。例如
  /// "NVIDIA RTX 4060 — \\\\.\\DISPLAY1（2880x1920 @ 0,0）"。
  virtual std::string describe() const = 0;

 protected:
  IFrameSource() = default;
};

/// 指针合成的诊断读数。**只有桌面源有这一套**。
///
/// ## 为什么不把它并进 `IFrameSource`
///
/// 摄像头没有"鼠标指针"这个概念。挂在主接口上的话，`WebcamSource` 就得实现
/// 两个**恒为 0** 的函数 —— 而"恒为 0"和"这一路真的没合成过指针"在日志和
/// `/api/status` 里长得一模一样。那正是最坏的一类读数：它看起来像在回答一个
/// 问题，实际上不管发生了什么都是那个值。
///
/// 用 `dynamic_cast` 一次（在 `open()` 之后）拿到它，拿不到就是"这一路没有指针
/// 这回事"—— 那是**结构上**的答案，不是读出来的数。
class ICursorDiagnostics {
 public:
  virtual ~ICursorDiagnostics() = default;
  /// 本会话落过指针像素的帧数。
  virtual std::uint64_t composited_frames() const = 0;
  /// 其中**只因为鼠标动了**才产出的帧数（桌面静止时光标还在动，靠的就是它们）。
  virtual std::uint64_t pointer_only_frames() const = 0;
  /// 当前指针状态的人话描述，日志用。
  virtual std::string pointer_description() const = 0;
};

/// "要哪一个画面来源"的**唯一一份描述**。
///
/// 配置里 `channels.items[]` 的一项、命令行 `--display` / `--camera`，都先归一
/// 成它再往后走。不这么做的话，"选源"这件事会有两份语义（一份在配置解析里、
/// 一份在命令行覆盖里），而它们对同一个字符串给出不同解释的症状是
/// "配置里写的摄像头，重启之后变成了显示器"。
struct SourceSpec {
  /// `display` = 桌面（DXGI），`webcam` = 摄像头（Media Foundation）。
  /// **未知取值一律当 display**：`validate_config` 已经在配置那一层把非法值
  /// 挡掉了，这里再报一次错只会多一条没人会读的分支。
  std::string kind = "display";
  /// `kind == "display"` 时用。`enumerate_outputs()` 里的序号。
  unsigned display_index = 0;
  /// `kind == "webcam"` 时用。空 = 第一台。见 `WebcamSource` 的匹配规则。
  std::string camera;
};

/// 按描述造一个源。**这是唯一一处** `new DisplaySource` / `new WebcamSource`
/// 出现的地方 —— 于是"两种源各自要什么参数"这件事只有一份答案。
std::unique_ptr<IFrameSource> make_frame_source(SourceSpec const& spec);

/// 摄像头源的构造函数。
///
/// **声明在这里、定义在 `webcam_capture.cpp`。** 这样 `frame_source.cpp` 不必
/// 把 Media Foundation 的头文件拖进来（它和 `windows.h` 的包含顺序很挑，而
/// 这里只是个决策点，没有任何理由付那个代价）。
std::unique_ptr<IFrameSource> make_webcam_source(std::string selector);

}  // namespace d2o
