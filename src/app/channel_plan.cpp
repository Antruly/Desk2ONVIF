/**
 * @file src/app/channel_plan.cpp
 * @brief 见 channel_plan.h。这里只有 `plan_channels` 一个函数。
 */

#include "app/channel_plan.h"

#include "common/log.h"

namespace d2o {

namespace {

/// 从 `ChannelConfig` 抄出计划里那几个"通道 2 起才有的"字段。
///
/// 抽出来只为一件事：**显式项和自动合成项必须落到同一张字段表上**。
/// 两份各抄一遍的话，加一个字段（比如以后的 `gop`）只会加到其中一处，
/// 而症状是"自动建出来的那个通道这一项永远是默认值" —— 看起来像那个显示器
/// 有什么特殊之处，看不出是漏抄了一个赋值。
void copy_tunables(ChannelConfig const& c, ChannelPlan& p) {
  p.name = c.name;
  p.source = c.source;
  p.display_index = c.display_index;
  p.camera = c.camera;
  p.enabled = c.enabled;
  p.quality = c.quality;
  p.fps = c.fps;
  p.bitrate_kbps = c.bitrate_kbps;
  p.max_height = c.max_height;
  p.sub_enabled = c.sub_enabled;
  p.sub_height = c.sub_height;
  p.sub_bitrate_kbps = c.sub_bitrate_kbps;
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

}  // namespace

std::vector<ChannelPlan> plan_channels(
    std::vector<ChannelDisplayInfo> const& displays, ChannelsConfig const& cfg,
    unsigned channel1_display_index, bool channel1_is_display) {
  std::vector<ChannelPlan> out;
  out.reserve(cfg.items.size() + displays.size());

  // --- 1. 显式项 ----------------------------------------------------------
  //
  // 这一趟同时做规则 2：**同一块屏只给一路**。判据是"这块屏已经被谁占了"，
  // 而通道 1 排在最前面 —— 它是不是在抓屏、抓的是哪一块，由调用方传进来。
  std::vector<unsigned> taken;   // 已经被占掉的显示器序号
  if (channel1_is_display) taken.push_back(channel1_display_index);
  for (std::size_t i = 0; i < cfg.items.size(); ++i) {
    ChannelPlan p;
    // **通道号 = 下标 + 2。** `validate_config` 就是按这一条补的名字和默认
    // 路径（`channels.cpp` 里那句 `int const no = static_cast<int>(i) + 2`），
    // 这里再按同一把尺子算一遍 —— 两边算得不一样的话，配置里写的
    // `/ch3` 会接到一个叫「通道 4」的通道上。
    p.index = static_cast<unsigned>(i) + 2;
    p.item_index = i;
    copy_tunables(cfg.items[i], p);
    expand_quality(p);

    // 规则 2：这块屏是不是已经被别人占了？
    if (p.source == "display") {
      for (unsigned const d : taken) {
        if (d != p.display_index) continue;
        p.enabled = false;
        if (channel1_is_display && d == channel1_display_index) {
          // 通道 1 是"先来"的：它由顶层 `capture.*` 描述，用户没别的办法
          // 让它换一块屏（只能 `--display` / `--camera`）。
          p.blocked_reason = fmt(
              "显示器 #%u 正被通道 1 使用 —— 一块屏同时只能抓一路"
              "（DXGI 的限制），这一路建不了。给它换一块屏，"
              "或者把通道 1 换到摄像头/别的屏上。",
              d);
        } else {
          p.blocked_reason = fmt(
              "显示器 #%u 已经被前面某一项占用了 —— 一块屏同时只能抓一路"
              "（DXGI 的限制）。删掉重复的那一项。",
              d);
        }
        break;
      }
      if (p.enabled) taken.push_back(p.display_index);
    }
    out.push_back(std::move(p));
  }

  // --- 2. 自动发现的显示器 ------------------------------------------------
  //
  // 号从**全部显式项之后**接着排（含被关掉的项）。于是"关掉通道 3"不会让
  // 通道 4 变成通道 3 —— 序号可能出现空档，而空档不伤人；反过来把后面每一路
  // 的端口和 RTSP 路径整体挪一格，会伤到 NVR 上那些按路径存的书签。
  unsigned next = static_cast<unsigned>(cfg.items.size()) + 2;
  for (ChannelDisplayInfo const& d : displays) {
    if (!d.capturable) continue;   // 见头文件规则 3
    if (channel1_is_display && d.index == channel1_display_index) continue;
    bool covered = false;
    for (ChannelConfig const& it : cfg.items) {
      // **只按 display_index 认领，不看 `enabled`。** `enabled:false` 的项
      // 正是用户"关掉这块屏"的写法 —— 把它当成"没覆盖"就会把用户刚关掉的
      // 通道原样又建回来，而界面上那条项明明白白写着已关闭。
      //
      // `source == "webcam"` 的项不参与：那个字段对摄像头项没有意义（默认
      // 全是 0），拿它认领会让"配了一路摄像头"顺手把显示器 #0 也吞掉。
      if (it.source == "display" && it.display_index == d.index) {
        covered = true;
        break;
      }
    }
    if (covered) continue;

    ChannelPlan p;
    p.index = next++;
    // 名字里的序号用**显示器的**，不是通道号：`--list-displays` 报的是 `#N`，
    // 界面上写的也是同一个数。写成通道号的话，用户按界面上那句去
    // `--display N` 会指到一块别的屏上。
    p.name = fmt("显示器 #%u", d.index);
    p.source = "display";
    p.display_index = d.index;
    p.auto_created = true;
    // 默认档位「流畅」——`ChannelConfig::quality` 的默认值就是它。走
    // `expand_quality` 而不是在这里手抄 20/1024/1080：档位表改了而这里没改，
    // 症状是"自动建的那一路画质和界面上选的对不上"。
    expand_quality(p);
    // 路径的默认值在配置层是 `validate_config` 补的，自动项没经过它，所以
    // 在这里按同一条规则补（`/ch<N>` / `/ch<N>_sub`）。
    p.rtsp_path = fmt("/ch%u", p.index);
    p.rtsp_sub_path = fmt("/ch%u_sub", p.index);
    out.push_back(std::move(p));
  }

  return out;
}

}  // namespace d2o
