#pragma once

/**
 * @file src/app/channel_plan.h
 * @brief "这一趟要建哪几路通道、每一路要什么" —— 一份**纯函数**的答案。
 *
 * ## 为什么把它抽成一个不碰 DXGI 的纯函数
 *
 * 这条规则有两半：显式项那一半（`channels.items[]`，`validate_config` 已经
 * 展开过档位、补过名字和路径）和自动发现那一半（**运行期**枚举出来的显示器，
 * 不落盘）。两半合起来决定一件很实在的事 —— **端口号和 RTSP 路径**。
 *
 * 把它写在 `cmd_run` 里的话，验证它需要一台接着两块屏的机器。而这条开发机上
 * 只有一块屏（`--list-displays` 实测 1 个输出），于是"自动建通道"这段代码
 * 永远不会有第二条路径被跑过 —— 它在多屏机器上错成什么样，在单屏机器上
 * 一行日志都看不出来。抽成纯函数之后，`tests\channel_plan_check.cpp` 可以拿
 * 手搓的显示器清单（0/1/3 块、可捕获/不可捕获、被显式项覆盖/没被覆盖）逐条钉住。
 *
 * ## 它**不**答的问题
 *
 * "那一路现在跑得怎么样"（fps / 码率 / 状态）完全不在这一份里 —— 那是运行时的
 * 事，归 `ChannelStatus`。两份合一个结构的代价是"计划"里会混进每秒都在变的数，
 * 于是"这一路该用哪个端口"这种问题也得先跑起来才答得出来。
 */

#include "common/config.h"

#include <cstddef>
#include <string>
#include <vector>

namespace d2o {

/// 一台显示输出在**规划通道**时需要的全部信息。
///
/// 刻意不是 `OutputInfo`：那个结构里还带着 DXGI 的适配器序号、厂商 ID 这些
/// 规划用不到的东西，而这一份要能在没有 GPU、没有窗口的单元测试里手搓出来。
struct ChannelDisplayInfo {
  /// `enumerate_outputs()` 里的序号，也就是 `--list-displays` 报的 `#N`。
  unsigned index = 0;
  /// 人话描述（`describe()` 那一份），只给人看。
  std::string description;
  int width = 0;
  int height = 0;
  int x = 0;
  int y = 0;
  /// 探过可捕获性没有。**false 的屏不建通道** —— 向日葵的虚拟屏就是这一类，
  /// 给它建一路的结果是"这个通道永远黑屏"，而用户会去查摄像头或网络。
  bool capturable = false;
};

/// 一个通道的**静态计划**。通道 1 **不在这里** —— 它由顶层 `capture.*` /
/// `rtsp.*` 描述，代码也在 `cmd_run` 里那条既有路径上（见 main.cpp 的说明）。
struct ChannelPlan {
  /// 通道号，**2 起**。端口是 `web.port + index`，默认路径是 `/ch<index>`。
  unsigned index = 0;
  std::string name;
  /// `display` | `webcam`。未知取值在配置层已被挡掉（`validate_config`）。
  std::string source = "display";
  unsigned display_index = 0;
  std::string camera;
  /// false = 这一条**占号但不建**（用户显式关掉的显示器通道）。
  bool enabled = true;
  std::string quality = "smooth";
  int fps = 20;
  int bitrate_kbps = 1024;
  int max_height = 1080;
  bool sub_enabled = true;
  int sub_height = 360;
  int sub_bitrate_kbps = 512;
  /// 不含前导斜杠，和 `RtspStreamOptions::path` 一致。
  std::string rtsp_path;
  std::string rtsp_sub_path;

  /// true = 这一条是自动发现的显示器**合成**出来的，配置里没有对应项。
  /// 界面上要能区分"我配的"和"它自己冒出来的"，否则用户会去配置里找一条
  /// 根本不存在的项。
  bool auto_created = false;
  /// 对应 `channels.items[]` 的下标；`auto_created` 时是 `npos`。
  std::size_t item_index = static_cast<std::size_t>(-1);

  /// 非空 = 这一条**建不了**，`enabled` 已被置为 false，这句话是给用户看的
  /// 原因（直接当状态那一栏的文案）。
  ///
  /// 和"用户自己关掉的"（`enabled:false` 且这里为空）分开：一种是"你关了"，
  /// 另一种是"你要的这路要不到"。都写"已关闭"的话，用户会去配置里找一个
  /// 明明是开着的开关。
  std::string blocked_reason;
};

/// 规划出**通道 2 起的全部通道**，顺序即通道号。
///
/// @param displays 运行期枚举出来的显示输出（含抓不了的）。
/// @param cfg      配置里的 `channels` 那一棵。
/// @param channel1_display_index 通道 1 抓的是哪一块屏。
/// @param channel1_is_display   通道 1 是不是在抓屏（`--camera` 之后它不是）。
///
/// 规则（四条，顺序即优先级）：
///   1. `items[]` 里每一项都成为一个计划，**通道号 = 下标 + 2**（与
///      `validate_config` 的 `no = i + 2` 是同一条规则）。**`enabled:false`
///      的项也占号** —— 否则"勾掉再勾回来"会让后面每一路的端口和路径整体
///      挪一格，而 NVR 上那个书签是按路径存的。
///   2. **同一块屏只给一路**：显式项里 `source == "display"` 且那块屏已经被
///      通道 1（它也在抓屏时）或者**前面某一项**占了的，标记
///      `blocked_reason` + `enabled = false`，仍然占号。
///      DXGI 一块输出同时只允许一个 `IDXGIOutputDuplication`，第二个
///      `DuplicateOutput` 必失败；而**谁先谁后是线程赛跑**（通道 2 的线程先
///      起、通道 1 的会话后跑），所以不拦的话结果有两种：要么这一路红、要么
///      **通道 1 红**并带着整个进程退出（`run()` 失败 = 没有通道 1 = 没事可做）。
///      一个配错的屏号因此有了 50% 的概率把服务弄没，而两种结果都不说原因。
///   3. 每一块**可捕获**且没被上面任何一项覆盖、也不是通道 1 那块的显示器，
///      合成一条 `enabled` + 默认档位的计划，号从上面最后一项之后接着排。
///   4. 抓不了的屏**一条都不建** —— 建了就是一路永远黑屏的通道。
std::vector<ChannelPlan> plan_channels(
    std::vector<ChannelDisplayInfo> const& displays, ChannelsConfig const& cfg,
    unsigned channel1_display_index, bool channel1_is_display);

}  // namespace d2o
