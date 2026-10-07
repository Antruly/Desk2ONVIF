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

  clamp("capture.fps", c.capture.fps, 1, 120);
  clamp("capture.bitrate_kbps", c.capture.bitrate_kbps, 100, 100000);
  clamp("capture.gop_seconds", c.capture.gop_seconds, 1, 60);
  clamp("capture.threads", c.capture.threads, 0, 128);
  clamp("rtsp.port", c.rtsp.port, 1, 65535);
  clamp("rtsp.sub_height", c.rtsp.sub_height, 120, 2160);
  clamp("rtsp.sub_bitrate_kbps", c.rtsp.sub_bitrate_kbps, 32, 100000);
  clamp("web.port", c.web.port, 1, 65535);

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
      {"device_name", c.onvif.device_name},
      {"location", c.onvif.location},
  };
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
  } catch (std::exception const& e) {
    err = std::string("配置字段类型不对：") + widen_field_name(e.what());
    return false;
  }
  return true;
}

}  // namespace d2o
