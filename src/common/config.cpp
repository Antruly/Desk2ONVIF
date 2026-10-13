#include "common/config.h"

#include "common/log.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace d2o {

char const* const kSecretSentinel = "__unchanged__";

namespace {

using nlohmann::json;

/// 取一个字段。**键缺失就保留默认值** —— 这是配置能向前兼容的全部原因：
/// 老配置文件里没有新加的键，读的时候必须安静地跳过，而不是当错误。
/// 类型不对则让 `get<T>()` 抛出去，由 `config_from_json` 转成带字段名的人话。
template <typename T>
void take(json const& j, char const* key, T& out) {
  auto const it = j.find(key);
  if (it == j.end() || it->is_null()) return;
  out = it->get<T>();
}

/// 反向：把值写进 JSON 对象。
template <typename T>
void put(json& j, char const* key, T const& v) {
  j[key] = v;
}

std::string widen_field_name(std::string const& what) {
  return what.empty() ? std::string("(未知字段)") : what;
}

/// 递归找差异。`path` 是已经走过的点分路径，顶层为空。
///
/// 只处理"两边都是对象"和"走到叶子"两种情形：**类型不同**（比如一边是字符串
/// 一边是数字）也算不同 —— 那确实是一处差异，而且是最该被报出来的一种。
void walk_diff(json const& a, json const& b, std::string const& path,
               std::vector<std::string>& out) {
  if (a.is_object() && b.is_object()) {
    for (auto const& kv : a.items()) {
      std::string const sub = path.empty() ? kv.key() : path + "." + kv.key();
      auto const it = b.find(kv.key());
      if (it == b.end()) {
        out.push_back(sub);  // 只有一边有这一项：也是差异
        continue;
      }
      walk_diff(kv.value(), *it, sub, out);
    }
    for (auto const& kv : b.items()) {
      // 反向补一次：只在 b 里存在的键在上一轮循环里看不到。
      if (!a.contains(kv.key())) out.push_back(path.empty() ? kv.key()
                                                            : path + "." + kv.key());
    }
    return;
  }
  if (a != b) out.push_back(path);
}

}  // namespace

std::vector<QualityTier> const& quality_tiers() {
  // **从最费性能到最省**，这个顺序本身就是语义：界面的下拉、
  // `suggest_lower_quality()` 的"下一档"都靠它。
  static std::vector<QualityTier> const kTiers = {
      {"high", "高性能", 30, 8192, 0, true},
      {"balanced", "均衡", 25, 2048, 0, true},
      {"smooth", "流畅", 20, 1024, 1080, true},
      {"verylow", "极低", 12, 512, 720, false},
  };
  return kTiers;
}

QualityTier const* find_quality_tier(std::string const& key) {
  for (auto const& t : quality_tiers()) {
    if (key == t.key) return &t;
  }
  return nullptr;
}

std::vector<LatencyTier> const& latency_tiers() {
  // **从延迟最低到最高**。「优先画质」的 threads=6 与自动值封顶的 6 相同，
  // 也就是说它是"今天的流水线深度、把关键帧放长"这一档 —— 三档里只有它
  // 不靠牺牲吞吐换延迟。
  //
  // 档位名刻意**不叫「极致0延时」**：这个名字会被量出来证伪（开音频时地板
  // 就有 ~320 ms：环回环 300 ms + AAC priming 21 ms，网页预览的真值更是
  // 秒级）。界面上写"能到多少"而不写"地板在哪"，用户照样会拿一个够不到的
  // 数来对账。
  static std::vector<LatencyTier> const kTiers = {
      {"realtime", "极致低延迟", 1, 1},
      {"balanced", "均衡", 3, 2},
      {"quality", "优先画质", 6, 4},
  };
  return kTiers;
}

LatencyTier const* find_latency_tier(std::string const& key) {
  for (auto const& t : latency_tiers()) {
    if (key == t.key) return &t;
  }
  return nullptr;
}

std::string suggest_lower_quality(AppConfig const& c) {
  auto const& tiers = quality_tiers();
  // 默认给 tiers.size()（一个非法的下标）表示"落点未知"，于是下面那两处
  // 判断都会走成"没有更低的了"。宁可不建议，也不要建议一个更高的档。
  std::size_t idx = tiers.size();
  if (QualityTier const* t = find_quality_tier(c.capture.quality)) {
    idx = static_cast<std::size_t>(t - tiers.data());
  } else {
    // `custom`（或任何没被认得的值）：按帧率反推它落在哪一档。档位表是按
    // 帧率降序的，所以第一个满足 `fps <= 该档帧率` 的就是它所在的档。
    for (std::size_t i = 0; i < tiers.size(); ++i) {
      if (c.capture.fps <= tiers[i].fps) {
        idx = i;
        break;
      }
    }
  }
  if (idx + 1 >= tiers.size()) return std::string();  // 已经在最低档
  return tiers[idx + 1].key;
}

std::string default_config_path() {
  char const* appdata = std::getenv("APPDATA");
  if (appdata != nullptr && *appdata != '\0') {
    return std::string(appdata) + "\\Desk2ONVIF\\config.json";
  }
  // %APPDATA% 拿不到（服务账户、精简环境）时退到 exe 同级。这一支几乎不会
  // 走到，但真走到时"配置存不下来"比"配置存到当前目录"更难排查。
  return "desk2onvif.json";
}

bool load_config(AppConfig& out, std::string const& path, std::string& err,
                 bool* existed) {
  namespace fs = std::filesystem;
  if (existed != nullptr) *existed = false;

  std::error_code ec;
  if (!fs::exists(path, ec)) {
    // 没有配置文件是完全正常的状态（第一次运行）。不是错误。
    return true;
  }
  if (existed != nullptr) *existed = true;

  std::ifstream in(path, std::ios::binary);
  if (!in) {
    err = "打不开配置文件：" + path;
    return false;
  }
  std::ostringstream buf;
  buf << in.rdbuf();
  return config_from_json(buf.str(), out, err);
}

bool save_config(AppConfig const& c, std::string const& path, std::string& err) {
  namespace fs = std::filesystem;
  try {
    fs::path const p(path);
    if (p.has_parent_path()) fs::create_directories(p.parent_path());

    // 先写临时文件再改名。直接往目标文件写的话，写到一半崩了/掉电了，
    // 留下的是半个 JSON —— 下次启动连默认值都读不回来。
    fs::path const tmp = p.string() + ".tmp";
    {
      std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
      if (!out) {
        err = "写不了临时文件：" + tmp.string();
        return false;
      }
      // 存盘的**不抹**密码，否则存一次就把口令弄丢了。
      out << config_to_json(c, /*redact_secrets=*/false) << "\n";
      if (!out) {
        err = "写配置文件失败（磁盘满？）：" + tmp.string();
        return false;
      }
    }
    // 目标已存在时 Windows 的 rename 会失败，所以先删目标。
    // 这一步不是原子的（删了之后改名之前掉电就没配置了），但比"写一半"
    // 好：最坏是回到默认值，而不是拿到一个语法都不合法的文件。
    std::error_code ec;
    fs::remove(p, ec);
    fs::rename(tmp, p, ec);
    if (ec) {
      err = "改名失败：" + ec.message() + "（临时文件留在 " + tmp.string() + "）";
      return false;
    }
    return true;
  } catch (std::exception const& e) {
    err = std::string("保存配置时出错：") + e.what();
    return false;
  }
}

bool validate_config(AppConfig& c, std::vector<std::string>& notes) {
  bool changed = false;
  auto clamp = [&](char const* name, int& v, int lo, int hi) {
    if (v < lo || v > hi) {
      notes.push_back(fmt("%s = %d 超出 [%d, %d]，已夹到 %d", name, v, lo, hi,
                          std::clamp(v, lo, hi)));
      v = std::clamp(v, lo, hi);
      changed = true;
    }
  };

  // **画质档位先展开，再做区间夹取。** 展开出来的值本来就在合法区间内，
  // 但 `custom` 的值得靠后面的夹取兜底，所以两件事都要做、且这个顺序最省事。
  //
  // 被覆盖的项**逐条记进 notes**：不记的话，"选了流畅但界面显示帧率 30"
  // 这种（用户手改过具体字段）会变成一个没有任何解释的矛盾。
  if (QualityTier const* t = find_quality_tier(c.capture.quality)) {
    auto const snap = [&](char const* name, int& v, int want) {
      if (v == want) return;
      notes.push_back(
          fmt("画质档位「%s」把 %s 从 %d 改成 %d", t->label, name, v, want));
      v = want;
      changed = true;
    };
    snap("capture.fps", c.capture.fps, t->fps);
    snap("capture.bitrate_kbps", c.capture.bitrate_kbps, t->bitrate_kbps);
    snap("capture.max_height", c.capture.max_height, t->max_height);
    if (c.rtsp.sub_enabled != t->sub_enabled) {
      notes.push_back(fmt("画质档位「%s」把 rtsp.sub_enabled 改成 %s", t->label,
                          t->sub_enabled ? "true" : "false"));
      c.rtsp.sub_enabled = t->sub_enabled;
      changed = true;
    }
  } else if (c.capture.quality != "custom") {
    // 打错字要**说出来**。静默当成 custom 的话，用户会以为档位在管着，
    // 实际一个字段都没被管 —— 而这正是"改了没反应"最难查的那一类。
    notes.push_back("capture.quality 不认识「" + c.capture.quality +
                    "」，已改成 custom（可选：high / balanced / smooth / "
                    "verylow / custom）");
    c.capture.quality = "custom";
    changed = true;
  }

  // **延时档位**：与上面那段同构，展开的是另一组字段（threads / gop_seconds），
  // 与画质档位展开的四项没有交集，所以两段互不干扰、先后顺序也无所谓。
  //
  // 放在夹取之前，理由和画质那段一样：档位表里的值本来就在合法区间内，
  // 而 `custom` 的值得靠后面的 clamp 兜底。
  if (LatencyTier const* t = find_latency_tier(c.capture.latency)) {
    auto const snap = [&](char const* name, int& v, int want) {
      if (v == want) return;
      notes.push_back(
          fmt("延时档位「%s」把 %s 从 %d 改成 %d", t->label, name, v, want));
      v = want;
      changed = true;
    };
    snap("capture.threads", c.capture.threads, t->threads);
    snap("capture.gop_seconds", c.capture.gop_seconds, t->gop_seconds);
  } else if (c.capture.latency != "custom") {
    // 与画质档位同一条纪律：打错字要**说出来**再改成 custom。静默当成
    // custom 的话，用户会以为档位在管着，实际一个字段都没被管 ——
    // 而这正是"改了没反应"最难查的那一类。
    notes.push_back("capture.latency 不认识「" + c.capture.latency +
                    "」，已改成 custom（可选：realtime / balanced / quality / "
                    "custom）");
    c.capture.latency = "custom";
    changed = true;
  }

  clamp("capture.fps", c.capture.fps, 1, 120);
  clamp("capture.bitrate_kbps", c.capture.bitrate_kbps, 100, 100000);
  clamp("capture.gop_seconds", c.capture.gop_seconds, 1, 60);
  clamp("capture.threads", c.capture.threads, 0, 128);
  // 0 是**有意义**的值（= 不缩放），不是"没设置"，所以下界是 0 而不是 240。
  clamp("capture.max_height", c.capture.max_height, 0, 4320);
  clamp("adaptive.min_fps", c.adaptive.min_fps, 1, 60);
  clamp("osd.plate_alpha", c.osd.plate_alpha, 0, 255);
  clamp("rtsp.port", c.rtsp.port, 1, 65535);
  // 8 kbps 是 AAC-LC 在 48 kHz 立体声下的实际下限，512 是"再高也没意义"。
  // 夹取而不是报错，理由和别处一样：**静默修正**才是坏的，夹了就要说出来。
  clamp("audio.bitrate_kbps", c.audio.bitrate_kbps, 8, 512);
  // 增益是 float，用不上上面那个 int 的 clamp。0 是**有意义**的值（= 把这一路
  // 静音掉），所以下界是 0 而不是 1；上界 8 是因为再往上只是更狠的削顶，
  // 而削顶听上去像爆音 —— 那不是用户调这个数想要的东西。
  auto clamp_gain = [&](char const* name, float& v) {
    if (v < 0.0f || v > 8.0f) {
      float const clamped = std::clamp(v, 0.0f, 8.0f);
      notes.push_back(
          fmt("%s = %.3f 超出 [0, 8]，已夹到 %.3f", name, v, clamped));
      v = clamped;
      changed = true;
    }
  };
  clamp_gain("audio.mic_gain", c.audio.mic_gain);
  clamp_gain("audio.system_gain", c.audio.system_gain);
  clamp("rtsp.sub_height", c.rtsp.sub_height, 120, 2160);
  clamp("rtsp.sub_bitrate_kbps", c.rtsp.sub_bitrate_kbps, 32, 100000);
  clamp("web.port", c.web.port, 1, 65535);

  if (c.adaptive.mode != "off" && c.adaptive.mode != "throttle" &&
      c.adaptive.mode != "auto") {
    notes.push_back("adaptive.mode 不认识「" + c.adaptive.mode +
                    "」，已改成 throttle（可选：off / throttle / auto）");
    c.adaptive.mode = "throttle";
    changed = true;
  }

  // 时间。偏移的上界取 ±365 天：它同时挡住"手滑多打几位数"和"纪元换算写错了"
  // 那类差 1900 年/差一天的错 —— 那两种错在这个区间外，会被当场夹掉并说出来，
  // 而不是安静地让水印显示一个荒诞的日期。
  clamp("time.offset_s", c.time.offset_s, -365 * 86400, 365 * 86400);
  clamp("time.ntp_interval_s", c.time.ntp_interval_s, 0, 86400);
  if (c.time.mode != "manual" && c.time.mode != "ntp") {
    notes.push_back("time.mode 不认识「" + c.time.mode +
                    "」，已改成 manual（可选：manual / ntp）");
    c.time.mode = "manual";
    changed = true;
  }
  // 空服务器名会让 NTP 那条路每次都走到"连地址都拿不到"。与其让它每次都失败
  // 一次再报，不如在这里给回默认值并说明 —— 这两条路对用户是同一件事，
  // 但对"为什么对不上时"的排查是两件事。
  if (c.time.mode == "ntp" && c.time.ntp_server.empty()) {
    notes.push_back("time.ntp_server 是空的，已改回默认的 ntp.aliyun.com");
    c.time.ntp_server = TimeConfig{}.ntp_server;
    changed = true;
  }

  // 审计日志的留存天数。**0 是有意义的值**（永不剪枝），所以下界是 0 而不是 1；
  // 上界取 3650（十年）只是挡住"手滑多打几位"，它不代表任何建议 —— 半年之后
  // 的文件早就不该在系统盘上了。上界不是硬性的技术限制，只是一条 sanity 线。
  clamp("log.audit_keep_days", c.log.audit_keep_days, 0, 3650);

  // OSD 的各项。字符串枚举也要**校验**，理由和 quality 一样：写错了要当场
  // 说，否则水印会安静地按默认位置/默认类型画出来，而配置里写的是别的。
  if (c.osd.items.size() > OsdConfig::kMaxItems) {
    notes.push_back(fmt("osd.items 有 %zu 项，超过上限 %zu 项，多出来的已丢弃",
                        c.osd.items.size(), OsdConfig::kMaxItems));
    c.osd.items.resize(OsdConfig::kMaxItems);
    changed = true;
  }
  for (std::size_t i = 0; i < c.osd.items.size(); ++i) {
    OsdItemConfig& it = c.osd.items[i];
    std::string const pre = fmt("osd.items[%zu]", i);
    clamp((pre + ".x_pct").c_str(), it.x_pct, 0, 100);
    clamp((pre + ".y_pct").c_str(), it.y_pct, 0, 100);
    clamp((pre + ".font_size").c_str(), it.font_size, 0, 512);
    if (it.type != "time" && it.type != "name" && it.type != "text") {
      notes.push_back(pre + ".type 不认识「" + it.type +
                      "」，已改成 text（可选：time / name / text）");
      it.type = "text";
      changed = true;
    }
    if (it.position != "upper_left" && it.position != "upper_right" &&
        it.position != "lower_left" && it.position != "lower_right" &&
        it.position != "custom") {
      notes.push_back(pre + ".position 不认识「" + it.position +
                      "」，已改成 upper_left（可选：upper_left / upper_right / "
                      "lower_left / lower_right / custom）");
      it.position = "upper_left";
      changed = true;
    }
  }

  if (c.rtsp.path.empty() || c.rtsp.path[0] != '/') {
    notes.push_back("rtsp.path 必须以 / 开头，已改成 /" + c.rtsp.path);
    c.rtsp.path = "/" + c.rtsp.path;
    changed = true;
  }
  if (c.rtsp.sub_path.empty() || c.rtsp.sub_path[0] != '/') {
    notes.push_back("rtsp.sub_path 必须以 / 开头，已改成 /" + c.rtsp.sub_path);
    c.rtsp.sub_path = "/" + c.rtsp.sub_path;
    changed = true;
  }
  if (c.rtsp.sub_enabled && c.rtsp.sub_path == c.rtsp.path) {
    // 两条 ServerMediaSession 挂同一个路径，是"两个同名的东西只能活一个"的
    // 局面，而无论哪一个赢，现象都是一条能正常播的流 —— 分不出它是主还是子。
    // 与其去赌 Live555 在重名时的取舍，不如在这里就把它拆开。
    std::string alt = c.rtsp.path + "_sub";
    notes.push_back("rtsp.sub_path 与 rtsp.path 相同（" + c.rtsp.path +
                    "），两路会撞在一起，sub_path 已改成 " + alt);
    c.rtsp.sub_path = std::move(alt);
    changed = true;
  }
  // ---------------------------------------------------------------------------
  // 附加通道（通道 2 起）
  // ---------------------------------------------------------------------------
  //
  // 四个动作，**顺序不能换**：
  //
  //   1. 截断    —— 先砍到 8 项。被砍掉的那几项不该再产生任何 note，否则用户会
  //                  收到一串"第 11 项如何如何"，而第 11 项根本不会被建出来。
  //   2. 补默认  —— 空名字/空路径要按**序号**展开成「通道 N」/`/chN`，否则步骤 3
  //                  拿不到能比较的值（"这项没写路径"和"另一项写了 /ch3"撞了，
  //                  按原样比是比不出来的）。
  //   3. 去重    —— 撞了路由或撞了显示器的项**整项丢弃**。
  //   4. 展开+夹取 —— 档位覆盖 fps/bitrate/max_height/sub_*，先展开再夹，和顶层
  //                  `capture.quality` 那一段同一个道理。
  //
  // **去重是按补完之后的值判的**，这是步骤 2 必须在 3 前面的唯一原因：`live`
  // 与 `/live` 在 Live555 眼里是同一条路由，按用户写的原样去重会漏掉这一对。
  //
  // 注意这条顺序**只有一个方向可观测**：`used` 里的值永远已经是归一化过的
  // （占位发生在 `lead()` 之后），所以"两项之间撞车"分辨不出顺序，能分辨的
  // 只有"这一项自己写的原始值 对上 通道 1 那两条已经归一化的路由"。
  // 判据在 `tests\channels_check.cpp` 的 `[5]`，用的是 `live`（没写斜杠）
  // 撞通道 1 的 `/live` 这个形状，并且做过变异检验（`out\ac-mut` 的 m5）。
  if (c.channels.items.size() > ChannelsConfig::kMaxChannels) {
    notes.push_back(fmt("channels.items 有 %zu 项，超过上限 %zu 项，多出来的已丢弃",
                        c.channels.items.size(), ChannelsConfig::kMaxChannels));
    c.channels.items.resize(ChannelsConfig::kMaxChannels);
    changed = true;
  }
  {
    // 通道 1 的两条路由先占位 —— 撞上它们和撞上别的通道是同一件事：一个路径
    // 只能挂一条流，否则 NVR 拉到的是"碰巧先注册的那一条"，而它看起来完全正常。
    // **不看 `sub_enabled`**：路径是"留了位置"，不是"正在用"，让占用规则与
    // 另一个开关有关只会让"为什么这项被丢了"变难讲。
    std::vector<std::string> used{c.rtsp.path, c.rtsp.sub_path};
    std::vector<unsigned> used_display;
    std::vector<ChannelConfig> kept;
    kept.reserve(c.channels.items.size());
    for (std::size_t i = 0; i < c.channels.items.size(); ++i) {
      ChannelConfig ch = c.channels.items[i];
      std::string const pre = fmt("channels.items[%zu]", i);
      // 序号 = 在数组里的位置 + 2（通道 1 不是 items 的一项）。
      //
      // **去重丢项时不重编号。** 重编号会悄悄改掉后面每一项的默认路径和名字：
      // 丢掉第 3 项，第 4 项就从 `/ch4` 变成 `/ch3`，而 NVR 上那个书签是按
      // `/ch4` 存的。保住"通道 N ↔ items[N-2]"这条恒等式，每一条 note 才是
      // 可执行的（"通道 5 的 rtsp_path 撞了" ⇒ 去看 items[3]）；代价只是
      // 序号可能出现空档，而空档不伤人。
      int const no = static_cast<int>(i) + 2;
      if (ch.name.empty()) ch.name = fmt("通道 %d", no);
      if (ch.source != "display" && ch.source != "webcam") {
        notes.push_back(pre + ".source 不认识「" + ch.source +
                        "」，已改成 display（可选：display / webcam）");
        ch.source = "display";
        changed = true;
      }
      auto lead = [&](char const* fname, std::string& p, std::string def) {
        if (p.empty()) p = std::move(def);
        if (p[0] != '/') {
          notes.push_back(pre + "." + fname + " 必须以 / 开头，已改成 /" + p);
          p = "/" + p;
          changed = true;
        }
      };
      lead("rtsp_path", ch.rtsp_path, fmt("/ch%d", no));
      lead("rtsp_sub_path", ch.rtsp_sub_path, fmt("/ch%d_sub", no));

      // 去重判据。**摄像头项不参与 `display_index` 去重**：那个字段对 webcam
      // 项根本没有意义（默认全是 0），拿它去重会把"同一台摄像头的两路不同画质"
      // 误杀成一项。同一台摄像头建两路是合法的，靠 `rtsp_path` 区分即可。
      std::string why;
      std::string how;
      if (ch.source == "display" &&
          std::find(used_display.begin(), used_display.end(), ch.display_index) !=
              used_display.end()) {
        why = fmt("display_index = %u 与前面某一项重复", ch.display_index);
        how = "同一块显示器只会建一个通道，想改它就把那一项改掉、别再加一项";
      } else if (std::find(used.begin(), used.end(), ch.rtsp_path) != used.end()) {
        why = "rtsp_path " + ch.rtsp_path + " 与已有路由重复";
        how = "给这一项换一个 rtsp_path 就能保住它";
      } else if (std::find(used.begin(), used.end(), ch.rtsp_sub_path) !=
                 used.end()) {
        why = "rtsp_sub_path " + ch.rtsp_sub_path + " 与已有路由重复";
        how = "给这一项换一个 rtsp_sub_path，或者把 sub_enabled 关掉";
      }
      if (!why.empty()) {
        notes.push_back(pre + " 已丢弃：" + why + "（" + how + "）");
        changed = true;
        continue;
      }
      // 两条路径**都查完再占位**：先占 `rtsp_path` 再发现 `rtsp_sub_path` 撞了
      // 而丢掉整项的话，那条 `rtsp_path` 就成了一个谁也不用的幽灵占位，
      // 后面的项会为它被误判成重复。
      //
      // 这里还依赖上面 `lead()` **已经跑过**这件事：占位的是两条**非空**的路径。
      // 顺序反过来的话，`rtsp_sub_path` 空着就往 `used` 里塞一个空串，于是
      // 后面每一个也没写 sub_path 的项都会被它判成重复 —— 症状是"第二块显示
      // 器莫名消失，理由是子码流路径重复"，而配置里根本没有一条子码流路径。
      used.push_back(ch.rtsp_path);
      used.push_back(ch.rtsp_sub_path);
      if (ch.source == "display") used_display.push_back(ch.display_index);

      // 档位展开。与顶层那段同构，只是档位标签要带上"是哪一项"。
      if (QualityTier const* t = find_quality_tier(ch.quality)) {
        auto const snap = [&](char const* fname, int& v, int want) {
          if (v == want) return;
          notes.push_back(fmt("%s 的画质档位「%s」把 %s 从 %d 改成 %d",
                              pre.c_str(), t->label, fname, v, want));
          v = want;
          changed = true;
        };
        snap("fps", ch.fps, t->fps);
        snap("bitrate_kbps", ch.bitrate_kbps, t->bitrate_kbps);
        snap("max_height", ch.max_height, t->max_height);
        if (ch.sub_enabled != t->sub_enabled) {
          notes.push_back(fmt("%s 的画质档位「%s」把 sub_enabled 改成 %s",
                              pre.c_str(), t->label,
                              t->sub_enabled ? "true" : "false"));
          ch.sub_enabled = t->sub_enabled;
          changed = true;
        }
      } else if (ch.quality != "custom") {
        notes.push_back(pre + ".quality 不认识「" + ch.quality +
                        "」，已改成 custom（可选：high / balanced / smooth / "
                        "verylow / custom）");
        ch.quality = "custom";
        changed = true;
      }
      clamp((pre + ".fps").c_str(), ch.fps, 1, 120);
      clamp((pre + ".bitrate_kbps").c_str(), ch.bitrate_kbps, 100, 100000);
      // 0 = 不缩放，同 capture.max_height，所以下界是 0。
      clamp((pre + ".max_height").c_str(), ch.max_height, 0, 4320);
      clamp((pre + ".sub_height").c_str(), ch.sub_height, 120, 2160);
      clamp((pre + ".sub_bitrate_kbps").c_str(), ch.sub_bitrate_kbps, 32, 100000);
      kept.push_back(std::move(ch));
    }
    // 无条件赋值：上面每一项都是在**副本**上改的（名字、路径、展开后的档位、
    // 夹取过的数值），不写回就等于全都没做。丢项的情形也走这里。
    c.channels.items = std::move(kept);
  }

  if (c.onvif.user.empty()) {
    notes.push_back("onvif.user 不能为空，已改成 admin");
    c.onvif.user = "admin";
    changed = true;
  }
  if (c.web.enabled && c.rtsp.enabled && c.web.port == c.rtsp.port) {
    // 两个服务抢同一个端口，先起的赢，后起的 bind 失败。与其让人对着
    // "web 起不来了"猜，不如在这里说清楚。
    notes.push_back(fmt("web.port 与 rtsp.port 都是 %d，Web 已改到 %d",
                        c.web.port, c.web.port + 1));
    c.web.port += 1;
    changed = true;
  }
  return changed;
}

namespace {

/// `osd` 那棵子树。**只有这一处构造它** —— `config_to_json` 与 `osd_to_json`
/// 都从这里取，所以"配置文件里的 osd"和"用来比较变没变的那份文本"不可能
/// 是两份不同的字段清单。
json osd_json_node(OsdConfig const& o) {
  json items = json::array();
  for (auto const& it : o.items) {
    items.push_back(json{
        {"enabled", it.enabled},
        {"type", it.type},
        {"text", it.text},
        {"position", it.position},
        {"x_pct", it.x_pct},
        {"y_pct", it.y_pct},
        {"font_size", it.font_size},
        {"date_format", it.date_format},
        {"time_format", it.time_format},
    });
  }
  return json{
      {"enabled", o.enabled},
      {"adaptive_color", o.adaptive_color},
      {"outline", o.outline},
      {"plate_alpha", o.plate_alpha},
      {"items", std::move(items)},
  };
}

/// `channels` 那棵子树。**只有这一处构造它**，纪律同 `osd_json_node`：
/// `config_to_json` 从这里取，而 `diff_config` 又只比 `config_to_json` 的输出，
/// 于是"落盘的字段清单"和"用来判变没变的字段清单"不可能漂成两份。
///
/// **空 `items` 时调用方整个键都不写**（见 `config_to_json`），所以这里不需要
/// 也没有"空数组"这个输出形态。
json channels_json_node(ChannelsConfig const& c) {
  json items = json::array();
  for (auto const& ch : c.items) {
    items.push_back(json{
        {"enabled", ch.enabled},
        {"name", ch.name},
        {"source", ch.source},
        {"display_index", ch.display_index},
        {"camera", ch.camera},
        {"quality", ch.quality},
        {"fps", ch.fps},
        {"bitrate_kbps", ch.bitrate_kbps},
        {"max_height", ch.max_height},
        {"sub_enabled", ch.sub_enabled},
        {"sub_height", ch.sub_height},
        {"sub_bitrate_kbps", ch.sub_bitrate_kbps},
        {"rtsp_path", ch.rtsp_path},
        {"rtsp_sub_path", ch.rtsp_sub_path},
    });
  }
  return json{{"items", std::move(items)}};
}

}  // namespace

std::string osd_to_json(OsdConfig const& o) { return osd_json_node(o).dump(); }

std::string config_to_json(AppConfig const& c, bool redact_secrets) {
  json j;
  j["capture"] = json{
      {"display_index", c.capture.display_index},
      {"fps", c.capture.fps},
      {"bitrate_kbps", c.capture.bitrate_kbps},
      {"gop_seconds", c.capture.gop_seconds},
      {"threads", c.capture.threads},
      {"preset", c.capture.preset},
      {"tune", c.capture.tune},
      {"profile", c.capture.profile},
      {"quality", c.capture.quality},
      {"max_height", c.capture.max_height},
      {"latency", c.capture.latency},
  };
  j["rtsp"] = json{
      {"enabled", c.rtsp.enabled},
      {"port", c.rtsp.port},
      {"path", c.rtsp.path},
      {"sub_enabled", c.rtsp.sub_enabled},
      {"sub_path", c.rtsp.sub_path},
      {"sub_height", c.rtsp.sub_height},
      {"sub_bitrate_kbps", c.rtsp.sub_bitrate_kbps},
  };
  j["audio"] = json{
      {"enabled", c.audio.enabled},
      {"system", c.audio.system},
      {"microphone", c.audio.microphone},
      {"device", c.audio.device},
      {"bitrate_kbps", c.audio.bitrate_kbps},
      {"mic_gain", c.audio.mic_gain},
      {"system_gain", c.audio.system_gain},
  };
  j["web"] = json{
      {"enabled", c.web.enabled},
      {"host", c.web.host},
      {"port", c.web.port},
      {"static_root", c.web.static_root},
      {"auth_enabled", c.web.auth_enabled},
      {"user", c.web.user},
      {"password", redact_secrets && !c.web.password.empty() ? kSecretSentinel
                                                             : c.web.password},
  };
  j["onvif"] = json{
      {"enabled", c.onvif.enabled},
      {"user", c.onvif.user},
      {"password", redact_secrets && !c.onvif.password.empty() ? kSecretSentinel
                                                               : c.onvif.password},
      {"manufacturer", c.onvif.manufacturer},
      {"model", c.onvif.model},
      {"firmware_version", c.onvif.firmware_version},
      {"hardware_id", c.onvif.hardware_id},
      {"serial_number", c.onvif.serial_number},
      {"device_uuid", c.onvif.device_uuid},
      {"device_name", c.onvif.device_name},
      {"location", c.onvif.location},
  };
  j["osd"] = osd_json_node(c.osd);
  j["adaptive"] = json{
      {"mode", c.adaptive.mode},
      {"min_fps", c.adaptive.min_fps},
  };
  j["time"] = json{
      {"mode", c.time.mode},
      {"ntp_server", c.time.ntp_server},
      {"ntp_interval_s", c.time.ntp_interval_s},
      {"offset_s", c.time.offset_s},
  };
  j["log"] = json{
      {"audit", c.log.audit},
      {"audit_keep_days", c.log.audit_keep_days},
  };
  // **一项都没有时这个键整个不写。** 这不是省字节：`channels` 键的存在与否
  // 就是兼容边界本身 —— 缺了它（等价于 `items` 为空）读回来是"只有通道 1"，
  // 也就是第五轮之前的行为，落盘的字节和那时一字不差（`config_to_json` 的
  // 输出被 `diff_config` / 保存路径直接比较，多一个空数组就会让每台已装机器
  // 的配置在第一次启动时报一次"配置变了"）。
  if (!c.channels.items.empty()) j["channels"] = channels_json_node(c.channels);
  return j.dump(2);
}

void diff_config(AppConfig const& a, AppConfig const& b,
                 std::vector<std::string>& out) {
  // **故意走 JSON，而不是手写第二份字段清单。** 手写的那份迟早会和
  // config_to_json 里的字段集不一致，而症状恰好是这段代码要防的那一种：
  // 某个字段改了却不出现在 pending 里，于是界面上显示"已生效"。
  // 比较同一个序列化器的输出，字段集就是同一份，加字段时不可能漏。
  json const ja = json::parse(config_to_json(a, /*redact_secrets=*/false), nullptr, false);
  json const jb = json::parse(config_to_json(b, /*redact_secrets=*/false), nullptr, false);
  if (ja.is_discarded() || jb.is_discarded()) return;  // 理论上到不了这里
  // **数组是整体比较，不是逐项比。** `walk_diff` 只在"两边都是对象"时往下钻，
  // 数组走到叶子那一步就整块 `a != b` 了。于是 `channels.items`（和 `osd.items`）
  // 里改任何一项、加一项、甚至只是换了个顺序，报出来的都是**同一条路径**
  // `channels.items` 一次。
  //
  // 这正是想要的粒度：pending 列表要回答的是"这个改动生效了没有"，而
  // `channels.items` 整块就是一次原子改动（`config_from_json` 那边也是整数组
  // 替换）。反过来若改成按下标逐项比，重排通道会报出一长串 `channels.items[1].fps`
  // 之类的假差异，而实际生效的是同一次替换。
  walk_diff(ja, jb, std::string(), out);
  // **把顺序钉死，而不是靠"json 的键本来就是有序的"这个论证。** 调用方要用
  // 它做集合运算（"这次变了的" 减去 "还没生效的"），有序才能用二分；而依赖
  // nlohmann 底层容器的那条链太长，将来换个 `ordered_json` 就悄悄断了 ——
  // 断掉的表现是集合运算给出错误结果，不是编译错误。
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool config_from_json(std::string const& text, AppConfig& out, std::string& err) {
  json j;
  try {
    j = json::parse(text);
  } catch (std::exception const& e) {
    err = std::string("配置不是合法的 JSON：") + e.what();
    return false;
  }
  if (!j.is_object()) {
    err = "配置的顶层必须是一个 JSON 对象。";
    return false;
  }

  try {
    if (auto const it = j.find("capture"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "display_index", out.capture.display_index);
      take(s, "fps", out.capture.fps);
      take(s, "bitrate_kbps", out.capture.bitrate_kbps);
      take(s, "gop_seconds", out.capture.gop_seconds);
      take(s, "threads", out.capture.threads);
      take(s, "preset", out.capture.preset);
      take(s, "tune", out.capture.tune);
      take(s, "profile", out.capture.profile);
      take(s, "quality", out.capture.quality);
      take(s, "max_height", out.capture.max_height);
      take(s, "latency", out.capture.latency);
    }
    if (auto const it = j.find("rtsp"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "enabled", out.rtsp.enabled);
      take(s, "port", out.rtsp.port);
      take(s, "path", out.rtsp.path);
      take(s, "sub_enabled", out.rtsp.sub_enabled);
      take(s, "sub_path", out.rtsp.sub_path);
      take(s, "sub_height", out.rtsp.sub_height);
      take(s, "sub_bitrate_kbps", out.rtsp.sub_bitrate_kbps);
    }
    if (auto const it = j.find("audio"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "enabled", out.audio.enabled);
      take(s, "system", out.audio.system);
      take(s, "microphone", out.audio.microphone);
      take(s, "device", out.audio.device);
      take(s, "bitrate_kbps", out.audio.bitrate_kbps);
      take(s, "mic_gain", out.audio.mic_gain);
      take(s, "system_gain", out.audio.system_gain);
    }
    if (auto const it = j.find("web"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "enabled", out.web.enabled);
      take(s, "host", out.web.host);
      take(s, "port", out.web.port);
      take(s, "static_root", out.web.static_root);
      take(s, "auth_enabled", out.web.auth_enabled);
      take(s, "user", out.web.user);
      // 和 onvif.password 同一套：临时值初值是哨兵，"键不存在"和"值就是哨兵"
      // 在这里是同一件事 —— 调用方没打算改口令。初值留空串的话，
      // `{"web":{"port":9000}}` 这种局部补丁会把口令**清成空串**，而空口令
      // 在这个服务里是合法的，于是它会静默地变成"谁都能登进来"。（陷阱 50）
      std::string wpw = kSecretSentinel;
      take(s, "password", wpw);
      if (wpw != kSecretSentinel) out.web.password = wpw;
    }
    if (auto const it = j.find("onvif"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "enabled", out.onvif.enabled);
      take(s, "user", out.onvif.user);
      take(s, "manufacturer", out.onvif.manufacturer);
      take(s, "model", out.onvif.model);
      take(s, "firmware_version", out.onvif.firmware_version);
      take(s, "hardware_id", out.onvif.hardware_id);
      take(s, "serial_number", out.onvif.serial_number);
      take(s, "device_uuid", out.onvif.device_uuid);
      take(s, "device_name", out.onvif.device_name);
      take(s, "location", out.onvif.location);

      // 密码单独处理：收到哨兵就表示"这一项没改"，**必须**保持原值。
      // 少了这一条，Web UI 上"改个帧率顺手保存"会把口令覆写成
      // __unchanged__ 六个字母，而且没有任何提示。
      //
      // 临时值**初值就是哨兵**，因为"字段不存在"和"字段是哨兵"在这里是同一件
      // 事：都表示调用方没打算改口令。初值留空串的话，`{"onvif":{"model":"x"}}`
      // 这种局部补丁会把口令清成空串 —— 而空口令在本服务里是**合法**的
      // （见 32 号陷阱：挑战照发、HA1 用空口令算），于是它会静默地变成"谁都能进"。
      // 其余字段没有这个问题，因为 `take` 直接写进 `out`，不经过临时值。
      std::string pw = kSecretSentinel;
      take(s, "password", pw);
      if (pw != kSecretSentinel) out.onvif.password = pw;
    }
    if (auto const it = j.find("osd"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "enabled", out.osd.enabled);
      take(s, "adaptive_color", out.osd.adaptive_color);
      take(s, "outline", out.osd.outline);
      take(s, "plate_alpha", out.osd.plate_alpha);

      // `items` 是**整体替换**，不是逐项合并：数组就是列表本身，把新的一项
      // 并进旧列表会让"删掉了一项"这个操作在 POST 回来之后又冒出来。
      // 缺这个键时保留默认的两项（时间 + 名称）—— 老配置文件里没有它。
      if (auto const ai = s.find("items"); ai != s.end() && !ai->is_null()) {
        if (!ai->is_array()) {
          err = "osd.items 必须是一个数组。";
          return false;
        }
        std::vector<OsdItemConfig> parsed;
        for (auto const& e : *ai) {
          if (!e.is_object()) {
            err = "osd.items 的每一项都必须是一个对象。";
            return false;
          }
          // 每一项**从默认值起**，只覆盖写了的键 —— 和顶层同一套容忍规则。
          OsdItemConfig one;
          take(e, "enabled", one.enabled);
          take(e, "type", one.type);
          take(e, "text", one.text);
          take(e, "position", one.position);
          take(e, "x_pct", one.x_pct);
          take(e, "y_pct", one.y_pct);
          take(e, "font_size", one.font_size);
          take(e, "date_format", one.date_format);
          take(e, "time_format", one.time_format);
          parsed.push_back(std::move(one));
        }
        out.osd.items = std::move(parsed);
      }
    }
    if (auto const it = j.find("channels"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      // `items` 是**整体替换**，与 `osd.items` 同一条规则（理由也一样：数组就是
      // 列表本身，把新项并进旧列表会让"删掉了一个通道"在 POST 回来之后又冒出来）。
      //
      // **缺这个键时保留原值**，不是清空：这里的默认值和其他段一样是"不写就不动"。
      // 老配置文件和只发 `{"channels":{"items":[...]}}` 的补丁都走这一条。
      if (auto const ai = s.find("items"); ai != s.end() && !ai->is_null()) {
        if (!ai->is_array()) {
          err = "channels.items 必须是一个数组。";
          return false;
        }
        std::vector<ChannelConfig> parsed;
        for (auto const& e : *ai) {
          if (!e.is_object()) {
            err = "channels.items 的每一项都必须是一个对象。";
            return false;
          }
          // 与 osd.items 同一套容忍规则：每一项**从默认值起**，只覆盖写了的键。
          // 所以 `{"source":"webcam"}` 一项就够，其余字段（fps、路径、画质档位）
          // 全部走默认，并由 validate_config 按序号补出名字和路径。
          ChannelConfig one;
          take(e, "enabled", one.enabled);
          take(e, "name", one.name);
          take(e, "source", one.source);
          take(e, "display_index", one.display_index);
          take(e, "camera", one.camera);
          take(e, "quality", one.quality);
          take(e, "fps", one.fps);
          take(e, "bitrate_kbps", one.bitrate_kbps);
          take(e, "max_height", one.max_height);
          take(e, "sub_enabled", one.sub_enabled);
          take(e, "sub_height", one.sub_height);
          take(e, "sub_bitrate_kbps", one.sub_bitrate_kbps);
          take(e, "rtsp_path", one.rtsp_path);
          take(e, "rtsp_sub_path", one.rtsp_sub_path);
          parsed.push_back(std::move(one));
        }
        out.channels.items = std::move(parsed);
      }
    }
    if (auto const it = j.find("adaptive"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "mode", out.adaptive.mode);
      take(s, "min_fps", out.adaptive.min_fps);
    }
    if (auto const it = j.find("time"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "mode", out.time.mode);
      take(s, "ntp_server", out.time.ntp_server);
      take(s, "ntp_interval_s", out.time.ntp_interval_s);
      take(s, "offset_s", out.time.offset_s);
    }
    if (auto const it = j.find("log"); it != j.end() && it->is_object()) {
      auto const& s = *it;
      take(s, "audit", out.log.audit);
      take(s, "audit_keep_days", out.log.audit_keep_days);
    }
  } catch (std::exception const& e) {
    err = std::string("配置字段类型不对：") + widen_field_name(e.what());
    return false;
  }
  return true;
}

}  // namespace d2o
