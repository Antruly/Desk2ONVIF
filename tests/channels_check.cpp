/**
 * @file tests/channels_check.cpp
 * @brief Phase AC 的主判据：多通道**配置层** —— 兼容边界、整数组替换、逐项展开、
 *        路径与显示器去重、截断，以及"哪些话必须说出来"。
 *
 * ## 为什么要有这个文件（而不是只扩 `tests/web_contract.py`）
 *
 * `validate_config` 的 notes **在 HTTP 上根本看不见** —— `apply_config` 把它们
 * 送进 `D2O_WARN` 就没了（见 `src/main.cpp` 里那个循环）。这不是疏漏，是既有
 * 的分工：HTTP 那层报的是**结果**（某个值变成了什么），notes 报的是**为什么** ——
 * 而"为什么"那一半正是用户对着"我明明选了流畅"找线索时要看的东西。
 * 于是"去重的时候必须说一句话"这条承诺，从浏览器那边**验不到**，只能像
 * `tests/latency_tier_check.cpp` 的 [2] 段那样直接调 `validate_config`。
 *
 * 两个文件各管一半，缺一条都会让另一半变成没人验的承诺：
 *
 *   - 这里：notes 的内容、边角情形、`channels` 键的省略、JSON 形状错误。
 *   - `web_contract.py` 的 [2c]：**真的从 HTTP 走一圈** —— 网页写两条通道、
 *     `GET /api/config` 读回来、`channels` 键在空的时候不出现。
 *
 * ## 最要紧的一条：空 `items` ⇒ 落盘文本里**没有** `channels` 这个键
 *
 * `config_to_json` 的输出被保存路径和 `diff_config` 直接比较，多一个空数组就会
 * 让**每台已装机器的配置在第一次启动时报一次"配置变了"**，而这件事在开发机上
 * 永远看不到（那里总是先被写进过通道）。所以它排在第一段。
 *
 * 编译（在 D:\tools\Desk2ONVIF 下，PATH 里要有 C:\msys64\mingw64\bin，
 * 否则 cc1 会静默死掉，陷阱 18）：
 *
 *   g++ -std=c++17 -Isrc -Ithird_party/uvcpp/include-ext \
 *       tests/channels_check.cpp src/common/config.cpp -o build/channels_check.exe
 *
 * **不需要** libdesk2onvif_core：配置层只依赖 `config.cpp` 自己，而 `fmt()` 是
 * `log.h` 里的模板（头文件就够）。链接行动辄二十个库（`latency_tier_check.cpp`
 * 那样）在这里是纯浪费 —— 而"链接行短"直接决定了这个测试会不会被随手跑。
 */

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

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

/// 产品的那条路：validate → 落盘文本 → 读回 → 再 validate。
struct Once {
  AppConfig cfg;                     ///< 读回来那一份
  std::string text;                  ///< 第一次 validate 之后的落盘文本
  std::string text_again;            ///< 读回并再 validate 之后的落盘文本
  bool parsed = false;
  std::string err;                   ///< config_from_json 的报错
  std::vector<std::string> notes;    ///< 第一次的
  std::vector<std::string> notes2;   ///< 第二次的
};

Once cycle(AppConfig in) {
  Once o;
  std::vector<std::string> n1;
  validate_config(in, n1);
  // validate 之后才序列化 —— 这正是 `apply_config` 的顺序（validate → save）。
  o.text = config_to_json(in, /*redact_secrets=*/false);
  o.notes = n1;
  if (!config_from_json(o.text, o.cfg, o.err)) return o;
  o.parsed = true;
  std::vector<std::string> n2;
  validate_config(o.cfg, n2);
  o.notes2 = n2;
  o.text_again = config_to_json(o.cfg, /*redact_secrets=*/false);
  return o;
}

bool says(std::vector<std::string> const& notes, std::string const& needle) {
  for (std::string const& n : notes) {
    if (n.find(needle) != std::string::npos) return true;
  }
  return false;
}

std::string join(std::vector<std::string> const& notes) {
  std::string s;
  for (std::string const& n : notes) {
    if (!s.empty()) s += "\n         ";
    s += n;
  }
  return s;
}

/// 一项显示器通道。`path` 空 = 用默认路径。
ChannelConfig disp(unsigned idx, std::string path = std::string()) {
  ChannelConfig c;
  c.source = "display";
  c.display_index = idx;
  c.rtsp_path = std::move(path);
  return c;
}

ChannelConfig cam(std::string camera, std::string path) {
  ChannelConfig c;
  c.source = "webcam";
  c.camera = std::move(camera);
  c.rtsp_path = std::move(path);
  return c;
}

/// 数一下文本里出现过几次 `"key"`（顶层键）。**按文本数而不是解析后数**：
/// 这一段验的恰恰是文本本身。
int count_key(std::string const& text, std::string const& key) {
  std::string const pat = "\"" + key + "\"";
  int n = 0;
  for (std::size_t p = text.find(pat); p != std::string::npos;
       p = text.find(pat, p + 1)) {
    ++n;
  }
  return n;
}

// ---------------------------------------------------------------------------
// [0] 量具自检（陷阱 82）
// ---------------------------------------------------------------------------
//
// 下面每一条断言都建立在"`cycle` 真的把配置转了一圈"之上。它要是恒假
// （比如 `config_from_json` 一直在返回 false，于是 `o.cfg` 是个默认构造的对象、
// 而引用它的断言全在读默认值），那整片红绿都不可信。所以先拿一个**已知会变**的
// 输入证明这条路通。
void test_instrument() {
  std::printf("\n[0] 量具自检\n");
  AppConfig c;
  c.capture.fps = 37;               // 37 不是任何档位的值，但也不是非法值
  c.capture.quality = "custom";     // ……而 custom 才不会把它按档位改回去
  c.capture.latency = "custom";
  c.web.password = "p@ss\"引号\n换行";
  Once o = cycle(c);
  check(o.parsed, "validate → to_json → from_json 这条路通", "报错：" + o.err);
  check(o.cfg.capture.fps == 37,
        "自检：改过的 fps 37 原样回来了（实际 " + num(o.cfg.capture.fps) + "）");
  // 口令里有引号和换行 —— 哪天换成手拼 JSON 或正则会话，这一条会当场变红。
  check(o.cfg.web.password == c.web.password,
        "自检：带引号与换行的口令原样往返（说明走的是真 JSON 编解码）");
  check(!o.text.empty() && count_key(o.text, "capture") == 1,
        "自检：文本非空且认得出 capture 键（下面所有下毒断言的前提）");
  check(o.text_again == o.text,
        "自检：已经 validate 过一次的配置，第二圈的落盘文本逐字节相同",
        "不幂等的话，下面所有的往返比较都会掺进'第二圈又改了点什么'");
}

// ---------------------------------------------------------------------------
// [1] 兼容边界：空 items ⇒ 文本里没有 channels 键
// ---------------------------------------------------------------------------
void test_absent_when_empty() {
  std::printf("\n[1] 兼容边界：一项都没有时不写 channels 键\n");
  AppConfig c;
  std::vector<std::string> notes;
  validate_config(c, notes);
  std::string const text = config_to_json(c, false);

  // 先证"读到了东西"：空串上，"没有 channels" 是恒真的（陷阱 91）。
  check(text.size() > 200,
        "落盘文本非空（" + num(static_cast<long long>(text.size())) + " 字节）");
  check(count_key(text, "rtsp") >= 1 && count_key(text, "capture") >= 1,
        "文本里认得出 rtsp 与 capture 两个键（量具没瞎）");
  check(count_key(text, "channels") == 0,
        "默认配置的落盘文本里**没有** channels 键（兼容边界）");
  // 只说"没有一条 note 提到 channels"，**不**说"没有 note"：出厂配置本来就会
  // 产生一条与通道无关的 —— `capture.threads` 出厂是 0（= 按核数自动选），
  // 而延时档位表里「均衡」写的是 3，于是每次 validate 都会报一句
  // "延时档位「均衡」把 capture.threads 从 0 改成 3"。这是既有行为，
  // 不是这一段要管的事；把断言写成 `notes.empty()` 就会为它变红，
  // 而红的位置指向 channels —— 一个完全错误的方向。
  check(!says(notes, "channels"), "而且没有一条 note 提到 channels",
        "冒出来的 note：" + join(notes));

  // 反证：塞一项进去，同一个量具必须能看见它。少了这一条，上面那句
  // "没有 channels" 也可能只是因为 count_key 认不出这个键。
  AppConfig c2 = c;
  c2.channels.items.push_back(disp(1));
  std::vector<std::string> n2;
  validate_config(c2, n2);
  check(count_key(config_to_json(c2, false), "channels") == 1,
        "放一项进去，channels 键就出现了（同一个量具看得见）");

  // 去掉一项 ≠ 清空：只剩一项时键还在。
  AppConfig c3 = c;
  c3.channels.items.push_back(disp(0));
  c3.channels.items.push_back(disp(0));   // 第二项会被去重丢掉
  std::vector<std::string> n3;
  validate_config(c3, n3);
  check(c3.channels.items.size() == 1 && count_key(config_to_json(c3, false), "channels") == 1,
        "去重剩一项时 channels 键照样在（去掉一项不等于清空）");

  AppConfig c4 = c;
  c4.channels.items.push_back(disp(0));
  c4.channels.items.push_back(disp(1));
  std::vector<std::string> n4;
  validate_config(c4, n4);
  c4.channels.items.clear();
  check(count_key(config_to_json(c4, false), "channels") == 0,
        "清空 items 之后文本里又没有了（这条键跟着 items 走）");
}

// ---------------------------------------------------------------------------
// [2] 往返一致 + 整数组替换
// ---------------------------------------------------------------------------
void test_roundtrip() {
  std::printf("\n[2] 两条通道：落盘 → 读回，逐字段一致\n");
  AppConfig c;
  c.channels.items.push_back(disp(1, "/cam_front"));
  c.channels.items.push_back(cam("", "/cam_usb"));
  std::vector<std::string> notes;
  validate_config(c, notes);

  Once o = cycle(c);
  check(o.parsed, "两条通道的配置读得回来", "报错：" + o.err);
  if (!o.parsed) return;
  check(o.cfg.channels.items.size() == 2,
        "读回来还是两项（实际 " +
            num(static_cast<long long>(o.cfg.channels.items.size())) + "）");
  if (o.cfg.channels.items.size() != 2) return;

  ChannelConfig const& a = o.cfg.channels.items[0];
  ChannelConfig const& b = o.cfg.channels.items[1];
  check(a.source == "display" && a.display_index == 1 && a.rtsp_path == "/cam_front",
        "第一项（显示器 1、/cam_front）逐字段对上了");
  check(b.source == "webcam" && b.rtsp_path == "/cam_usb",
        "第二项（摄像头、/cam_usb）逐字段对上了");
  check(a.enabled && b.enabled, "两项都默认开着");
  check(b.camera.empty(), "摄像头项的 camera 空着（= 第一台）也能往返");
  // 文本层面再钉一次：字段真的被写进去了，而不是"两边一样地少"。
  check(count_key(o.text, "display_index") >= 3 && count_key(o.text, "camera") >= 2,
        "文本里两项都带着 display_index 与 camera 两个键（不是被省略掉的空对象）");
  // 第二圈等于第一圈：读回来那份没有被 validate 再改一次。
  check(o.text_again == o.text, "往返幂等：读回来再落盘，文本逐字节相同");

  // 整数组替换：`{"channels":{}}`（没有 items 键）必须**不动**原值。
  AppConfig keep = c;
  std::string err;
  check(config_from_json("{\"channels\":{}}", keep, err) && keep.channels.items.size() == 2,
        "只发 {\"channels\":{}} 时原有两项岿然不动（没有 items 键 = 没打算改）",
        "报错：" + err);
  // 而 `items: []` 是**明说要清空** —— 两者必须分得开，否则"删掉所有通道"这个
  // 操作在网页上做不到（发了空数组、服务端当成"没写"）。
  check(config_from_json("{\"channels\":{\"items\":[]}}", keep, err) &&
            keep.channels.items.empty(),
        "发 items:[] 才是清空（与'没写这个键'分得开）");
}

// ---------------------------------------------------------------------------
// [3] 档位展开 —— 值必须变，话也必须说
// ---------------------------------------------------------------------------
void test_tier_expand() {
  std::printf("\n[3] 通道的画质档位：展开成具体值，并逐条说明\n");
  AppConfig c;
  ChannelConfig ch = disp(1, "/c");
  ch.quality = "smooth";     // 流畅 = fps 20 / 1024 / max_height 1080 / sub_enabled true
  ch.fps = 30;               // 故意写成别的值
  ch.bitrate_kbps = 9999;
  ch.max_height = 0;
  ch.sub_enabled = false;
  c.channels.items.push_back(ch);
  std::vector<std::string> notes;
  validate_config(c, notes);
  ChannelConfig const& got = c.channels.items[0];

  QualityTier const* t = find_quality_tier("smooth");
  check(t != nullptr, "档位表里有 smooth（下面几条的基准）");
  if (!t) return;
  check(got.fps == t->fps && got.bitrate_kbps == t->bitrate_kbps &&
            got.max_height == t->max_height && got.sub_enabled == t->sub_enabled,
        "四项都被展开成档位的值（fps " + num(got.fps) + " / " +
            num(got.bitrate_kbps) + " / " + num(got.max_height) + " / sub " +
            (got.sub_enabled ? "on" : "off") + "）");
  // **话说了没有。** 静默展开的后果是"选了流畅但界面显示帧率 30"这种没有任何
  // 解释的矛盾 —— 值变了和说了话是两件事，都得验（陷阱 85 的形状）。
  check(says(notes, "channels.items[0]") && says(notes, "画质档位"),
        "每一项被覆盖的字段都有一条 note，且点名是 channels.items[0]");
  check(says(notes, "30") && says(notes, "20"),
        "note 里写清了从多少改到多少（30 → 20）");
  // **逐字段点名**，不能只判"有一条提到 channels.items[0] 的 note"：
  // `sub_enabled` 那条是**内联**写的（不走 `snap()`），它一个人就能让上面
  // 那条宽判据变绿，而 fps / bitrate_kbps / max_height 三条静默消失。
  // 实测：`out\ac-mut` 的 m6 变异体（snap 只改值不说话）正是这样 ——
  // 有了这一条它才会红在正确的位置。
  {
    std::string missing;
    for (char const* f : {"fps", "bitrate_kbps", "max_height", "sub_enabled"}) {
      std::string const pat =
          std::string("channels.items[0] 的画质档位「") + t->label + "」把 " + f;
      if (!says(notes, pat)) missing += std::string(missing.empty() ? "" : " / ") + f;
    }
    check(missing.empty(),
          "四个字段**各自**都有一条点名到字段的 note（不是只靠 sub_enabled 凑数）",
          "没说到的是：" + missing);
  }
  std::printf("         notes：%s\n", join(notes).c_str());

  // **判别力**：custom 必须一个字段都不动。少了这一条，"什么都按 smooth 展开"
  // 也能让上面那四条通过。
  AppConfig c2;
  ChannelConfig ch2 = disp(1, "/c");
  ch2.quality = "custom";
  ch2.fps = 17;
  ch2.bitrate_kbps = 777;
  ch2.max_height = 0;
  c2.channels.items.push_back(ch2);
  std::vector<std::string> n2;
  validate_config(c2, n2);
  check(c2.channels.items[0].fps == 17 && c2.channels.items[0].bitrate_kbps == 777 &&
            c2.channels.items[0].max_height == 0,
        "custom 档位一个字段都不动（17 / 777 / 0 原样留着）");
  check(!says(n2, "画质档位"), "custom 也不该冒出'画质档位'的 note");

  // 打错字要说出来，而且**不能**静默当成 custom —— 那会让人以为档位在管着。
  AppConfig c3;
  ChannelConfig ch3 = disp(1, "/c");
  ch3.quality = "hd";
  c3.channels.items.push_back(ch3);
  std::vector<std::string> n3;
  validate_config(c3, n3);
  check(c3.channels.items[0].quality == "custom",
        "不认识的档位被改成 custom（实际 " + c3.channels.items[0].quality + "）");
  check(says(n3, "hd") && says(n3, "high / balanced"),
        "note 里回显了打错的那个词，并列出了合法值");

  // 夹取：1 / 120 是 fps 的界，子码流高度下界 120。
  AppConfig c4;
  ChannelConfig ch4 = disp(1, "/c");
  ch4.quality = "custom";
  ch4.fps = 999;
  ch4.sub_height = 5;
  c4.channels.items.push_back(ch4);
  std::vector<std::string> n4;
  validate_config(c4, n4);
  check(c4.channels.items[0].fps == 120 && c4.channels.items[0].sub_height == 120,
        "越界的 fps 999 / sub_height 5 被夹到 120 / 120");
  check(says(n4, "channels.items[0].fps") && says(n4, "channels.items[0].sub_height"),
        "夹取也逐条报，字段名带上了 items[0]. 前缀");
}

// ---------------------------------------------------------------------------
// [4] 默认值：名字与路径按**序号**补
// ---------------------------------------------------------------------------
void test_defaults() {
  std::printf("\n[4] 空名字/空路径按序号补默认值\n");
  AppConfig c;
  c.channels.items.push_back(disp(1));   // → 通道 2 / /ch2
  c.channels.items.push_back(disp(2));   // → 通道 3 / /ch3
  std::vector<std::string> notes;
  validate_config(c, notes);
  check(c.channels.items[0].name == "通道 2" && c.channels.items[0].rtsp_path == "/ch2" &&
            c.channels.items[0].rtsp_sub_path == "/ch2_sub",
        "第一项补成 通道 2 / /ch2 / /ch2_sub（实际 " + c.channels.items[0].name + " / " +
            c.channels.items[0].rtsp_path + " / " + c.channels.items[0].rtsp_sub_path + "）");
  check(c.channels.items[1].name == "通道 3" && c.channels.items[1].rtsp_path == "/ch3",
        "第二项补成 通道 3 / /ch3（序号从 2 起，通道 1 不是 items 的一项）");
  // 这一条同时钉住**通道的出厂默认值与档位表必须一致**：`ChannelConfig` 出厂的
  // 20/1024/1080/true 恰好是「流畅」档的值，所以一项都没改的通道不该冒出任何
  // note。档位表哪天被改了而这里没跟上，这条会当场变红，而不是等到用户每次
  // 启动都收到几条"通道 2 的画质档位「流畅」把 fps 从 20 改成 18"。
  //
  // 判据是"没有提到 channels 的 note"而不是"没有 note" —— 出厂配置另有一条
  // 与通道无关的（`capture.threads` 0 → 3，见 [1] 段的解释）。
  check(!says(notes, "channels"), "补默认值**不**报 note（那是默认，不是修正）",
        "冒出来的 note：" + join(notes));

  // 用户写了名字和路径就一个都不许动。
  AppConfig c2;
  ChannelConfig a = disp(1, "/front");
  a.name = "大门口";
  a.rtsp_sub_path = "/front_sub";
  c2.channels.items.push_back(a);
  std::vector<std::string> n2;
  validate_config(c2, n2);
  check(c2.channels.items[0].name == "大门口" && c2.channels.items[0].rtsp_path == "/front" &&
            c2.channels.items[0].rtsp_sub_path == "/front_sub",
        "写了的名字与两条路径原样不动");
  check(!says(n2, "channels"), "而且没有一条提到 channels 的 note",
        "冒出来的 note：" + join(n2));

  // 缺前导 `/`。
  AppConfig c3;
  c3.channels.items.push_back(disp(1, "front"));
  std::vector<std::string> n3;
  validate_config(c3, n3);
  check(c3.channels.items[0].rtsp_path == "/front",
        "rtsp_path 缺前导斜杠被补上（" + c3.channels.items[0].rtsp_path + "）");
  check(says(n3, "必须以 / 开头"), "并且说了出来");

  // 不认识的 source。
  AppConfig c4;
  ChannelConfig bad = disp(1, "/x");
  bad.source = "camera";
  c4.channels.items.push_back(bad);
  std::vector<std::string> n4;
  validate_config(c4, n4);
  check(c4.channels.items[0].source == "display",
        "不认识的 source 被改成 display（实际 " + c4.channels.items[0].source + "）");
  check(says(n4, "camera") && says(n4, "display / webcam"),
        "note 里回显了打错的那个词，并列出了合法值");
}

// ---------------------------------------------------------------------------
// [5] 去重
// ---------------------------------------------------------------------------
void test_dedup() {
  std::printf("\n[5] 去重：显示器与路由\n");
  // 重复的 display_index：后一项丢。
  AppConfig c;
  c.channels.items.push_back(disp(0, "/a"));
  c.channels.items.push_back(disp(0, "/b"));   // ← 丢
  c.channels.items.push_back(disp(1, "/c"));
  std::vector<std::string> notes;
  validate_config(c, notes);
  check(c.channels.items.size() == 2,
        "三项里丢掉重复的那一项，剩两项（实际 " +
            num(static_cast<long long>(c.channels.items.size())) + "）");
  if (c.channels.items.size() == 2) {
    check(c.channels.items[0].rtsp_path == "/a" && c.channels.items[1].rtsp_path == "/c",
          "丢掉的是**后者**（/b 没了，/a 和 /c 都在）");
  }
  check(says(notes, "channels.items[1]") && says(notes, "display_index"),
        "note 点明了是 items[1]、理由是 display_index 重复");
  std::printf("         notes：%s\n", join(notes).c_str());

  // **不重编号**：丢掉 items[1] 之后，items[2] 的默认路径必须还是 /ch4。
  // 重编号会悄悄改掉后面每一项的默认路径，而 NVR 上那个书签是按路径存的。
  AppConfig c2;
  c2.channels.items.push_back(disp(0));
  c2.channels.items.push_back(disp(0));   // ← 丢
  c2.channels.items.push_back(disp(1));   // 默认路径应当是 /ch4
  std::vector<std::string> n2;
  validate_config(c2, n2);
  check(c2.channels.items.size() == 2 && c2.channels.items[1].rtsp_path == "/ch4" &&
            c2.channels.items[1].name == "通道 4",
        "丢项之后**不重编号**：幸存的那一项仍是 /ch4 / 通道 4（实际 " +
            (c2.channels.items.size() > 1 ? c2.channels.items[1].rtsp_path + " / " +
                                                c2.channels.items[1].name
                                          : std::string("(没了)")) + "）");

  // 重复的路由：后一项丢。
  AppConfig c3;
  c3.channels.items.push_back(disp(0, "/same"));
  c3.channels.items.push_back(disp(1, "/same"));   // ← 丢
  std::vector<std::string> n3;
  validate_config(c3, n3);
  check(c3.channels.items.size() == 1, "路径重复的那一项被丢掉");
  check(says(n3, "rtsp_path /same") && says(n3, "重复"),
        "note 点明了是哪条路径重复");
  // 丢掉了要给出路，否则用户只知道"少了一个通道"。
  check(says(n3, "换一个 rtsp_path"), "note 里给了怎么保住那一项");

  // **补斜杠之后**才去重：`live` 与 `/live` 在 Live555 眼里是同一条路由。
  //
  // ⚠️ 这条顺序**只有一个方向验得出来**，值得写清楚是什么方向 ——
  // 因为 `used` 里的值永远已经是归一化过的（占位发生在 `lead()` 之后），
  // 所以"两项之间撞车"（`same` 对 `/same`）**分辨不出**顺序：把 `lead` 挪到
  // 去重后面，第一项占的仍是归一化之后的值，第二项照样被认出来。
  // 能分辨的只有"**这一项自己写的原始值** 对上 `used` 里已有的归一化值"，
  // 而去重之前 `used` 里只有通道 1 那两条 —— 所以用例必须是
  // **"没写斜杠的路径撞上通道 1"**。
  //
  // 这不是推理出来的：`out\ac-mut` 的 m5 变异体（`lead` 挪到去重之后）在
  // 一个"两项都写斜杠"的用例上照样绿，而那次绿**绿在了另一个原因上**
  // （空 sub_path 被当成占位互相撞）。一条绿在错地方的断言比红更难发现。
  AppConfig c4;
  c4.channels.items.push_back(disp(1, "live"));   // 通道 1 占着 /live
  std::vector<std::string> n4;
  validate_config(c4, n4);
  check(c4.channels.items.empty(),
        "没写斜杠的 `live` 也认得出撞了通道 1 的 /live（实际剩 " +
            num(static_cast<long long>(c4.channels.items.size())) + " 项）");
  check(says(n4, "/live"), "note 里报出了撞的那个路径");

  // 子码流那条路同一个道理，得单独来一遍：`lead("rtsp_sub_path", …)` 是另一次
  // 调用，只验主路径的话，把它单独挪到去重之后照样能全绿。
  AppConfig c4b;
  ChannelConfig b4 = disp(1, "/a");
  b4.rtsp_sub_path = "live_sub";                  // 通道 1 占着 /live_sub
  c4b.channels.items.push_back(b4);
  std::vector<std::string> n4b;
  validate_config(c4b, n4b);
  check(c4b.channels.items.empty(),
        "子码流路径没写斜杠时同样在去重前补好（实际剩 " +
            num(static_cast<long long>(c4b.channels.items.size())) + " 项）");

  // 与**通道 1** 撞：通道 1 的 /live 与 /live_sub 也占着位置。
  AppConfig c5;
  c5.channels.items.push_back(disp(1, "/live"));       // 撞通道 1 的主码流
  c5.channels.items.push_back(disp(2, "/live_sub"));   // 撞通道 1 的子码流
  c5.channels.items.push_back(disp(3, "/ok"));
  std::vector<std::string> n5;
  validate_config(c5, n5);
  check(c5.channels.items.size() == 1 && c5.channels.items[0].rtsp_path == "/ok",
        "撞上通道 1 的 /live 与 /live_sub 一样被丢（实际剩 " +
            num(static_cast<long long>(c5.channels.items.size())) + " 项）");
  check(says(n5, "/live"), "note 里报出了撞的那个路径");

  // **幽灵占位**。一项的 `rtsp_path` 空着没人用、但 `rtsp_sub_path` 撞了 ——
  // 它整项要丢掉，而它那条 `rtsp_path` **不能**留在已占用的集合里，否则后面
  // 一个用同样路径的无辜项会被误判成重复。
  // （这正是"两条路径都查完再占位"那一步防的事；先占再查的写法会让这条红。）
  AppConfig c6;
  c6.channels.items.push_back(disp(1, "/p1"));
  c6.channels.items.back().rtsp_sub_path = "/s1";
  ChannelConfig g = disp(2, "/p2");
  g.rtsp_sub_path = "/s1";                 // ← 撞上面那条，整项丢
  c6.channels.items.push_back(g);
  c6.channels.items.push_back(disp(3, "/p2"));   // ← 必须活着
  std::vector<std::string> n6;
  validate_config(c6, n6);
  check(c6.channels.items.size() == 2 && c6.channels.items[1].rtsp_path == "/p2",
        "被丢掉的那一项没留下幽灵占位：后面用 /p2 的项活着（实际剩 " +
            num(static_cast<long long>(c6.channels.items.size())) + " 项）");
  std::printf("         notes：%s\n", join(n6).c_str());

  // **摄像头项不参与 display_index 去重。** 同一台摄像头建两路不同画质是合法的，
  // 而两项的 display_index 默认都是 0 —— 拿它去重会把这两项误杀成一项。
  AppConfig c7;
  c7.channels.items.push_back(cam("", "/cam_hi"));
  c7.channels.items.push_back(cam("", "/cam_lo"));
  std::vector<std::string> n7;
  validate_config(c7, n7);
  check(c7.channels.items.size() == 2,
        "两项摄像头（都是第一台、display_index 都是 0）**都活着**（实际 " +
            num(static_cast<long long>(c7.channels.items.size())) + " 项）");
  check(!says(n7, "display_index"), "也没有冒出 display_index 重复的 note");

  // 但摄像头的**路径**照样要去重。
  AppConfig c8;
  c8.channels.items.push_back(cam("", "/same2"));
  c8.channels.items.push_back(cam("", "/same2"));
  std::vector<std::string> n8;
  validate_config(c8, n8);
  check(c8.channels.items.size() == 1, "两项摄像头路径撞了，后一项照样丢");
}

// ---------------------------------------------------------------------------
// [6] 截断
// ---------------------------------------------------------------------------
void test_truncate() {
  std::printf("\n[6] 超过 %zu 项被截断\n", ChannelsConfig::kMaxChannels);
  AppConfig c;
  for (unsigned i = 0; i < 12; ++i) c.channels.items.push_back(disp(i));
  std::vector<std::string> notes;
  validate_config(c, notes);
  check(c.channels.items.size() == ChannelsConfig::kMaxChannels,
        "12 项截成 " + num(static_cast<long long>(c.channels.items.size())) + " 项");
  check(says(notes, "超过上限"), "有一条 note 说了为什么少了几项");
  check(std::count_if(notes.begin(), notes.end(), [](std::string const& n) {
          return n.find("channels.items[") != std::string::npos;
        }) == 0,
        "被砍掉的那几项**一条 note 都没有**（先截断、再逐项处理）",
        "冒出来的：" + join(notes));
  // 截断之后剩下的那 8 项自己得是干净的。
  bool all_ready = true;
  for (std::size_t i = 0; i < c.channels.items.size(); ++i) {
    if (c.channels.items[i].name.empty() || c.channels.items[i].rtsp_path.empty()) {
      all_ready = false;
    }
  }
  check(all_ready, "幸存的那几项名字与路径都补齐了");

  // 边界：正好 8 项时**不该**有截断的 note（差一位数的错最难发现）。
  AppConfig c2;
  for (unsigned i = 0; i < ChannelsConfig::kMaxChannels; ++i) {
    c2.channels.items.push_back(disp(i));
  }
  std::vector<std::string> n2;
  validate_config(c2, n2);
  check(c2.channels.items.size() == ChannelsConfig::kMaxChannels && !says(n2, "超过上限"),
        "正好 " + num(static_cast<long long>(ChannelsConfig::kMaxChannels)) +
            " 项时不动也不报");
}

// ---------------------------------------------------------------------------
// [7] JSON 形状不对要**报错**，不能猜
// ---------------------------------------------------------------------------
void test_bad_shape() {
  std::printf("\n[7] channels 的形状错误必须报错\n");
  AppConfig c;
  std::string err;
  check(!config_from_json("{\"channels\":{\"items\":{}}}", c, err),
        "items 是对象而不是数组 → 拒绝");
  check(err.find("channels.items") != std::string::npos,
        "报错里点名字段（" + err + "）");
  err.clear();
  check(!config_from_json("{\"channels\":{\"items\":[1,2]}}", c, err),
        "items 里的项不是对象 → 拒绝");
  check(err.find("channels.items") != std::string::npos,
        "报错里点名字段（" + err + "）");
  err.clear();
  check(!config_from_json("{\"channels\":{\"items\":[{\"fps\":\"快\"}]}}", c, err),
        "fps 的类型不对（字符串）→ 拒绝而不是当成默认值");
  std::printf("         类型错误的原文：%s\n", err.c_str());
  err.clear();
  // `channels` 整个不是对象时**宽容跳过**（与其余各段同一条规则：键在但形状
  // 不对的段落由 `is_object()` 挡住）。这里要钉的是"跳过时**没动**已有内容" ——
  // 少了后半句，"跳过"和"清空"分不开。
  AppConfig c9;
  c9.channels.items.push_back(disp(1, "/keep"));
  bool const ok9 = config_from_json("{\"channels\":\"nope\"}", c9, err);
  check(ok9 && c9.channels.items.size() == 1,
        "channels 不是对象时安静跳过，原有的一项没被清掉",
        "ok=" + std::string(ok9 ? "true" : "false") + " err=" + err);
  err.clear();
  // **向前兼容**：不认识的键要安静跳过，将来加了字段老代码也不该炸。
  check(config_from_json("{\"channels\":{\"items\":[{\"future_field\":42}]}}", c, err),
        "不认识的键安静跳过（向前兼容）", "报错：" + err);
}

// ---------------------------------------------------------------------------
// [8] diff_config：整块报一次
// ---------------------------------------------------------------------------
void test_diff() {
  std::printf("\n[8] diff_config 对通道的一处改动报什么\n");
  AppConfig a;
  a.channels.items.push_back(disp(1, "/x"));
  a.channels.items.push_back(disp(2, "/y"));
  std::vector<std::string> na;
  validate_config(a, na);

  AppConfig b = a;
  // ⚠️ 必须先翻成 custom 再改 fps —— 否则 validate 会把 11 按「流畅」档夹回 20，
  // 于是"改了一项"这件事根本没发生，而下面那条断言会以"报零条"的形式红掉，
  // 指向"diff 坏了"这个完全错误的方向。
  b.channels.items[1].quality = "custom";
  b.channels.items[1].fps = 11;
  std::vector<std::string> nb;
  validate_config(b, nb);
  check(b.channels.items[1].fps == 11, "自检：改进去的 fps 11 没被档位夹回去");

  std::vector<std::string> d;
  diff_config(a, b, d);
  check(d.size() == 1 && d[0] == "channels.items",
        "只改了一项的一个字段 → 恰好报一条 channels.items（实际 " +
            num(static_cast<long long>(d.size())) + " 条：" +
            (d.empty() ? std::string("(空)") : d[0]) + "）");

  std::vector<std::string> d2;
  diff_config(a, a, d2);
  check(d2.empty(), "自己和自己比没有差异");

  // 反证：diff 对 channels 是**看着的**（不是整段被忽略）。少了这一条，
  // 上面那句"恰好一条"无法区分"改一项报一条"和"改什么都报一条"。
  AppConfig e = a;
  e.channels.items.push_back(disp(3, "/z"));
  std::vector<std::string> ne;
  validate_config(e, ne);
  std::vector<std::string> d3;
  diff_config(a, e, d3);
  check(std::find(d3.begin(), d3.end(), "channels.items") != d3.end(),
        "加了一整项 → channels.items 出现在差异里");

  // 兼容：一边有通道、一边没有，也必须报出来 —— 而且报的是 **`channels`**
  // 而不是 `channels.items`。
  //
  // 这条路径名字是 `walk_diff` 的形状决定的：键只在一侧存在时，它把**那个键**
  // 报出来，连下钻都不下钻（数组是叶子、对象才有下一层）。所以"整个 channels
  // 子树出现/消失"报的是 `channels`，"子树里的 items 变了"报的才是
  // `channels.items`。前端要认的字段名**两个都得有**（Phase AF 那张
  // `FIELD_LABELS` 表），少一个的后果是提示里出现一行没有名字的英文路径。
  AppConfig f;
  std::vector<std::string> nf;
  validate_config(f, nf);
  std::vector<std::string> d4;
  diff_config(f, a, d4);
  check(d4.size() == 1 && d4[0] == "channels",
        "从'没有 channels 键'变成'有' → 报 channels（整棵子树），实际 " +
            join(d4));
  // 反向也要：少了它，"从有到无"（删掉所有通道）会静默地不进 pending。
  std::vector<std::string> d5;
  diff_config(a, f, d5);
  check(d5.size() == 1 && d5[0] == "channels",
        "从'有'变成'没有' → 同样报 channels，实际 " + join(d5));

  // **粒度的真话**：`walk_diff` 只在"两边都是对象"时下钻，数组是整块比的。
  // 所以重排两项报出来的也是同一条路径，而不是一串按下标的假差异。
  AppConfig g = a;
  std::swap(g.channels.items[0], g.channels.items[1]);
  std::vector<std::string> ng;
  validate_config(g, ng);
  std::vector<std::string> d6;
  diff_config(a, g, d6);
  check(d6.size() == 1 && d6[0] == "channels.items",
        "只换了个顺序 → 还是那一条（数组整块比，不按下标逐项比；实际 " +
            num(static_cast<long long>(d6.size())) + " 条）");
}

}  // namespace

int main() {
  std::printf("=== 多通道配置骨架（Phase AC）===\n");
  test_instrument();
  test_absent_when_empty();
  test_roundtrip();
  test_tier_expand();
  test_defaults();
  test_dedup();
  test_truncate();
  test_bad_shape();
  test_diff();
  std::printf("----\n通过 %d 项，失败 %d 项。\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
