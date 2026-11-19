/**
 * @file tests/channel_plan_check.cpp
 * @brief 通道规划那一步的判据：**显式通道**怎么排号、谁被挡住、档位怎么展开、
 *        每通道的水印/延时字段有没有原样传下去。
 *
 * ## 与本文件旧版最大的差别：**不再自动发现显示器**
 *
 * 旧版会把每一块"没被显式项覆盖"的可捕获显示器自动建成一个通道。新版的
 * 产品模型是 NVR 式的：**默认安装只有通道 1**（本机桌面），别的屏/摄像头由
 * 用户在「通道管理」里显式添加。"自动发现"会让"有几块屏就有几个通道"这件事
 * 在界面上看不出来，所以整段规则被删掉了。
 *
 * 因此本文件的重点从"自动那半会不会多建"变成两件事：
 *
 *   1. **通道号是稳定的** —— 取 `ChannelConfig::number`，删一项不会让后面
 *      整体前移（端口 `web.port + number`、RTSP 路径、NVR 书签都挂着它）。
 *   2. **建不了的显式项要说清原因** —— 同一块屏被占、屏不存在、屏抓不了，
 *      三种都要 `blocked_reason` 非空，且仍然占号。
 *
 * 编译（Linux 上也能跑，本文件不碰 DXGI）：
 *
 *   g++ -std=c++17 -Isrc -Ithird_party/uvcpp/include-ext \
 *       tests/channel_plan_check.cpp src/app/channel_plan.cpp src/common/config.cpp \
 *       -o /tmp/channel_plan_check
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "app/channel_plan.h"
#include "common/config.h"

using namespace d2o;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, std::string const& what,
           std::string const& hint = std::string()) {
  if (ok) {
    ++g_pass;
    std::printf("[OK]   %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s\n", what.c_str());
    if (!hint.empty()) std::printf("       %s\n", hint.c_str());
  }
}

std::string num(long long v) { return std::to_string(v); }

ChannelDisplayInfo disp(unsigned idx, bool capturable = true) {
  ChannelDisplayInfo d;
  d.index = idx;
  d.description = "屏幕 #" + num(idx);
  d.width = 1920;
  d.height = 1080;
  d.x = static_cast<int>(idx) * 1920;
  d.y = 0;
  d.capturable = capturable;
  return d;
}

ChannelConfig item_disp(unsigned idx, int number = 0) {
  ChannelConfig c;
  c.source = "display";
  c.display_index = idx;
  c.number = number;
  return c;
}

ChannelConfig item_cam(std::string camera, int number = 0) {
  ChannelConfig c;
  c.source = "webcam";
  c.camera = std::move(camera);
  c.number = number;
  return c;
}

/// 把计划拼成 `号:名字` 的一行，断言"整张表"时用。
std::string shape(std::vector<ChannelPlan> const& ps) {
  std::string s;
  for (ChannelPlan const& p : ps) {
    if (!s.empty()) s += " | ";
    s += num(p.index) + ":" + p.name;
    if (!p.enabled) s += p.blocked_reason.empty() ? "(关)" : "(挡)";
  }
  return s;
}

ChannelPlan const* find(std::vector<ChannelPlan> const& ps, unsigned index) {
  for (ChannelPlan const& p : ps) {
    if (p.index == index) return &p;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// [0] 量具自检
// ---------------------------------------------------------------------------
void test_instrument() {
  std::printf("\n[0] 量具自检\n");
  ChannelsConfig cfg;
  cfg.items.push_back(item_cam("cam-a", 2));
  cfg.items.push_back(item_disp(1, 3));
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
  check(ps.size() == 2, "两条显式项 → 两条计划（实际 " + shape(ps) + "）");
  check(find(ps, 2) != nullptr && find(ps, 3) != nullptr,
        "能按通道号查到 2、3 —— 查不到的话下面所有否定式断言都是假绿");
}

// ---------------------------------------------------------------------------
// [1] 默认安装：只有通道 1，一块屏都不自动建
// ---------------------------------------------------------------------------
void test_no_auto_discovery() {
  std::printf("\n[1] 默认安装：没有 items 就没有通道 2 起\n");
  {
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
    ChannelsConfig cfg;
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.empty(), "三块屏、没有显式项 → 0 条（实际 " + shape(ps) + "）");
  }
  {
    // 通道 1 抓摄像头时，桌面那块屏也不该自己变成一个通道 —— 要加就显式加。
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    ChannelsConfig cfg;
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.empty(), "--camera 之后也不自动建（实际 " + shape(ps) + "）");
  }
}

// ---------------------------------------------------------------------------
// [2] 稳定编号：取 number，不取数组下标
// ---------------------------------------------------------------------------
void test_stable_numbering() {
  std::printf("\n[2] 稳定编号\n");
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a", 5));
    cfg.items.push_back(item_disp(1, 2));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && ps[0].index == 5 && ps[1].index == 2,
          "号取配置里的 number（5、2），不按数组顺序重排（实际 " + shape(ps) + "）");
  }
  {
    // 没走过 validate_config 的手搓配置（number = 0）：退回下标 + 2。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a"));
    cfg.items.push_back(item_cam("cam-b"));
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && ps[0].index == 2 && ps[1].index == 3,
          "number=0 时退回下标 + 2（实际 " + shape(ps) + "）");
  }
  {
    // 删掉中间那项，后面那项的号不变 —— 这正是稳定编号的全部意义。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a", 2));
    cfg.items.push_back(item_cam("cam-c", 4));   // 3 号空着
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && ps[0].index == 2 && ps[1].index == 4,
          "空号不让人后面的通道前移（实际 " + shape(ps) + "）");
  }
}

// ---------------------------------------------------------------------------
// [3] 指向不存在 / 抓不了的屏：挡住，仍然占号
// ---------------------------------------------------------------------------
void test_bad_display() {
  std::printf("\n[3] 屏不存在 / 抓不了\n");
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(7, 2));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && !ps[0].enabled && ps[0].index == 2,
          "不存在的 #7 被挡住、仍然占 2 号（实际 " + shape(ps) + "）");
    check(ps[0].blocked_reason.find("#7") != std::string::npos,
          "原因点名 #7（实际 \"" + ps[0].blocked_reason + "\"）");
  }
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(1, 2));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1, /*capturable=*/false)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && !ps[0].enabled &&
              ps[0].blocked_reason.find("抓不到") != std::string::npos,
          "抓不了的屏被挡住、原因说'抓不到'（实际 \"" + ps[0].blocked_reason + "\"）");
  }
}

// ---------------------------------------------------------------------------
// [4] 显式项：关掉的占号、数值原样、档位展开
// ---------------------------------------------------------------------------
void test_explicit_items() {
  std::printf("\n[4] 显式项\n");
  {
    ChannelsConfig cfg;
    ChannelConfig off = item_disp(1, 2);
    off.enabled = false;
    cfg.items.push_back(off);
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && !ps[0].enabled && ps[0].index == 2 &&
              ps[0].blocked_reason.empty(),
          "用户关掉的项：enabled=false、blocked_reason=**空**、仍然占号（实际 " +
              shape(ps) + "）");
  }
  {
    ChannelsConfig cfg;
    ChannelConfig c = item_cam("cam-a", 2);
    c.quality = "custom";
    c.fps = 17;
    c.bitrate_kbps = 3333;
    c.max_height = 900;
    c.sub_enabled = false;
    c.rtsp_path = "cam";
    cfg.items.push_back(c);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 1 && ps[0].fps == 17 && ps[0].bitrate_kbps == 3333 &&
              ps[0].max_height == 900 && !ps[0].sub_enabled,
          "custom 档位的显式项原样保留（实际 " + num(ps[0].fps) + "fps/" +
              num(ps[0].bitrate_kbps) + "kbps）");
    check(ps[0].rtsp_path == "cam",
          "路径原样传过来，规划这一步不替 validate_config 补斜杠（实际 \"" +
              ps[0].rtsp_path + "\"）");
  }
  {
    ChannelsConfig cfg;
    ChannelConfig c = item_cam("cam-a", 2);
    c.quality = "high";
    cfg.items.push_back(c);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 1 && ps[0].fps == 30 && ps[0].bitrate_kbps == 8192 &&
              ps[0].max_height == 0,
          "「高性能」被展开成 30fps/8192kbps/max_height=0（实际 " +
              num(ps[0].fps) + "fps/" + num(ps[0].bitrate_kbps) + "/" +
              num(ps[0].max_height) + "）");
  }
}

// ---------------------------------------------------------------------------
// [5] 每通道独立字段：延时档位、水印、硬件标识
// ---------------------------------------------------------------------------
void test_per_channel_fields() {
  std::printf("\n[5] 每通道独立字段\n");
  ChannelsConfig cfg;
  ChannelConfig c = item_cam("cam-a", 2);
  c.latency = "realtime";
  c.gop_seconds = 99;   // 档位应当覆盖成 1
  c.threads = 99;       // 档位应当覆盖成 1
  c.hardware_id = "usb:vid_1bcf";
  c.hardware_serial = "SN12345";
  c.osd.enabled = false;
  c.osd.items[0].position = "lower_left";
  cfg.items.push_back(c);
  std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
  check(ps.size() == 1 && ps[0].threads == 1 && ps[0].gop_seconds == 1,
        "延时档位「极致低延迟」展开成 threads=1 / gop=1（实际 " +
            num(ps[0].threads) + "/" + num(ps[0].gop_seconds) + "）");
  check(!ps[0].osd.enabled && ps[0].osd.items[0].position == "lower_left",
        "这一路的水印原样进计划（不是全局那份）");
  check(ps[0].hardware_id == "usb:vid_1bcf" &&
            ps[0].hardware_serial == "SN12345",
        "硬件标识与序列号原样进计划");
}

// ---------------------------------------------------------------------------
// [6] 顺序稳定：同样的输入算两遍，逐字段相同
// ---------------------------------------------------------------------------
void test_order_stability() {
  std::printf("\n[6] 顺序稳定\n");
  ChannelsConfig cfg;
  cfg.items.push_back(item_cam("cam-a", 2));
  cfg.items.push_back(item_disp(1, 3));
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
  check(ps.size() == 2 && ps[0].index == 2 && ps[1].index == 3,
        "顺序与号（实际 " + shape(ps) + "）");

  std::vector<ChannelPlan> const again = plan_channels(d, cfg, 0, true);
  bool same = again.size() == ps.size();
  if (same) {
    for (std::size_t i = 0; i < ps.size(); ++i) {
      if (again[i].index != ps[i].index || again[i].name != ps[i].name ||
          again[i].rtsp_path != ps[i].rtsp_path) {
        same = false;
        break;
      }
    }
  }
  check(same, "同样的输入算两遍，结果逐字段相同");
}

// ---------------------------------------------------------------------------
// [7] 上限：规划层照实建出 items 的全部条目
// ---------------------------------------------------------------------------
void test_cap_boundary() {
  std::printf("\n[7] 上限只压在 items[] 上\n");
  ChannelsConfig cfg;
  for (std::size_t i = 0; i < ChannelsConfig::kMaxChannels; ++i) {
    cfg.items.push_back(
        item_cam("cam-" + num(static_cast<long long>(i)),
                 static_cast<int>(i) + 2));
  }
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
  check(ps.size() == ChannelsConfig::kMaxChannels,
        "满员 " + num(static_cast<long long>(ChannelsConfig::kMaxChannels)) +
            " 条 → 同样多条计划（实际 " + num(static_cast<long long>(ps.size())) +
            " 条）");
  bool unique = true;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    for (std::size_t j = i + 1; j < ps.size(); ++j) {
      if (ps[i].index == ps[j].index) unique = false;
    }
  }
  check(unique, "通道号互不重复（重号 = 两个通道抢同一个 ONVIF 端口）");
}

// ---------------------------------------------------------------------------
// [8] 同一块屏只给一路
// ---------------------------------------------------------------------------
void test_same_display_blocked() {
  std::printf("\n[8] 同一块屏只给一路\n");

  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(0, 2));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && ps[0].index == 2 && !ps[0].enabled,
          "撞通道 1 那块屏 → 仍然占 2 号、但不建（实际 " + shape(ps) + "）");
    check(!ps[0].blocked_reason.empty() &&
              ps[0].blocked_reason.find("显示器 #0") != std::string::npos,
          "原因点名 #0（实际 \"" + ps[0].blocked_reason + "\"）");
  }
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(0, 2));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.size() == 1 && ps[0].enabled && ps[0].blocked_reason.empty(),
          "--camera 之后通道 1 不抓屏 → 同一块屏照建（实际 " + shape(ps) + "）");
  }
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(1, 2));
    cfg.items.push_back(item_disp(1, 3));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && ps[0].enabled && !ps[1].enabled,
          "两条指同一块屏 → 前一条照跑、后一条被挡（实际 " + shape(ps) + "）");
    check(ps[1].index == 3 && !ps[1].blocked_reason.empty(),
          "被挡的那条照样占 3 号，且带原因");
  }
  {
    // 用户自己关掉的项不占屏：它本来就不抓。
    ChannelsConfig cfg;
    ChannelConfig off = item_disp(1, 2);
    off.enabled = false;
    cfg.items.push_back(off);
    cfg.items.push_back(item_disp(1, 3));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && !ps[0].enabled && ps[0].blocked_reason.empty() &&
              ps[1].enabled,
          "关掉的那条不算占用 → 同屏的另一条照建（实际 " + shape(ps) + "）");
  }
  {
    // 摄像头项的 display_index 默认是 0，不能拿它去占屏。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a", 2));
    cfg.items.push_back(item_disp(0, 3));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.size() == 2 && ps[0].enabled && ps[1].enabled,
          "摄像头项那个默认 0 不占屏（实际 " + shape(ps) + "）");
  }
}

// ---------------------------------------------------------------------------
// [9] 同一台摄像头只给一路
// ---------------------------------------------------------------------------
void test_same_camera_blocked() {
  std::printf("\n[9] 同一台摄像头只给一路\n");
  {
    ChannelsConfig cfg;
    ChannelConfig a = item_cam("cam-a", 2);
    a.hardware_id = "usb:cam-a";
    ChannelConfig b = item_cam("cam-a", 3);
    b.hardware_id = "usb:cam-a";
    cfg.items.push_back(a);
    cfg.items.push_back(b);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && ps[0].enabled && !ps[1].enabled,
          "同一个 hardware_id 的第二路被挡住（实际 " + shape(ps) + "）");
    check(ps[1].blocked_reason.find("一个摄像头同时只能被一路") !=
              std::string::npos,
          "原因说清了一个摄像头只能被一路打开");
  }
  {
    // 两台不同摄像头：都建。
    ChannelsConfig cfg;
    ChannelConfig a = item_cam("cam-a", 2);
    a.hardware_id = "usb:cam-a";
    ChannelConfig b = item_cam("cam-b", 3);
    b.hardware_id = "usb:cam-b";
    cfg.items.push_back(a);
    cfg.items.push_back(b);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && ps[0].enabled && ps[1].enabled,
          "两台不同摄像头都建（实际 " + shape(ps) + "）");
  }
  {
    // 老配置没有 hardware_id：空选择器 = 第一台，两路都空也冲突。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("", 2));
    cfg.items.push_back(item_cam("", 3));
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && ps[0].enabled && !ps[1].enabled,
          "两路都留空 = 都选第一台 → 第二路被挡（实际 " + shape(ps) + "）");
  }
  {
    // 关掉的那一路不占摄像头：它本来就不开。
    ChannelsConfig cfg;
    ChannelConfig off = item_cam("cam-a", 2);
    off.hardware_id = "usb:cam-a";
    off.enabled = false;
    ChannelConfig on = item_cam("cam-a", 3);
    on.hardware_id = "usb:cam-a";
    cfg.items.push_back(off);
    cfg.items.push_back(on);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 2 && !ps[0].enabled && ps[0].blocked_reason.empty() &&
              ps[1].enabled,
          "用户关掉的那一路不算占用（实际 " + shape(ps) + "）");
  }
}

}  // namespace

int main() {
  std::printf("=== 通道规划：显式通道 / 稳定编号 / 每通道字段 ===\n");
  test_instrument();
  test_no_auto_discovery();
  test_stable_numbering();
  test_bad_display();
  test_explicit_items();
  test_per_channel_fields();
  test_order_stability();
  test_cap_boundary();
  test_same_display_blocked();
  test_same_camera_blocked();
  std::printf("----\n通过 %d 项，失败 %d 项。\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
