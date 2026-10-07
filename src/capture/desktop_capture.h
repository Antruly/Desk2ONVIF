#pragma once

/**
 * @file src/capture/desktop_capture.h
 * @brief DXGI 桌面复制（Desktop Duplication）的封装。
 *
 * 一次调用 = 一步，不隐藏任何状态：
 *
 *   open()    建立会话（D3D 设备 + IDXGIOutputDuplication + 中转纹理）
 *   acquire() 拿下一帧；画面有变化时把像素拷进调用方给的 BgraFrame
 *   close()   释放
 *
 * 关于 ACCESS_LOST
 * ----------------
 * 分辨率切换、锁屏、显示器热插拔、切换用户都会让会话失效，返回
 * DXGI_ERROR_ACCESS_LOST。这是**正常事件**，不是崩溃 —— 正确做法是重建会话
 * 继续录。本类提供 reopen() 专做这件事，调用方负责重试策略。
 *
 * 关于中转纹理
 * ------------
 * DuplicateOutput 给的纹理在 GPU 上，必须 Map 到 CPU 才能读。DXGI 的桌面纹理
 * **不支持** CPU 访问，所以要先 CopyResource 到一张 USAGE_STAGING 的纹理上
 * 再 Map（D3D11 的硬性要求，不是这里的选择）。
 *
 * 拷贝顺序上有个细节：这里用的是"AcquireNextFrame → CopyResource →
 * ReleaseFrame → Map + memcpy"，而不是"AcquireNextFrame → Map + memcpy →
 * ReleaseFrame"。后者少一次 GPU 拷贝，但会把 DXGI 的帧持有到 CPU 拷完 8 MB
 * 为止。多花的这次 GPU blit 很便宜，换来的是不长时间占着复制会话。
 *
 * 关于鼠标指针
 * ------------
 * **桌面复制的画面里没有指针。** 它单独给：位置和可见性在帧信息里，形状要
 * 在 `ReleaseFrame` **之前**用 `GetFramePointerShape` 另取。所以这里自己把
 * 它画上去（规则见 cursor_overlay.h），并且专门处理一种情况：
 *
 *   **只动了鼠标、桌面没变**（`LastPresentTime == 0` 而 `LastMouseUpdateTime`
 *   非零）。这时 DXGI 给的中转纹理内容是**未定义**的，不能读 —— 可要是因此
 *   什么都不产出，画面上的指针就会冻在原地。所以除了要拷的那一份，这里还留着
 *   一份**没有指针的桌面图**（`desktop_`），指针单独动的时候从它重新合成。
 *
 * 代价说清楚：多一份全屏缓冲（3440x1440 约 20 MB，本机 2880x1920 约 22 MB），
 * 以及每次画面更新多一次全屏 memcpy。不画指针就没有这个代价，但没有指针的
 * 录屏对"这台机器在干什么"是说不清的。
 */

#include <cstdint>
#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef _WIN32
#  include <d3d11.h>
#  include <dxgi1_2.h>
#endif

#include "capture/cursor_overlay.h"
#include "capture/frame.h"
#include "common/com_ptr.h"

namespace d2o {

struct OutputInfo;

class DesktopCapture {
 public:
  enum class Status {
    Ok,          ///< 拿到一帧（changed 说明画面是否真的变了）
    Timeout,     ///< 超时内桌面无更新。正常，不是错误
    AccessLost,  ///< 会话失效，调 reopen() 重建
    Fatal,       ///< 别的问题，err 里有说明
  };

  DesktopCapture() = default;
  ~DesktopCapture();

  DesktopCapture(DesktopCapture const&) = delete;
  DesktopCapture& operator=(DesktopCapture const&) = delete;

  /// 建立捕获会话。err 在失败时填入人话。
  bool open(OutputInfo const& out, std::string& err);

  /// 会话失效后重建。设备本身通常还有效，只重来 DuplicateOutput 和中转纹理。
  /// 输出尺寸变了会自动重建中转纹理。
  bool reopen(std::string& err);

  void close();
  bool is_open() const { return dup_.get() != nullptr; }

  /// 取下一帧。
  ///
  /// @param timeout_ms  等待上限；超过返回 Timeout
  /// @param dst         画面有变化时，像素拷进这里（内部按需 resize）
  /// @param changed     收到帧时置位：像素内容是否与上一次不同。
  ///                    **指针移动也算**——那一帧和上一帧看起来就是不一样的，
  ///                    把它报成"没变"会让编码器复用旧画面，指针就冻住了。
  /// @param err         失败时填说明
  Status acquire(std::uint32_t timeout_ms, BgraFrame& dst, bool& changed,
                 std::string& err);

  int width() const { return width_; }
  int height() const { return height_; }

  /// 本会话累计收到的"画面真的变了"的帧数，用于诊断。
  std::uint64_t changed_frames() const { return changed_frames_; }

  /// 因为**只动了鼠标**而产出、否则会整帧丢掉的帧数。桌面静止时这个数会一直
  /// 涨，那正是光标在动 —— 它等于 0 而鼠标明明在动，就是合成没生效。
  std::uint64_t pointer_only_frames() const { return pointer_only_frames_; }

  /// 落过指针像素的帧数。和上面那个的区别：桌面自己也在变的时候，
  /// 指针是顺带画的，不计入 pointer_only_frames_ 但仍计入这里。
  std::uint64_t composited_frames() const { return composited_frames_; }

  /// 当前指针状态的人话描述，日志用。
  std::string pointer_description() const { return cursor_.describe(); }

 private:
  /// 按当前的输出尺寸建中转纹理；尺寸没变则复用已有的。
  bool ensure_staging(std::string& err);

  /// 把 dst 调成当前尺寸。中转纹理和图都要用它。
  void ensure_dst(BgraFrame& dst);

  /// 从复制会话取指针形状。**必须在 ReleaseFrame 之前调** —— 形状是跟着
  /// "当前这一帧"走的，帧一释放它就取不到了。
  void fetch_pointer_shape(DXGI_OUTDUPL_FRAME_INFO const& info);

  ComPtr<ID3D11Device> device_;
  ComPtr<ID3D11DeviceContext> context_;
  ComPtr<IDXGIOutputDuplication> dup_;
  ComPtr<ID3D11Texture2D> staging_;

  unsigned adapter_index_ = 0;
  unsigned output_index_ = 0;
  int width_ = 0;
  int height_ = 0;
  bool first_frame_ = true;
  bool warned_static_ = false;
  std::uint64_t changed_frames_ = 0;
  std::uint64_t pointer_only_frames_ = 0;
  std::uint64_t composited_frames_ = 0;

  CursorOverlay cursor_;
  /// 指针形状的原始缓冲。复用，免得每次形状更新都分配。
  std::vector<std::uint8_t> shape_buf_;
  /// **没有指针**的桌面图。指针单独动的时候从它重新合成，见文件头。
  std::vector<std::uint8_t> desktop_;
  bool have_desktop_ = false;
  /// 形状取失败只报一次，免得每帧刷屏（这条路一坏就是每帧一行）。
  bool warned_shape_ = false;

  bool create_session(std::string& err);
};

}  // namespace d2o
