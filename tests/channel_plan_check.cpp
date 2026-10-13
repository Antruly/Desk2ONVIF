/**
 * @file tests/channel_plan_check.cpp
 * @brief Phase AD 的主判据：**规划**那一步 —— 号怎么排、谁被跳过、档位怎么展开、
 *        以及"关掉的项还占不占号"。
 *
 * ## 为什么这个文件是必须的，而不是"顺手补一个单测"
 *
 * 这条开发机上**只有一块显示输出**（`--list-displays` 实测 1 个）。而
 * `plan_channels` 里那张自动发现的表，在单屏机器上永远只走
 * "只有通道 1 那块屏 → 一条都不建" 这一条路径 —— 也就是说，**多屏机器上那段
 * 代码一次都没被跑过**。它错成什么样，本地一行日志都看不出来，而症状在用户那边
 * 是"插上第二块屏之后多出来一个连不上的通道"或者"端口撞了、通道 2 起不来"。
 *
 * 所以这里拿**手搓的显示器清单**（0/1/3 块、可捕获/不可捕获、被显式项覆盖/
 * 没被覆盖、被关掉的项覆盖）逐条钉住。这些输入在本机造不出来，但它们是纯数据
 * —— 这正是 `plan_channels` 被抽成"不碰 DXGI 的纯函数"的全部理由（见头文件）。
 *
 * ## 边界：这个文件**不**验的东西
 *
 *   - 运行期行为（线程、端口绑定、RTSP 路径撞车时的降级）→ `tests\multichannel_e2e.ps1`
 *   - 配置层（`items[]` 的合法性、notes、落盘形状）→ `tests\channels_check.cpp`
 *
 * 编译（在 D:\tools\Desk2ONVIF 下，PATH 里要有 C:\msys64\mingw64\bin，
 * 否则 cc1 会静默死掉，陷阱 18）：
 *
 *   g++ -std=c++17 -Isrc -Ithird_party/uvcpp/include-ext \
 *       tests/channel_plan_check.cpp src/app/channel_plan.cpp src/common/config.cpp \
 *       -o build/channel_plan_check.exe
 *
 * `config.cpp` 要链进来是因为 `channel_plan.cpp` 调了 `find_quality_tier`
 * （档位表住在配置层）。**不需要 libdesk2onvif_core**：这条路一个 DXGI、一个
 * 编码器、一个套接字都不碰 —— 而链接行动辄二十个库直接决定了这个测试会不会
 * 被随手跑（同 `channels_check.cpp` 开头那条）。
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

/// 一台显示器。`capturable` 默认 true —— 绝大多数用例关心的是"排号"，
/// 而抓不了的那一类自有它自己的那一段。
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

ChannelConfig item_disp(unsigned idx) {
  ChannelConfig c;
  c.source = "display";
  c.display_index = idx;
  return c;
}

ChannelConfig item_cam(std::string camera) {
  ChannelConfig c;
  c.source = "webcam";
  c.camera = std::move(camera);
  return c;
}

/// 把计划拼成 `号:名字` 的一行，断言"整张表"时用 —— 逐字段断言在"顺序错了但
/// 内容对"这类故障上会给出十几条红，而一张表一眼就看出是哪一位错位了。
std::string shape(std::vector<ChannelPlan> const& ps) {
  std::string s;
  for (ChannelPlan const& p : ps) {
    if (!s.empty()) s += " | ";
    s += num(p.index) + ":" + p.name;
    if (!p.enabled) s += p.blocked_reason.empty() ? "(关)" : "(挡)";
    if (p.auto_created) s += "(自动)";
  }
  return s;
}

/// 数一下有多少条 `enabled`。
std::size_t enabled_count(std::vector<ChannelPlan> const& ps) {
  std::size_t n = 0;
  for (ChannelPlan const& p : ps) {
    if (p.enabled) ++n;
  }
  return n;
}

/// 找通道号是 `index` 的那一条。找不到返回 nullptr。
ChannelPlan const* find(std::vector<ChannelPlan> const& ps, unsigned index) {
  for (ChannelPlan const& p : ps) {
    if (p.index == index) return &p;
  }
  return nullptr;
}

// ---------------------------------------------------------------------------
// [0] 量具自检（陷阱 82 / 85）
// ---------------------------------------------------------------------------
//
// 下面每一条断言都建立在"`plan_channels` 真的按它的规则在建表"之上。它要是
// 恒返回空表（比如循环条件写反了），那"没有多余通道"那一整族断言会**全绿**而
// 实际上什么都没验。所以先拿一个**已知非空**的输入把表撑起来，证明这把尺子
// 量得出东西。
void test_instrument() {
  std::printf("\n[0] 量具自检\n");
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  ChannelsConfig cfg;
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
  check(!ps.empty(), "量具自检：三块屏、通道 1 占一块 → 计划表**非空**",
        "空表的话，下面所有'没有多余通道'的断言都会假绿");
  check(ps.size() == 2,
        "量具自检：于是正好两条（实际 " + num(static_cast<long long>(ps.size())) +
            " 条）—— " + shape(ps));
  check(find(ps, 2) != nullptr,
        "量具自检：里面真的能按通道号查到第 2 号 —— 查不到的话 find() 这把尺子坏了，"
        "下面拿 nullptr 做的否定式断言全是假的绿");
}

// ---------------------------------------------------------------------------
// [1] 只有一块屏（本机就是这一种）：一条都不建
// ---------------------------------------------------------------------------
//
// 这一段同时是**兼容红线**：0.1.6 及以前，单屏机器上就只有通道 1。自动发现要是
// 在"通道 1 占着唯一那块屏"时还建出一条来，每一个已经装了的老用户开机会多出
// 一个 8081 端口和一条永远一样的第二路流。
void test_single_display() {
  std::printf("\n[1] 单屏（本机的形状）：不多建\n");
  {
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    ChannelsConfig cfg;
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.empty(),
          "1 块屏 + 通道 1 抓它 → 0 条（实际 " + num(static_cast<long long>(ps.size())) +
              " 条）—— " + shape(ps));
  }
  {
    // 通道 1 改抓摄像头（`--camera`）：桌面那块屏于是没人管，应当补一条出来。
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    ChannelsConfig cfg;
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, false);
    check(ps.size() == 1 && ps[0].index == 2,
          "--camera 之后通道 1 不是抓屏的 → 那块屏补成通道 2（实际 " + shape(ps) + "）");
  }
  {
    // 一块屏都没有（纯远程会话、被拔了）：表是空的，不许崩。
    std::vector<ChannelDisplayInfo> d;
    ChannelsConfig cfg;
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.empty(), "一块屏都没有 → 0 条，不崩");
  }
}

// ---------------------------------------------------------------------------
// [2] 三块屏：自动建两条，号从 2 起，名字用**显示器**的序号
// ---------------------------------------------------------------------------
void test_three_displays() {
  std::printf("\n[2] 三块屏：自动建两条\n");
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  ChannelsConfig cfg;
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);

  check(ps.size() == 2 && ps[0].index == 2 && ps[1].index == 3,
        "号是 2、3（实际 " + shape(ps) + "）");

  // 名字里的序号用**显示器的**（`--list-displays` 报的那个 #N），不是通道号。
  // 写成通道号的话，用户按界面上那句去 `--display N` 会指到一块别的屏上。
  check(ps[0].name == "显示器 #1" && ps[1].name == "显示器 #2",
        "名字用的是显示器序号（#1、#2），不是通道号（实际 \"" + ps[0].name + "\" / \"" +
            ps[1].name + "\"）");

  check(ps[0].auto_created && ps[1].auto_created,
        "两条都标了 auto_created —— 界面上要能区分'我配的'和'它自己冒出来的'");
  check(ps[0].item_index == static_cast<std::size_t>(-1),
        "自动项的 item_index 是 npos，不是 0（拿 0 去索引 items[] 会读到用户配的第一条）");
  check(ps[0].source == "display" && ps[0].display_index == 1,
        "来源是 display、display_index 指向那块屏本身");
  check(ps[0].rtsp_path == "/ch2" && ps[0].rtsp_sub_path == "/ch2_sub",
        "默认路径按通道号补（实际 \"" + ps[0].rtsp_path + "\"）");

  // 档位：默认「流畅」，而且那个档位被**展开成了具体数值**。
  //
  // ⚠️ **期望值从档位表里读，不写死。** 写死 20/1024/1080 的话这条断言是假绿的：
  // `ChannelPlan` 的结构体默认值**恰好就是「流畅」档的那组数**，于是"漏调
  // `expand_quality`"这个缺陷在这组输入上表现完全一样（实测 —— 变异 m7 就是这么
  // 活下来的）。从表里读之后，"表改了而展开没跟上"才会红：那正是这条规则的
  // 真实失败方式（`channel_plan.cpp` 里那段注释说的"迟早对不上"）。
  QualityTier const* const smooth = find_quality_tier("smooth");
  check(smooth != nullptr, "档位表里有「流畅」这一档（没有的话下面那条无从谈起）");
  if (smooth != nullptr) {
    check(ps[0].quality == "smooth" && ps[0].fps == smooth->fps &&
              ps[0].bitrate_kbps == smooth->bitrate_kbps &&
              ps[0].max_height == smooth->max_height &&
              ps[0].sub_enabled == smooth->sub_enabled,
          "自动建的那一路用的是「流畅」档、且数值与该档位表**逐项一致**（实际 " +
              num(ps[0].fps) + "fps/" + num(ps[0].bitrate_kbps) + "kbps/" +
              num(ps[0].max_height) + "，表里是 " + num(smooth->fps) + "fps/" +
              num(smooth->bitrate_kbps) + "kbps/" + num(smooth->max_height) + "）",
          "这一条只有在\"表改了而 expand_quality 没跟上\"时才会红 —— 见上面那段");
  }
  check(ps[0].enabled, "自动建出来的通道默认是开的（用户要的是\"默认开启、自动的\"）");
}

// ---------------------------------------------------------------------------
// [3] 抓不了的屏：一条都不建
// ---------------------------------------------------------------------------
//
// 向日葵的虚拟屏就是这一类（见 memory 里那条 DXGI 实测）。给它建一路的结果是
// "这个通道永远黑屏"，而用户会去查摄像头或者网络 —— 因为界面上写着通道是好的。
void test_uncapturable() {
  std::printf("\n[3] 抓不了的屏不建通道\n");
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1, /*capturable=*/false),
                                       disp(2)};
  ChannelsConfig cfg;
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
  check(ps.size() == 1 && ps[0].index == 2 && ps[0].display_index == 2,
        "中间那块抓不了 → 只建它后面那块，且**号不跳过**（实际 " + shape(ps) + "）");
  check(find(ps, 3) == nullptr, "抓不了的那块没有留下一张空位");

  // 号是"最后一条之后接着排"，不是"按显示器序号算"：两块能抓的屏里只有一块
  // 建成了，它仍然拿 2 号。这一点值得钉住 —— 用户勾掉一个通道之后，后面那些
  // 通道的端口和 RTSP 路径**不该**整体挪一格。
  check(ps[0].index == 2,
        "建出来的那一条拿的是 2 号（实际 " + num(ps[0].index) + "）");
}

// ---------------------------------------------------------------------------
// [4] 显式项：占号、能被覆盖、关掉的也占号
// ---------------------------------------------------------------------------
void test_explicit_items() {
  std::printf("\n[4] 显式项与自动发现的交界\n");

  // 4a：用户显式配了显示器 #1，自动发现不该再建一条。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(1));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && ps[0].index == 2 && !ps[0].auto_created,
          "显式配了显示器 #1 → 只有那一条，没有第二条重复的（实际 " + shape(ps) + "）");
  }

  // 4b：**关掉的项也算覆盖。** 这是"用户关掉了一块屏"的写法 —— 把它当成"没覆盖"
  // 就会把用户刚关掉的通道原样又建回来，而界面上那条项明明白白写着已关闭。
  {
    ChannelsConfig cfg;
    ChannelConfig c = item_disp(1);
    c.enabled = false;
    cfg.items.push_back(c);
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && !ps[0].enabled && ps[0].index == 2,
          "关掉的项照样占号、照样覆盖，且**不会被自动发现又建回来**（实际 " +
              shape(ps) + "）");
    check(enabled_count(ps) == 0,
          "于是这条计划里一条启用的都没有（自动那半没偷偷补一条）");
  }

  // 4c：**摄像头项不参与认领显示器。** 那个 `display_index` 对摄像头项没有意义
  // （默认是 0），拿它认领会让"配了一路摄像头"顺手把显示器 #0 也吞掉。
  //
  // ⚠️ **形状要挑对，否则这段什么都没验。** 通道 1 抓屏时 `display_index` 默认的
  // 0 恰好就是通道 1 那块屏，规则 1 先把它跳过了 —— 于是"按 display_index 认领"
  // 这条错误的写法在这种输入下**表现完全一样**（这段会假绿）。真正能分辨的是
  // **通道 1 自己在抓摄像头**（`--camera`）：那时显示器 #0 是个正常的候选，
  // 而摄像头项里那个默认的 0 会把它整块吞掉。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("FHD Camera"));   // display_index 默认 0
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.size() == 3 && ps[0].source == "webcam" && ps[1].auto_created &&
              ps[1].display_index == 0 && ps[2].display_index == 1,
          "通道 1 抓摄像头时配了一路摄像头 → 两块屏都照样自动建（实际 " + shape(ps) +
              "）");
  }
  {
    // 同一个输入，通道 1 换成抓屏：这时 #0 归通道 1，只剩 #1 要自动建。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("FHD Camera"));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && ps[1].auto_created && ps[1].display_index == 1,
          "通道 1 抓屏时 → 只剩显示器 #1 自动建（实际 " + shape(ps) + "）");
  }

  // 4d：号从**全部**显式项之后接着排（含关掉的）。于是"关掉通道 3"不会让
  // 通道 4 变成通道 3 —— 序号可能出现空档，而空档不伤人；反过来整体挪一格会
  // 伤到 NVR 上那些按路径存的书签。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a"));           // 通道 2
    ChannelConfig off = item_disp(9);
    off.enabled = false;
    cfg.items.push_back(off);                          // 通道 3，关着
    cfg.items.push_back(item_disp(8));                 // 通道 4
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 4, "四条计划（实际 " + shape(ps) + "）");
    check(ps[3].auto_created && ps[3].index == 5,
          "自动发现的那条从**全部显式项之后**接着排，拿到 5 号（实际 " +
              num(ps[3].index) + "）—— 而不是补进 3 号那个空档");
    check(off.enabled == false && ps[1].index == 3,
          "关掉的那条仍然占着 3 号");
  }

  // 4e：显式项的数值**原样保留**，不被档位展开改掉。用户在配置里写死的
  // fps/码率必须活到计划里 —— 展开是幂等的（值本来就等于档位值），但
  // `custom` 档位时压根不该动。
  {
    ChannelsConfig cfg;
    ChannelConfig c = item_cam("cam-a");
    c.quality = "custom";
    c.fps = 17;
    c.bitrate_kbps = 3333;
    c.max_height = 900;
    c.sub_enabled = false;
    c.rtsp_path = "cam";            // 没有前导斜杠，看它会不会被补
    cfg.items.push_back(c);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 1 && ps[0].fps == 17 && ps[0].bitrate_kbps == 3333 &&
              ps[0].max_height == 900 && !ps[0].sub_enabled,
          "custom 档位的显式项原样保留（实际 " + num(ps[0].fps) + "fps/" +
              num(ps[0].bitrate_kbps) + "kbps）");
    // **路径不在这里补斜杠**：补斜杠是 `validate_config` 的活，而这个函数是
    // 纯的 —— 在这里补就会让"配置里写的"和"计划里的"有两份规则。这一条钉住的
    // 正是那条分工。
    check(ps[0].rtsp_path == "cam",
          "路径原样传过来，规划这一步不替 validate_config 补斜杠（实际 \"" +
              ps[0].rtsp_path + "\"）");
  }

  // 4f：显式项里写死的档位标签（非 custom）会被展开成具体值。
  {
    ChannelsConfig cfg;
    ChannelConfig c = item_cam("cam-a");
    c.quality = "high";
    cfg.items.push_back(c);
    std::vector<ChannelPlan> const ps = plan_channels({}, cfg, 0, true);
    check(ps.size() == 1 && ps[0].fps == 30 && ps[0].bitrate_kbps == 8192 &&
              ps[0].max_height == 0,
          "显式项的「高性能」被展开成 30fps/8192kbps/max_height=0（实际 " +
              num(ps[0].fps) + "fps/" + num(ps[0].bitrate_kbps) + "/" +
              num(ps[0].max_height) + "）");
  }
}

// ---------------------------------------------------------------------------
// [5] 顺序与稳定性
// ---------------------------------------------------------------------------
//
// "顺序即通道号"是整个多通道的地基：端口是 `web.port + index`，RTSP 路径默认
// 也是按 index 合成的。这个函数要是哪天把两块表合并的次序换了（比如先自动后
// 显式），号会整体错位，而**每一路自己看起来都是对的** —— 只有把整张表摆出来
// 才看得出来。
void test_order_stability() {
  std::printf("\n[5] 顺序即通道号\n");
  ChannelsConfig cfg;
  cfg.items.push_back(item_cam("cam-a"));
  cfg.items.push_back(item_disp(5));
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);

  // 四条：两条显式的，加上 #1、#2 两块没被覆盖的屏（#0 归通道 1）。
  check(ps.size() == 4, "四条计划（实际 " + shape(ps) + "）");
  check(ps[0].index == 2 && ps[0].source == "webcam",
        "第 1 位是显式项里的摄像头（通道 2）");
  check(ps[1].index == 3 && ps[1].display_index == 5,
        "第 2 位是显式项里的显示器 #5（通道 3）");
  check(ps[2].index == 4 && ps[2].auto_created && ps[2].display_index == 1,
        "第 3 位才是自动发现的 #1（通道 4）—— 显式永远排在自动前面");
  check(ps[3].index == 5 && ps[3].auto_created && ps[3].display_index == 2,
        "第 4 位是自动发现的 #2（通道 5）—— 自动那半按显示器序号排");

  // 同样的输入再算一遍，必须逐字段相同。计划是**纯函数**，而"两份对同一个
  // 输入给出不同的表"在多通道里的症状是重启之后端口/路径换了个位置。
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
// [6] 上限只压在 `items[]` 上，自动发现那半不被它掐
// ---------------------------------------------------------------------------
//
// ⚠️ **这一段的结论是"规划这一步不截断"，不是"总通道数有上限 8"。**
// `ChannelsConfig::kMaxChannels` 压的是**配置里那一列 `items[]`**（见
// `validate_config` 里那句 resize），而自动发现的通道是运行期按物理屏算出来的 ——
// 规划这一层**再压一道**只会造出第二个上限，且它比配置层那个更难解释
// （"我明明还能配 8 条，怎么插第二块屏就少了一条"）。
//
// 于是这里钉住的是那条分工：截断归 `validate_config`（`channels_check.cpp`
// 有它自己那一段），规划照实把表建出来。真要担心"自动建出来的太多把机器按住"，
// 该改的是 `kMaxChannels` 的语义，而不是在这里悄悄丢掉两块屏。
void test_cap_boundary() {
  std::printf("\n[6] 上限只压在 items[] 上\n");
  ChannelsConfig cfg;
  // 塞到配置层允许的最满：kMaxChannels 条显式项 ⇒ 通道 2 … 2+kMax-1。
  for (std::size_t i = 0; i < ChannelsConfig::kMaxChannels; ++i) {
    cfg.items.push_back(item_cam("cam-" + num(static_cast<long long>(i))));
  }
  std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
  std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);

  // 满员 8 条显式 + 2 条自动（#0 归通道 1，剩下 #1、#2）= 10 条计划。
  check(ps.size() == ChannelsConfig::kMaxChannels + 2,
        "满员的 " + num(static_cast<long long>(ChannelsConfig::kMaxChannels)) +
            " 条显式项 + 2 条自动 = 10 条计划（实际 " +
            num(static_cast<long long>(ps.size())) + " 条）");
  // 最后一条显式项 = 通道 2 + 8 - 1 = 9。
  check(ps[ChannelsConfig::kMaxChannels - 1].index == 9 &&
            !ps[ChannelsConfig::kMaxChannels - 1].auto_created,
        "最后一条显式项拿通道号 9（= 2 + 8 - 1，实际 " +
            num(ps[ChannelsConfig::kMaxChannels - 1].index) + "）");
  // 两条自动的跟在后面：10、11。
  check(ps[ChannelsConfig::kMaxChannels].auto_created &&
            ps[ChannelsConfig::kMaxChannels].index == 10 &&
            ps.back().auto_created && ps.back().index == 11,
        "自动发现的两条排在显式项之后、拿到 10 和 11（实际 " +
            num(ps[ChannelsConfig::kMaxChannels].index) + " 和 " +
            num(ps.back().index) + "）");
  // 号连续、不重复、不回绕 —— 端口是 `web.port + index`，重号就是两个通道
  // 抢同一个 ONVIF 端口，而那个故障在用户那边是"有一路怎么都加不进来"。
  bool unique = true;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    for (std::size_t j = i + 1; j < ps.size(); ++j) {
      if (ps[i].index == ps[j].index) unique = false;
    }
  }
  check(unique, "十条计划的通道号互不重复（重号 = 两个通道抢同一个 ONVIF 端口）");
}

// ---------------------------------------------------------------------------
// [7] 规则 2：同一块屏只给一路
// ---------------------------------------------------------------------------
//
// ## 这条规则为什么非有不可（它是实测撞出来的，不是设计出来的）
//
// DXGI 的桌面复制在一块输出上同时只允许一个 `IDXGIOutputDuplication`：第二个
// `DuplicateOutput` 失败，报 `E_INVALIDARG`。而**谁先谁后是线程赛跑** ——
// 通道 2 的采集线程在 `main.cpp` 里先起，通道 1 的 `session.run()` 后跑，
// 两边都可能赢。实测（out\mc-e2e\run8.txt）：通道 2 赢了，于是**通道 1** 的
// `DuplicateOutput` 失败 → `run()` 返回 Failed → 整个进程退出码 1 →
// **服务直接没了**。`run7.txt` 是同一个脚本同一份配置，那一趟活下来了。
//
// 也就是说，一个写错的 `display_index` 有大约一半的概率把整个服务弄没，
// 而两种结果都不说原因（"参数非法"四个字里读不出"两块屏写重了"）。
// 所以拦在**规划**这一步：不可能的那一路直接不建，并把原因写出来。
//
// ## 这一段的形状为什么必须是"纯函数"
//
// 上面那个 50% 是赛跑，**在验收脚本里没法稳定重现** —— `multichannel_e2e.ps1`
// 第三幕只能验"服务起来了 + 那句话说出口了"。真正把规则钉死的只能是这里。
void test_same_display_blocked() {
  std::printf("\n[7] 规则 2：同一块屏只给一路\n");

  // 7a：显式项写了通道 1 正在采的那块屏。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(0));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 1 && ps[0].index == 2,
          "撞通道 1 那块屏的显式项**仍然占 2 号**（实际 " + shape(ps) +
              "）—— 号一挪，NVR 上按路径存的书签就跟着挪");
    check(!ps[0].enabled, "但它不建（实际 " + shape(ps) + "）");
    check(!ps[0].blocked_reason.empty(),
          "而且带着一句给用户看的原因（空的话界面上只剩'已关闭'，用户会去配置里"
          "找一个明明是开着的开关）");
    check(ps[0].blocked_reason.find("显示器 #0") != std::string::npos,
          "原因里点名是哪一块屏（实际 \"" + ps[0].blocked_reason + "\"）");
  }

  // 7b：**判别式** —— 同一块屏但通道 1 不在抓屏（`--camera`）。这一条是拿来看
  // "代码有没有真的读 `channel1_is_display`"的：把那个开关当常数 true 用，
  // 7a 照样绿，只有这一条会红。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(0));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.size() == 1 && ps[0].enabled && ps[0].blocked_reason.empty(),
          "--camera 之后通道 1 不抓屏了 → 同一个 display_index 照建（实际 " +
              shape(ps) + "）");
  }

  // 7c：两条显式项指同一块屏 —— 挡住后面那条，前面那条照跑。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(1));
    cfg.items.push_back(item_disp(1));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && ps[0].enabled && !ps[1].enabled,
          "两条指同一块屏 → 前面那条照跑、后面那条被挡（实际 " + shape(ps) + "）");
    check(ps[1].index == 3, "被挡的那条**照样占号**（拿到 3，实际 " +
                                num(ps[1].index) + "）");
    check(!ps[1].blocked_reason.empty() &&
              ps[1].blocked_reason.find("显示器 #1") != std::string::npos,
          "原因里点名那块屏（实际 \"" + ps[1].blocked_reason + "\"）");
    // 被挡的那条不再参与"认领"，于是自动发现那半不会有任何变化：显示器 #1
    // 本来就被 7c 的第一条显式项覆盖了 —— 这一条钉的是"被挡住 ≠ 这块屏没人管"。
    check(!ps[1].auto_created && ps[0].index == 2,
          "挡住一条不会让自动发现多冒一条出来（实际 " + shape(ps) + "）");
  }

  // 7d：**摄像头项不会被这条规则误伤。** 摄像头没有"屏"可言，而它的
  // `display_index` 默认是 0 —— 拿它去比对就会把跟在后面的显示器项挡掉，
  // 而症状是"我配了两路，第二路从来没起过"。
  //
  // ⚠️ **形状要挑对。** 通道 1 抓屏时，它那块屏（默认 0）本来就先被跳过了，
  // 于是"摄像头项也参与占屏"这种错误写法在这个形状下表现完全一样（会假绿）。
  // 能分辨的是 `channel1_is_display = false`：那时显示器 #0 是个正常候选，
  // 而摄像头项里那个默认的 0 会把它整块吞掉。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a"));   // display_index 默认 0
    cfg.items.push_back(item_disp(0));
    std::vector<ChannelDisplayInfo> d = {disp(0)};
    std::vector<ChannelPlan> const ps =
        plan_channels(d, cfg, 0, /*channel1_is_display=*/false);
    check(ps.size() == 2 && ps[0].enabled && ps[1].enabled &&
              ps[1].blocked_reason.empty(),
          "摄像头项那个默认的 display_index=0 不占屏 → 排在它后面的显示器 #0 "
          "照样建（实际 " + shape(ps) + "）");
  }
  {
    // 两路摄像头并列：一个都不该被挡（它们的 display_index 都是 0）。
    ChannelsConfig cfg;
    cfg.items.push_back(item_cam("cam-a"));
    cfg.items.push_back(item_cam("cam-b"));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 3 && ps[0].enabled && ps[1].enabled,
          "两路摄像头并列都不被挡；#0 归通道 1，只剩 #1 自动建（实际 " +
              shape(ps) + "）");
  }

  // 7e：用户**自己关掉**的那条不占屏。它本来就不抓，没有 duplication 可言；
  // 而"关掉的项照样覆盖"是规则 3 的事（防自动发现把它建回来），两件事。
  // 这一条同时钉住 `blocked_reason` 与 `enabled:false` 是**两种**状态：
  // 前者非空 = 建不了，前者空 = 你自己关的。
  {
    ChannelsConfig cfg;
    ChannelConfig off = item_disp(1);
    off.enabled = false;
    cfg.items.push_back(off);          // 通道 2，用户关的
    cfg.items.push_back(item_disp(1)); // 通道 3，同屏但开着
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 0, true);
    check(ps.size() == 2 && !ps[0].enabled && ps[0].blocked_reason.empty() &&
              ps[1].enabled,
          "用户关掉的那条不算占用 → 同屏的另一条照建；而且关掉那条的 "
          "blocked_reason 是**空**的（实际 " + shape(ps) + "）");
  }

  // 7f：三块屏、通道 1 抓 #1，显式项把 #2 写了两遍 —— 挡一条，而 #0 由自动
  // 发现补上。整张表摆出来看：号是 2/3/4，被挡的那条在中间。
  {
    ChannelsConfig cfg;
    cfg.items.push_back(item_disp(2));
    cfg.items.push_back(item_disp(2));
    std::vector<ChannelDisplayInfo> d = {disp(0), disp(1), disp(2)};
    std::vector<ChannelPlan> const ps = plan_channels(d, cfg, 1, true);
    check(ps.size() == 3 && ps[0].index == 2 && ps[1].index == 3 &&
              ps[2].index == 4 && ps[2].auto_created &&
              ps[2].display_index == 0,
          "三块屏、通道 1 抓 #1：显式 #2 两条（后一条被挡）+ 自动补 #0 → "
          "整张表是 2/3/4（实际 " + shape(ps) + "）");
    check(ps[0].enabled && !ps[1].enabled && ps[2].enabled,
          "启用情况：建 / 挡 / 建（实际 " + shape(ps) + "）");
  }
}

}  // namespace

int main() {
  std::printf("=== 多通道规划：纯函数那一步（Phase AD）===\n");
  test_instrument();
  test_single_display();
  test_three_displays();
  test_uncapturable();
  test_explicit_items();
  test_order_stability();
  test_cap_boundary();
  test_same_display_blocked();
  std::printf("----\n通过 %d 项，失败 %d 项。\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
