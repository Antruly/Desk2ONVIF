#pragma once

/**
 * @file src/capture/display_info.h
 * @brief 枚举可捕获的显示输出，并真的验一下每一个能不能被捕获。
 *
 * "能不能被捕获"这件事必须实测而不是猜。本机装着向日葵，它的
 * `OrayIddDriver Device` 是一个**间接显示驱动（IDD）** —— IDD 的输出没有
 * 真实的显示硬件扫描输出，DXGI 桌面复制抓不到它，DuplicateOutput 会返回
 * DXGI_ERROR_UNSUPPORTED。看适配器名字去猜（"含 idd / virtual 就当是虚拟屏"）
 * 只能覆盖已知的几个厂商，而且会误伤。所以这里直接对每个输出真的建一次
 * 复制会话再关掉，拿到的才是事实。
 *
 * 这个探测是廉价的：DuplicateOutput 只是登记一个会话，不解引用任何帧。
 */

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#ifdef _WIN32
#  include <dxgi1_2.h>
#endif

namespace d2o {

/// 一个显示输出（不是显卡，一台机器可以有多块显卡、每块多路输出）。
struct OutputInfo {
  unsigned adapter_index = 0;   ///< EnumAdapters1 的下标，open() 时要用
  unsigned output_index = 0;    ///< EnumOutputs 的下标
  std::string adapter_name;     ///< 显卡名，如 "NVIDIA GeForce RTX 4060"
  std::string device_name;      ///< 输出设备名，如 "\\\\.\\DISPLAY1"
  RECT desktop_rect{};          ///< 在虚拟桌面里的位置和大小
  bool attached = false;        ///< 是否接在当前桌面上
  bool is_software = false;     ///< 软件适配器（WARP）
  unsigned vendor_id = 0;
  unsigned device_id = 0;

  int width() const { return static_cast<int>(desktop_rect.right - desktop_rect.left); }
  int height() const { return static_cast<int>(desktop_rect.bottom - desktop_rect.top); }
};

/// 探测结果。ok 为假时 message 说明原因。
struct CaptureProbe {
  bool ok = false;
  long hresult = 0;
  std::string message;
};

/// 枚举全部适配器的全部输出。不探测可捕获性（探测要建 D3D 设备，较慢）。
std::vector<OutputInfo> enumerate_outputs();

/// 真的建一次桌面复制会话，随即关闭。这是判定"这个输出能不能录"的唯一硬证据。
CaptureProbe probe_capturable(OutputInfo const& out);

/// 给输出起一个人能认出来的名字，例如
/// "NVIDIA GeForce RTX 4060 — \\\\.\\DISPLAY1（1920x1080 @ 0,0）"。
std::string describe(OutputInfo const& out);

}  // namespace d2o
