/**
 * @file src/app/channel_plan.cpp
 * @brief 见 channel_plan.h。这里只有 `plan_channels` 一个函数。
 */

#include "app/channel_plan.h"

#include "common/log.h"

#include <algorithm>

namespace d2o {

namespace {

/// 从 `ChannelConfig` 抄出计划里那几个"通道 2 起才有的"字段。
///
/// 抽出来只为一件事：**显式项和自动合成项必须落到同一张字段表上**。
/// 两份各抄一遍的话，加一个字段（比如以后的 `gop`）只会加到其中一处，
/// 而症状是"自动建出来的那个通道这一项永远是默认值" —— 看起来像那个显示器
/// 有什么特殊之处，看不出是漏抄了一个赋值。
void copy_tunables(ChannelConfig const& c, ChannelPlan& p) {
  p.id = c.id;
  p.hardware_id = c.hardware_id;
  p.hardware_serial = c.hardware_serial;
  p.name = c.name;
  p.source = c.source;
  p.display_index = c.display_index;
  p.camera = c.camera;
  p.enabled = c.enabled;
  p.quality = c.quality;
  p.fps = c.fps;
  p.bitrate_kbps = c.bitrate_kbps;
  p.max_height = c.max_height;
  p.latency = c.latency;
  p.gop_seconds = c.gop_seconds;
  p.threads = c.threads;
  p.sub_enabled = c.sub_enabled;
  p.sub_height = c.sub_height;
  p.sub_bitrate_kbps = c.sub_bitrate_kbps;
  p.osd = c.osd;
  p.rtsp_path = c.rtsp_path;
  p.rtsp_sub_path = c.rtsp_sub_path;
}

/// 把 `quality` 这个**标签**展开成具体的 fps / 码率 / 上限 / 子码流开关。
///
/// `validate_config` 对配置里写着的项已经做过一次了，所以显式项走到这里时
/// 展开是**幂等**的（值本来就等于档位值）。自动合成的项则**只有**这一条路
/// 能拿到数值 —— 少调这一次，合成出来的通道就会用 `ChannelConfig` 的结构体
/// 默认值，而那些默认值和档位表放在两个地方，迟早对不上。
void expand_quality(ChannelPlan& p) {
  QualityTier const* t = find_quality_tier(p.quality);
  if (t == nullptr) return;   // custom / 不认识的档位：数值就是它自己
  p.fps = t->fps;
  p.bitrate_kbps = t->bitrate_kbps;
  p.max_height = t->max_height;
  p.sub_enabled = t->sub_enabled;
}

/// 延时档位展开。与 `expand_quality` 同一条纪律：只有这一处，别处不许手抄。
/// `validate_config` 对配置里的项已经展开过（幂等），这里兜的是手搓的计划。
void expand_latency(ChannelPlan& p) {
  LatencyTier const* t = find_latency_tier(p.latency);
  if (t == nullptr) return;
  p.threads = t->threads;
  p.gop_seconds = t->gop_seconds;
}

}  // namespace

std::vector<ChannelPlan> plan_channels(
    std::vector<ChannelDisplayInfo> const& displays, ChannelsConfig const& cfg,
    unsigned channel1_display_index, bool channel1_is_display) {
  std::vector<ChannelPlan> out;
  out.reserve(cfg.items.size());

  // **同一块屏只给一路。** 判据是"这块屏已经被谁占了"，而通道 1 排在最前面
  // —— 它是不是在抓屏、抓的是哪一块，由调用方传进来。
  //
  // **不再自动发现显示器。** 默认安装只有通道 1；别的屏由用户在「通道管理」里
  // 显式添加。自动建通道会让"有几块屏就有几个通道"变成一件用户看不见的事，
  // 而它恰好与"NVR 式的显式通道管理"相反。
  std::vector<unsigned> taken;
  if (channel1_is_display) taken.push_back(channel1_display_index);
  /// 已经被占用的**摄像头**（同一个 reader 只能开一次）。
  std::vector<std::string> taken_cameras;

  for (std::size_t i = 0; i < cfg.items.size(); ++i) {
    ChannelPlan p;
    // **通道号取配置里的稳定编号。** `validate_config` 已经把 0 / 重复 /
    // 越界的号分配好了；这里保留 `i+2` 只是给没走过校验的手搓配置兜底。
    p.index = cfg.items[i].number >= 2
                  ? static_cast<unsigned>(cfg.items[i].number)
                  : static_cast<unsigned>(i) + 2;
    p.item_index = i;
    copy_tunables(cfg.items[i], p);
    expand_quality(p);
    expand_latency(p);

    if (p.source == "display") {
      // 这块屏在本机存在吗？抓不抓得到？
      ChannelDisplayInfo const* found = nullptr;
      for (ChannelDisplayInfo const& d : displays) {
        if (d.index == p.display_index) {
          found = &d;
          break;
        }
      }
      if (found == nullptr) {
        p.enabled = false;
        p.blocked_reason = fmt(
            "显示器 #%u 不在本机当前的显示输出里 —— 可能被拔掉了，"
            "或者显示器序号变了。重新扫描并选一块能抓的屏。",
            p.display_index);
      } else if (!found->capturable) {
        p.enabled = false;
        p.blocked_reason = fmt(
            "显示器 #%u 抓不到（%s）—— 向日葵一类的间接显示驱动常常这样。"
            "这一路建不了，换一块屏。",
            p.display_index, found->description.c_str());
      } else if (std::find(taken.begin(), taken.end(), p.display_index) !=
                 taken.end()) {
        p.enabled = false;
        if (channel1_is_display && p.display_index == channel1_display_index) {
          p.blocked_reason = fmt(
              "显示器 #%u 正被通道 1 使用 —— 一块屏同时只能抓一路"
              "（DXGI 的限制），这一路建不了。给它换一块屏，"
              "或者把通道 1 换到摄像头/别的屏上。",
              p.display_index);
        } else {
          p.blocked_reason = fmt(
              "显示器 #%u 已经被前面某一项占用了 —— 一块屏同时只能抓一路"
              "（DXGI 的限制）。删掉重复的那一项。",
              p.display_index);
        }
      }
      if (p.enabled) taken.push_back(p.display_index);
    } else if (p.source == "webcam" && p.enabled) {
      // **同一台摄像头只给一路。** Media Foundation 的 SourceReader 在释放时会
      // 把它拿到的 source `Shutdown()`，所以第二路根本开不起来（E_ACCESSDENIED
      // 或者 MF_E_SHUTDOWN）—— 而那看起来像"摄像头坏了"。判据优先用绑定的
      // `hardware_id`；老配置没有它时退回选择器（空 = 第一台）。
      std::string const key =
          !p.hardware_id.empty()
              ? p.hardware_id
              : (p.camera.empty() ? std::string("@first") : p.camera);
      if (std::find(taken_cameras.begin(), taken_cameras.end(), key) !=
          taken_cameras.end()) {
        p.enabled = false;
        p.blocked_reason = fmt(
            "摄像头「%s」已经被前面某一项占用 —— 一个摄像头同时只能被一路"
            "打开。删掉重复的那一项，或者给这一路换一台设备。",
            p.camera.empty() ? "第一台（默认）" : p.camera.c_str());
      } else {
        taken_cameras.push_back(key);
      }
    }
    out.push_back(std::move(p));
  }

  return out;
}

}  // namespace d2o
