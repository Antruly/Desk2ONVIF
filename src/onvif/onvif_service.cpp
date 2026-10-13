#include "onvif/onvif_service.h"

#include "common/device_state.h"
#include "common/log.h"
#include "onvif/digest.h"
#include "onvif/net_identity.h"

#include <webapp/uvcpp_web_request.h>
#include <webapp/uvcpp_web_response.h>
#include <wsdl/uvcpp_soap_message.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace d2o {
namespace {

using uvcpp::uvcpp_soap_message;
using uvcpp::uvcpp_soap_parse;
using uvcpp::uvcpp_soap_version;
using uvcpp::uvcpp_web_request;
using uvcpp::uvcpp_web_response;
using uvcpp::uvcpp_web_sent_info;

// ONVIF 的命名空间。**全部声明在 Envelope 上**，见头文件里那段说明 ——
// 这样内层元素想用哪个前缀就用哪个，不存在"声明管不到兄弟元素"的问题。
constexpr char kNsDevice[] = "http://www.onvif.org/ver10/device/wsdl";
constexpr char kNsMedia[] = "http://www.onvif.org/ver10/media/wsdl";
constexpr char kNsSchema[] = "http://www.onvif.org/ver10/schema";
constexpr char kNsNetwork[] = "http://www.onvif.org/ver10/network/wsdl";
// ONVIF 的**错误**命名空间（`ter`）。Fault 的子码就来自这里，
// 例如 `ter:ActionNotSupported`、`ter:NoSuchService`。
constexpr char kNsError[] = "http://www.onvif.org/ver10/error";

// 令牌。全都**不含空格和特殊字符**：有些 NVR 会把它直接拼进 URL 查询串。
constexpr char kTokenVideoSource[] = "VideoSource";
constexpr char kTokenVsConfig[] = "VideoSourceConfig";

/// 审计去重闸门的窗口（秒）。见 `OnvifService::Impl::note_audit`。
///
/// 一分钟这个数与网页侧登录失败那条闸门的口径一致（那个由
/// `SessionStore::kWindowSeconds` 决定），但**两者没有代码上的联系** ——
/// 它们防的是两种不同的重复（那一个是有人在试口令，这一个是 NVR 在重发配置）。
constexpr int kAuditDedupeSeconds = 60;

std::string xml_escape(std::string const& s) {
  std::string out;
  out.reserve(s.size() + 16);
  for (char c : s) {
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&apos;"; break;
      default: out.push_back(c);
    }
  }
  return out;
}

/// 属性值也要转义。ONVIF 里 `Uri="rtsp://..."` 这类值出现得非常频繁，
/// 忘了转义就是一个只在特定地址下才炸的 bug。
std::string attr(std::string const& s) { return xml_escape(s); }

int clamp_int(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --- 从请求体里抠标量 ------------------------------------------------------
//
// **不引 XML 库、也不建树。** 请求体在这条链上只用来取几个我们自己认识的
// 标量（`ProfileToken`、`SetSystemDateAndTime` 的那五个数字），而报文来自
// 网络这件事的安全性由 `uvcpp_soap_parse` 那一层负责（它有深度/节点数上限）。
// 这里假设输入**已经是格式良好的 XML**，只做字符串定位。
//
// 三个函数的区别只有一层：`xml_block` 取元素的内容，`xml_text` 取它里面第一
// 段纯文本，`xml_int` 再把它解析成一个带范围检查的整数。

/// 抠出第一个 `<某前缀:Tag>` … `</某前缀:Tag>` 之间的内容。找不到返回空串。
///
/// **为什么必须按块取，而不是扁平地找一个 `Hour>`**：`SetSystemDateAndTime`
/// 里 `UTCDateTime` 和 `LocalDateTime` 各有一个 `Time`/`Date`，里面又各有一个
/// `Hour`/`Minute`/…。扁平地找第一个 `Hour`，在"两个都给"的报文上会读到第一
/// 个块里的值而以为自己读的是唯一那个 —— 那是一个**读起来完全正常、结果差
/// 八小时**的错。
///
/// 只认**完整元素名**（`UTCDateTime` 而不是 `DateTime`）：`LocalDateTime` 也
/// 以 `DateTime` 结尾，后缀匹配在这里会当场出错。
std::string xml_block(std::string const& xml, std::string const& tag) {
  std::size_t from = 0;
  while (true) {
    std::size_t const at = xml.find(tag + ">", from);
    if (at == std::string::npos) return {};
    std::size_t const lt = xml.rfind('<', at);
    if (lt == std::string::npos) return {};
    // `lt` 与 `>` 之间是**完整的限定名**（`tt:Hour` / `tds:UTCDateTime`）。
    // 长度要从 `<` 的下一个字符数到那个 `>`，而 `at` 指向的是**元素名本身**
    // 的第一个字符 —— `find(tag + ">")` 回的是匹配的**起点**，不是末尾那个
    // `>`。所以还得再加上 `tag.size()`。
    //
    // 这里以前漏了这一项，于是 `qname` 只取到前缀那一小截（`"tds:"`），
    // 任何输入都匹配不上、`xml_block` 恒返回空串。它一直没暴露，是因为
    // **在 `SetSystemDateAndTime` 之前没有任何调用方** —— `profile_token_from_body`
    // 看着像用它，其实走的是裸 `find`。症状是 `SetSystemDateAndTime` 对任何
    // 客户端都回 "no usable UTCDateTime"，包括我们自己发的报文。
    std::string const qname = xml.substr(lt + 1, at + tag.size() - lt - 1);
    // 裸名（`<Hour>`，靠默认命名空间）与带前缀的（`<tt:Hour>`）都要认，而且
    // **只有**这两种。不能只比后缀：找 `DateTime` 会撞上 `UTCDateTime`，
    // 那种错会安静地取到另一个元素的内容。
    bool const bare = qname.size() == tag.size();
    bool const prefixed = qname.size() > tag.size() &&
                          qname[qname.size() - tag.size() - 1] == ':';
    if ((bare || prefixed) &&
        qname.compare(qname.size() - tag.size(), tag.size(), tag) == 0) {
      std::size_t const body_from = at + tag.size() + 1;
      std::string const close = "</" + qname + ">";
      std::size_t const end = xml.find(close, body_from);
      if (end == std::string::npos) return {};
      return xml.substr(body_from, end - body_from);
    }
    from = at + tag.size() + 1;
  }
}

/// `xml_block` 里第一段纯文本。数字元素用这个就够 —— 它们不会有子元素。
std::string xml_text(std::string const& xml, std::string const& tag) {
  std::string const b = xml_block(xml, tag);
  std::size_t const lt = b.find('<');
  std::string s = (lt == std::string::npos) ? b : b.substr(0, lt);
  // 去掉两头的空白：报文里 `\n  <tt:Hour>12</tt:Hour>` 很常见。
  std::size_t const a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  std::size_t const z = s.find_last_not_of(" \t\r\n");
  return s.substr(a, z - a + 1);
}

/// 取一个**属性**值：`<tt:Bounds x="0" width="1920" height="1080"/>` 里的
/// `height`，或 `<tt:Configuration token="VideoEncoderConfig_main">` 里的 `token`。
///
/// 为什么不能直接 `find(name + "=\"")`：属性名是别的名字的**后缀**这种情况很常见
/// —— `Name=` 是 `ProfileName=` 的子串，`token=` 是 `SourceToken=` 的子串（大小写
/// 不同只是运气）。直接 find 会取到隔壁那个属性的值，而"取错了值"在这里的表现是
/// 一个**看起来正确的数字**（比如把 `SourceToken` 的值当成 token，解析不出来就
/// 静默退回主码流）。所以要求匹配处的**前一个字符是属性之间的分隔符**。
std::string xml_attr(std::string const& xml, std::string const& name) {
  if (name.empty()) return {};
  std::size_t from = 0;
  while (true) {
    std::size_t const at = xml.find(name, from);
    if (at == std::string::npos) return {};
    from = at + name.size();
    // `at == 0` 也拒掉：属性前面一定有个 `<` 或空白（XML 的良构要求）。
    if (at == 0) continue;
    char const prev = xml[at - 1];
    if (prev != ' ' && prev != '\t' && prev != '\r' && prev != '\n' && prev != '<') {
      continue;
    }
    std::size_t i = from;
    while (i < xml.size() && (xml[i] == ' ' || xml[i] == '\t')) ++i;
    if (i >= xml.size() || xml[i] != '=') continue;
    ++i;
    while (i < xml.size() && (xml[i] == ' ' || xml[i] == '\t')) ++i;
    if (i >= xml.size() || (xml[i] != '"' && xml[i] != '\'')) continue;
    char const q = xml[i++];
    std::size_t const end = xml.find(q, i);
    if (end == std::string::npos) return {};
    return xml.substr(i, end - i);
  }
}

/// `xml_block` 的**标签**版本：返回 `Bounds` 这个元素的整个标签
/// （`<tt:Bounds x="0" width="1920"/>`），而不是它的内容。
///
/// 为什么需要它：**自闭合元素没有内容**。`<tt:Bounds ... />` 找不到
/// `</tt:Bounds>`，于是 `xml_block` 返回空串 —— 而属性恰恰在**标签里面**，
/// 走那条路一个都取不到。ONVIF 里这个形状很常见（`Bounds`、`RateControl` 的
/// 几个子元素都可能被写成自闭合的），要读属性就得走这一条。
std::string xml_tag(std::string const& xml, std::string const& tag) {
  std::size_t from = 0;
  while (true) {
    std::size_t const lt = xml.find('<', from);
    if (lt == std::string::npos) return {};
    from = lt + 1;
    // 元素名从 `<` 的下一个字符起，到第一个空白 / `>` / `/` 为止。
    std::size_t e = lt + 1;
    while (e < xml.size() && xml[e] != ' ' && xml[e] != '\t' && xml[e] != '\r' &&
           xml[e] != '\n' && xml[e] != '>' && xml[e] != '/') {
      ++e;
    }
    std::string const qname = xml.substr(lt + 1, e - lt - 1);
    // 和 `xml_block` 同一套规则：裸名或**恰好一个前缀**。只比后缀的话，
    // 找 `Bounds` 会命中 `OtherBounds`。
    bool match = (qname == tag);
    if (!match && qname.size() > tag.size() + 1 &&
        qname[qname.size() - tag.size() - 1] == ':' &&
        qname.compare(qname.size() - tag.size(), tag.size(), tag) == 0) {
      match = true;
    }
    if (!match) continue;
    std::size_t const gt = xml.find('>', e);
    if (gt == std::string::npos) return {};
    return xml.substr(lt, gt - lt + 1);
  }
}

/// 把一个**已经是字符串**的值按十进制整数解析并检查范围。
///
/// **越界当失败**，不当成"夹一下"。ONVIF 的 `SetSystemDateAndTime` 送一个 13 月
/// 进来，那是客户端在说胡话，而我们悄悄改成 12 月会让设备时钟**看起来被设成了、
/// 其实不是** —— 那种错在 NVR 界面上和成功一模一样。
bool str_int_in_range(std::string const& s, int lo, int hi, int& out) {
  if (s.empty()) return false;
  for (std::size_t i = 0; i < s.size(); ++i) {
    char const c = s[i];
    if (c >= '0' && c <= '9') continue;
    // 允许前导 `-`，但只允许在第一个字符上。
    //
    // 这里以前写的是 `for (char c : s)` 再判 `&c == &s[0]` —— `c` 是**副本**，
    // 那个地址永远不等于 `s[0]` 的地址，于是负号一律被判成非法。功能上没暴露
    // （年月日时分秒本来就不该是负的），但那句注释在说一件不会发生的事。
    if (i == 0 && c == '-') continue;
    return false;
  }
  errno = 0;
  char* end = nullptr;
  long const v = std::strtol(s.c_str(), &end, 10);
  if (errno != 0 || end == s.c_str() || v < lo || v > hi) return false;
  out = static_cast<int>(v);
  return true;
}

/// 取一个子元素的整数并检查范围。
///
/// 单独抽出 `str_int_in_range` 是因为有一半的整数在 ONVIF 里是**属性**而不是
/// 子元素（`<tt:Bounds ... height="1080"/>`）—— 那条路走不到 `xml_text`。
/// 而"解析 + 范围检查"这两件事必须两边完全一样，否则属性那条路上的越界值
/// 会被悄悄接受，同一个数字从两个入口进来得到两种待遇。
bool xml_int(std::string const& xml, std::string const& tag, int lo, int hi,
             int& out) {
  return str_int_in_range(xml_text(xml, tag), lo, hi, out);
}

/// 取一个**属性**的整数并检查范围。
bool xml_attr_int(std::string const& xml, std::string const& name, int lo,
                  int hi, int& out) {
  return str_int_in_range(xml_attr(xml, name), lo, hi, out);
}

/// 取一个**属性**的浮点并检查范围。目前只有一个调用点：`tt:Pos` 的 `x`/`y`
/// （`tt:Vector` 的两个必填属性）。
///
/// 走浮点而不是整数是因为它们的取值是**归一化**的 0..1（`0.5` 这类）。
/// 越界当失败、不当夹取 —— 理由和 `str_int_in_range` 那段一样：把 1.7 悄悄
/// 夹成 1.0 会让"位置被挪到了边上"看起来像一次成功的设置。
bool xml_attr_float(std::string const& xml, std::string const& name, double lo,
                     double hi, double& out) {
  std::string const s = xml_attr(xml, name);
  if (s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  double const v = std::strtod(s.c_str(), &end);
  // `end == s.c_str()` 表示一个字符都没吃进去（空串、或者 "abc"）。
  // 后面的字符不检查：`0.5` 后面跟什么我们都不关心，而拒绝一个合法数字
  // 比接受一个多余字符的代价大 —— 后者只是宽容。
  if (errno != 0 || end == s.c_str() || v < lo || v > hi) return false;
  out = v;
  return true;
}

/// `tt:TZ` 的取值。**两种来源都要认**，返回分钟（东为正）；认不出来返回 false
/// —— 调用方应当回 Fault，而不是当 0 处理（当成 UTC 就是整整差一个时区）。
///
///   * **xsd:duration** —— 规范里 `tt:TZ` 的类型就是这个，也是我们自己
///     `GetSystemDateAndTime` 报出去的形状：`PT8H`、`-PT5H30M`、`PT0S`。
///     NVR 发的正是这一种。注意**符号在 `P` 之前**（`-PT5H`），写成 `PT-5H`
///     不是合法的 duration —— 本文件以前就写成那样，见 `iso_tz_duration`。
///   * `UTC+08:00` / `+08:00` / `-05:00` / `UTC` —— 规范里没有，但有一些客户端
///     （和不少手写脚本、以及本项目的测试）发这一种。认下来没有坏处，不认就
///     静默退回 UTC。
bool parse_tz_minutes(std::string const& s, long& out) {
  if (s.empty()) return false;

  std::size_t p = 0;
  int sign = 1;
  if (s[p] == '-') { sign = -1; ++p; }
  else if (s[p] == '+') { ++p; }

  if (p < s.size() && s[p] == 'P') {
    // ---- xsd:duration ----
    ++p;
    // 时区偏移只可能有时间部分。`P1D` 之类的"天"对时区没有意义，拒掉比
    // 硬猜好 —— 猜错就是差一天。
    if (p >= s.size() || s[p] != 'T') return false;
    ++p;
    long total = 0;
    bool any = false;
    while (p < s.size()) {
      std::size_t q = p;
      long v = 0;
      bool digits = false;
      while (q < s.size() && s[q] >= '0' && s[q] <= '9') {
        v = v * 10 + (s[q] - '0');
        digits = true;
        ++q;
      }
      if (!digits || q >= s.size()) return false;
      char const unit = s[q];
      if (unit == 'H') total += v * 60;
      else if (unit == 'M') total += v;
      // 秒级偏移现实中不存在（历史时区除外，而这个程序不打算处理它们）：
      // 四舍五入到最近的分钟，而不是拒绝整串。
      else if (unit == 'S') total += (v >= 30) ? 1 : 0;
      else return false;
      any = true;
      p = q + 1;
    }
    if (!any) return false;
    // 现实中的时区范围是 -12:00..+14:00，比这个宽的都是发错了。
    if (total > 14 * 60) return false;
    out = sign * total;
    return true;
  }

  // ---- `UTC±HH:MM` / `±HH:MM` ----
  if (s.compare(0, 3, "UTC") == 0 || s.compare(0, 3, "utc") == 0) p = 3;
  if (p >= s.size()) { out = 0; return true; }  // 光写 "UTC"
  sign = (s[p] == '-') ? -1 : 1;
  if (s[p] != '+' && s[p] != '-') return false;
  ++p;
  if (s.size() < p + 2) return false;
  int hh = 0, mm = 0;
  for (int k = 0; k < 2; ++k) {
    if (s[p + k] < '0' || s[p + k] > '9') return false;
    hh = hh * 10 + (s[p + k] - '0');
  }
  p += 2;
  if (p < s.size() && s[p] == ':') {
    ++p;
    if (s.size() < p + 2) return false;
    for (int k = 0; k < 2; ++k) {
      if (s[p + k] < '0' || s[p + k] > '9') return false;
      mm = mm * 10 + (s[p + k] - '0');
    }
    p += 2;
  }
  if (p != s.size()) return false;
  if (hh > 14 || mm > 59) return false;
  out = sign * (static_cast<long>(hh) * 60 + mm);
  return true;
}

/// ONVIF 的时间字段是**分开的数字元素**，不是 ISO8601 字符串 ——
/// 这一点和 WS-Discovery 那边（`AppSequence/@InstanceId`）不一样。
std::string date_time_fields(std::tm const& t) {
  std::string x;
  x += "<tt:Year>" + std::to_string(t.tm_year + 1900) + "</tt:Year>";
  x += "<tt:Month>" + std::to_string(t.tm_mon + 1) + "</tt:Month>";
  x += "<tt:Day>" + std::to_string(t.tm_mday) + "</tt:Day>";
  x += "<tt:Hour>" + std::to_string(t.tm_hour) + "</tt:Hour>";
  x += "<tt:Minute>" + std::to_string(t.tm_min) + "</tt:Minute>";
  x += "<tt:Second>" + std::to_string(t.tm_sec) + "</tt:Second>";
  return x;
}

void gmtime_utc(std::time_t t, std::tm& out) {
#if defined(_WIN32)
  ::gmtime_s(&out, &t);
#else
  ::gmtime_r(&t, &out);
#endif
}

void localtime_local(std::time_t t, std::tm& out) {
#if defined(_WIN32)
  ::localtime_s(&out, &t);
#else
  ::localtime_r(&t, &out);
#endif
}

/// 把一串"钟面字段"当成 UTC 换算成 `time_t`。
///
/// **不用 `timegm` / `_mkgmtime`**：各平台名字不同（本文件的
/// `utc_offset_minutes` 当初就是为了这个才绕开的），而我们要的是"标准库两件套
/// 能推出来的东西"。做法是两步：先把字段当成**本地**钟面换成一个瞬间，再加上
/// 本地在**那个瞬间**的偏移。
///
/// 偏移必须在那个瞬间上算，不能用"此刻"的 —— 跨夏令时切换时两者差一小时，
/// 而 NVR 完全可能下发一个向导里手选的、别的日期的时刻。
///
/// **方向是「加」。** 同一个钟面读数，读成 UTC 得到的瞬间比读成东八区本地
/// **晚** 8 小时（"13:03 UTC" 比 "13:03 +08:00" 晚 —— 后者其实就是 05:03 UTC）。
/// 这里以前写的是减，于是被设的每一个时间都偏了 **2×偏移**（东八区 = 16 小时），
/// 而**在 UTC 的机器上它完全看不出来**：offset 为 0 时加和减是同一件事。
/// 实测（东八区）：设 `now+180` 进去，读回来的是 `now-57420`。
std::time_t utc_fields_to_time_t(std::tm const& fields) {
  std::tm g = fields;
  g.tm_isdst = -1;
  std::time_t const as_local = std::mktime(&g);
  if (as_local == static_cast<std::time_t>(-1)) return as_local;
  std::tm lt{}, gt{};
  localtime_local(as_local, lt);
  gmtime_utc(as_local, gt);
  // 两边都按**同一套**夏令时规则解释（都置 0），这样它们的差就是纯粹的偏移量，
  // 跨夏令时切换那天也不会被搅进来。
  lt.tm_isdst = 0;
  gt.tm_isdst = 0;
  std::time_t const local_t = std::mktime(&lt);
  std::time_t const utc_t = std::mktime(&gt);
  if (local_t == static_cast<std::time_t>(-1) ||
      utc_t == static_cast<std::time_t>(-1)) {
    return as_local;
  }
  return as_local + static_cast<long>(std::difftime(local_t, utc_t));
}

/// 本机相对 UTC 的偏移（分钟，东为正）。
///
/// 用 `mktime(gmtime(t))` 再和 t 相减，而不是去碰 `timegm` / `_mkgmtime` ——
/// 后两者各平台名字不同（MinGW 上不保证有），而这里只需要标准库的两件套。
/// 推导：把 UTC 的钟面当成"本地时间"换算回 time_t，得到的是"当本地钟面等于
/// 此刻 UTC 钟面"的那个瞬间；它和此刻的差就是偏移量。
///
/// `tm_isdst = -1` 让 mktime 自己判夏令时。切换前后一小时可能差一小时，
/// 对设个相机时钟来说无所谓；而写死 0 会**全年**差一小时。
long utc_offset_minutes() {
  std::time_t const now = std::time(nullptr);
  std::tm g{};
  gmtime_utc(now, g);
  g.tm_isdst = -1;
  std::time_t const as_local = std::mktime(&g);
  if (as_local == static_cast<std::time_t>(-1)) return 0;
  return static_cast<long>(std::difftime(now, as_local) / 60.0);
}

/// ISO 8601 的时长形式：`PT8H`、`-PT5H30M`。ONVIF 的 `tt:TZ` 就是这么写的。
///
/// **负号的落点是 `P` 之前，不是 `PT` 之后。** `xsd:duration` 的文法是
/// `-?P...`，所以西半球（比如 UTC-5）该报 `-PT5H`；写成 `PT-5H` 不是合法的
/// duration，严格的客户端会拒收整条 `GetSystemDateAndTimeResponse`，
/// 而宽容的客户端会把它当成 0 —— 于是 NVR 界面上的时间和画面上差 5 小时。
///
/// 这个 bug 在本机看不出来：这里时区是 UTC+8，走的是正数分支。
std::string iso_tz_duration(long minutes) {
  long const abs_m = minutes < 0 ? -minutes : minutes;
  std::string s = (minutes < 0) ? "-PT" : "PT";
  if (abs_m >= 60) {
    s += std::to_string(abs_m / 60) + "H";
    if (abs_m % 60) s += std::to_string(abs_m % 60) + "M";
  } else {
    s += std::to_string(abs_m) + "M";
  }
  return s;
}

/// 整个 SOAP 信封。Body 内容由调用方给。
///
/// **所有前缀只在这里声明一次。** 这是整个文件存在的前提：ONVIF 的响应是
/// 跨命名空间的（`<trt:GetProfilesResponse>` 里装着 `<tt:Name>`），而前缀
/// 声明的作用域只覆盖**该元素及其后代** —— 声明在顶层包装元素上就管不到
/// 兄弟，声明在 Envelope 上才全局有效。
std::string soap_envelope(uvcpp_soap_version v, std::string const& body_inner) {
  bool const v12 = (v == uvcpp_soap_version::V1_2);
  std::string x;
  x.reserve(body_inner.size() + 512);
  x += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>";
  x += "<soap:Envelope xmlns:soap=\"";
  x += v12 ? "http://www.w3.org/2003/05/soap-envelope"
           : "http://schemas.xmlsoap.org/soap/envelope/";
  x += "\" xmlns:tds=\"";  x += kNsDevice;
  x += "\" xmlns:trt=\"";  x += kNsMedia;
  x += "\" xmlns:tt=\"";   x += kNsSchema;
  x += "\" xmlns:dn=\"";   x += kNsNetwork;
  x += "\" xmlns:ter=\"";  x += kNsError;
  x += "\"><soap:Body>";
  x += body_inner;
  x += "</soap:Body></soap:Envelope>";
  return x;
}

/// Fault 的语义分类。**这不是"随便挑一个"**：ONVIF 的 Fault 表里，
/// "设备不支持这个可选操作"是 `env:Receiver` + 子码 `ter:ActionNotSupported`
/// —— 不是 `Sender`。发成 `Sender` 的话，按规范读的客户端会理解成"是你请求
/// 写错了"，有的实现会因此放弃整台设备的添加流程。
enum class FaultKind {
  /// 请求本身有问题（报文畸形、必填参数缺失）→ `Sender`。
  kBadRequest,
  /// 设备没实现这个可选操作 → `Receiver` + `ter:ActionNotSupported`。
  kNotSupported,
};

struct FaultInfo {
  FaultKind kind = FaultKind::kBadRequest;
  /// `ter:` 子码。空 = 不发子码。
  std::string subcode;
  /// 子码的下一层（`ter:NoSuchService` 这类）。空 = 不发。
  std::string sub_subcode;
  std::string reason;
};

/// 一条符合 ONVIF 的 Fault。
///
/// SOAP 1.2 的子码是**递归**的：`Code/Subcode/Value` 是这一层，再深一层要写成
/// `Code/Subcode/Subcode/Value`，而**不是**在同一个 Subcode 里并排放两个 Value。
/// 真实设备两种写法都有，但只有前者是良构的。
std::string soap_fault_xml(uvcpp_soap_version v, FaultInfo const& f) {
  bool const v12 = (v == uvcpp_soap_version::V1_2);
  std::string body;
  body += "<soap:Fault>";
  if (v12) {
    char const* const top =
        (f.kind == FaultKind::kNotSupported) ? "soap:Receiver" : "soap:Sender";
    body += "<soap:Code><soap:Value>";
    body += top;
    body += "</soap:Value>";
    if (!f.subcode.empty()) {
      body += "<soap:Subcode><soap:Value>ter:" + f.subcode + "</soap:Value>";
      if (!f.sub_subcode.empty()) {
        body += "<soap:Subcode><soap:Value>ter:" + f.sub_subcode +
                "</soap:Value></soap:Subcode>";
      }
      body += "</soap:Subcode>";
    }
    body += "</soap:Code>";
    body += "<soap:Reason><soap:Text xml:lang=\"en\">" + xml_escape(f.reason) +
            "</soap:Text></soap:Reason>";
  } else {
    // 1.1 的 `faultcode` **是**一个 QName，没有独立的子码字段；ONVIF 那套
    // `ter:` 子码塞进 `detail`。`soap:Server` 对应 1.2 的 `Receiver`。
    body += "<faultcode>";
    body += (f.kind == FaultKind::kNotSupported) ? "soap:Server" : "soap:Client";
    body += "</faultcode>";
    body += "<faultstring>" + xml_escape(f.reason) + "</faultstring>";
    if (!f.subcode.empty()) {
      body += "<detail><ter:" + f.subcode + "/></detail>";
    }
  }
  body += "</soap:Fault>";
  return soap_envelope(v, body);
}

/// 成功的响应：`<op xmlns="ns">inner</op>` 包进 Body。
std::string soap_response(uvcpp_soap_version v, char const* ns,
                          std::string const& op_local, std::string const& inner) {
  std::string body;
  body.reserve(inner.size() + op_local.size() * 2 + 64);
  body += "<" + op_local + " xmlns=\"" + ns + "\">";
  body += inner;
  body += "</" + op_local + ">";
  return soap_envelope(v, body);
}

/// 水印位置的线值 → 给人看的中文。
///
/// **这是显示层，不是配置层**：审计日志只写不读回（没有任何代码把它解析回来），
/// 所以在这里翻译不会踩到"把线值翻成中文再喂回 `validate_config`"那个坑。
/// 认不出的值原样打出去 —— 将来加了新位置，日志里会多一个没翻译的英文词，
/// 那是**看得见**的，而不是一个悄悄写错的方位。
std::string osd_position_cn(std::string const& p) {
  if (p == "upper_left") return "左上";
  if (p == "upper_right") return "右上";
  if (p == "lower_left") return "左下";
  if (p == "lower_right") return "右下";
  if (p == "custom") return "自定义";
  return p;
}

/// 一项水印长什么样，拼成一行给人看的细节。
///
/// **写的是"现在是什么"**（改完之后的那个状态），不是"哪几个字段被提到了" ——
/// NVR 送来的多半是它刚读到的那一整份，逐字段列出"它提了位置和字号"对读日志的
/// 人没有意义；"右下角、32px 的名称水印"才是。
std::string osd_item_detail(OsdItemConfig const& it) {
  std::string s = it.type == "time"   ? "时间水印"
                  : it.type == "name" ? "名称水印"
                                      : "文字水印";
  if (it.type == "text" && !it.text.empty()) s += "「" + it.text + "」";
  s += "，位置=" + osd_position_cn(it.position);
  if (it.position == "custom") {
    s += "(" + std::to_string(it.x_pct) + "%," + std::to_string(it.y_pct) + "%)";
  }
  s += "，字号=" + std::to_string(it.font_size) + "px";
  s += it.enabled ? "，已启用" : "，已关闭";
  return s;
}

/// `Set*` 这一次**送了哪几项**，拼成一行给人看的细节。
///
/// ⚠️ 这里写的是"报文里带了什么"，**不是"哪几项真的生效了"** —— 后者要拿
/// `running_cfg` 去比，而那份权威副本在 main.cpp（见 `OnvifServiceOptions::on_patch`
/// 的注释：ONVIF 这一层连 `AppConfig` 都不该认全）。硬在这里猜一个 `pending`
/// 集合，就是同一件事的第二个副本，而两份迟早不一致 —— 症状是日志说"已生效"
/// 而画面没变。
///
/// `p` 一项都没有时返回空串（调用点只在 `!p.empty()` 时用它）。
std::string patch_detail(OnvifDevicePatch const& p) {
  std::string s;
  auto add = [&s](std::string const& item) {
    if (!s.empty()) s += ", ";
    s += item;
  };
  if (p.name) add("名称=" + *p.name);
  if (p.resolution_height) add("主码流高度=" + std::to_string(*p.resolution_height));
  if (p.fps) add("帧率=" + std::to_string(*p.fps));
  if (p.gop_seconds) add("关键帧间隔=" + std::to_string(*p.gop_seconds) + "秒");
  if (p.h264_profile) add("H.264 档位=" + *p.h264_profile);
  if (p.bitrate_kbps) add("主码率=" + std::to_string(*p.bitrate_kbps) + "kbps");
  if (p.sub_height) add("子码流高度=" + std::to_string(*p.sub_height));
  if (p.sub_bitrate_kbps) add("子码率=" + std::to_string(*p.sub_bitrate_kbps) + "kbps");
  return s;
}

}  // namespace

struct OnvifService::Impl {
  OnvifServiceOptions opt;
  bool anonymous_dt_ = true;
  /// **不持有**，见 `set_device_state` 的注释。空 = 没有 ONVIF 的那条路。
  DeviceState* device = nullptr;

  /// 处理函数要回 Fault 时置这里并返回空串，由 `handle()` 发出去。
  ///
  /// **为什么不直接把 Fault 的 XML 返回去**：那条路的 HTTP 状态码是 200，而
  /// 这个服务里所有别的 Fault 都是 500。同一个端点上两种状态码，客户端会把
  /// 200 那一种当成"设备回了一个我不认识的响应"，而 500 那一种才是它预期的
  /// SOAP Fault —— 于是同一个错误会因为走哪条路而表现成两回事。
  /// 顺带：`faults` 计数器也只有 `send_fault` 那一条路会加。
  bool fault_pending = false;
  FaultInfo fault;

  /// 置一个 Fault 并返回空串，好让调用点写成 `return fail(...)`。
  std::string fail(FaultInfo f) {
    fault_pending = true;
    fault = std::move(f);
    return {};
  }
  std::atomic<std::uint64_t> requests{0};
  std::atomic<std::uint64_t> auth_failures{0};
  std::atomic<std::uint64_t> faults{0};
  /// 只在循环线程上写，所以不加锁。
  std::string last_unsupported;

  /// NVR 要求按 NTP 对时（`SetSystemDateAndTime` 的 `DateTimeType=NTP`）。
  ///
  /// **这个位由 `device_op` 置、由 `handle()` 消费**，因为动作必须在**响应发出去
  /// 之后**才能做（理由见 `OnvifServiceOptions::on_ntp_request`）：`device_op`
  /// 手上只有一个字符串、连 `resp` 都够不着，所以它只能置位。
  ///
  /// 这是这棵树里"先回响应再动手"的第四处（前两处是 `/api/reload` 的注释与
  /// `/api/restart`，第三处是 `/api/time/sync`），形状与 `fault_pending` 完全一样
  /// —— 同一个 Impl、同一把锁（这里根本不需要锁：整条 SOAP 路径都在循环线程上）。
  bool ntp_requested = false;

  /// 审计去重闸门的状态（见 `note_audit`）。**只在循环线程上写**，所以不加锁。
  std::string audit_last_key;
  std::chrono::steady_clock::time_point audit_last_at{};

  /// 写一行操作日志（审计），**并去掉"一分钟之内一字不差的那一条"**。
  ///
  /// ## 为什么要去重，而不是"多写几行无所谓"
  ///
  /// NVR 会周期性地把同一份配置原样送回来（`SetVideoEncoderConfiguration` 最常见）、
  /// 也会反复重发同一个时刻附近的时间。写日志本身很便宜，但**这份日志是给人读的**：
  /// 一条真正的改名被几百行"又要改一次码率 2048"淹掉之后，它就不再能回答用户那句
  /// "谁在什么时候改了摄像头名称"了 —— 而回答那句话正是它存在的全部理由。
  ///
  /// 闸门是"上一条**一字不差**的（来源 + 动作 + 细节）在一分钟之内"：只要 NVR 送来的
  /// 内容有一个字不同（改了名、换了码率、时间偏移变了），它就是新的一条。
  ///
  /// ## 为什么闸门在这里、而不是在 `AuditLog` 里
  ///
  /// 网页那几种动作（重启/恢复出厂/改配置）由人点出来，根本不会高频重复；把它们
  /// 一起纳入去重，只会让"连着做两次同样的操作、日志里只有一条"变成一个说不清的
  /// 现象。所以闸门只装在**会被机器反复调用**的这一侧。
  ///
  /// 身份写成 `onvif:<用户名>`，与 `web:<用户名>` 同一个惯例。**口令永远不在这里**
  /// —— 它连参数都不是。
  void note_audit(std::string const& peer, std::string const& action,
                  std::string const& detail) {
    if (!opt.audit) return;
    std::string const key = peer + "\x1f" + action + "\x1f" + detail;
    auto const now = std::chrono::steady_clock::now();
    if (key == audit_last_key &&
        now - audit_last_at < std::chrono::seconds(kAuditDedupeSeconds)) {
      return;
    }
    audit_last_key = key;
    audit_last_at = now;
    opt.audit(AuditWho{peer, "onvif:" + opt.user}, action, detail);
  }

  /// **整个进程只许建一次。** `DigestAuth` 里的 nonce 密钥是随机构造的，
  /// 每次新建都会让先前发出去的所有 nonce 立刻失效 —— 而症状极具欺骗性：
  /// 401 上的挑战完全合规、客户端按 RFC 算出的摘要也完全正确，服务端却一律
  /// 回 401，看起来像是"摘要算法写错了"。所以它跟着 `opt` 一起重建，不按
  /// 请求建。
  std::unique_ptr<DigestAuth> auth;

  DigestAuth& authenticator() {
    if (!auth) auth = std::make_unique<DigestAuth>(opt.realm, opt.user, opt.password);
    return *auth;
  }

  /// 一路都没配时也得能回答 —— `profiles.front()` 在空 vector 上是崩，
  /// 而崩在一条来自网络的 SOAP 请求里是最糟的失败方式。
  std::vector<OnvifStreamProfile> const& profiles() const {
    static std::vector<OnvifStreamProfile> const kFallback(1);
    return opt.profiles.empty() ? kFallback : opt.profiles;
  }

  std::string video_source_config(std::string const& ind,
                                  OnvifStreamProfile const& p) const {
    std::string x;
    x += ind + "<tt:Name>VideoSourceConfig</tt:Name>";
    x += ind + "<tt:UseCount>1</tt:UseCount>";
    x += ind + "<tt:SourceToken>" + kTokenVideoSource + "</tt:SourceToken>";
    x += ind + "<tt:Bounds x=\"0\" y=\"0\" width=\"" + std::to_string(p.width) +
         "\" height=\"" + std::to_string(p.height) + "\"/>";
    return x;
  }

  std::string encoder_config(std::string const& ind,
                             OnvifStreamProfile const& p) const {
    // GovLength 是**帧数**不是秒数（配置里存的是秒）。
    int const gov = clamp_int(p.gop_seconds * p.fps, 1, 100000);
    std::string x;
    x += ind + "<tt:Name>" + xml_escape(p.name) + "</tt:Name>";
    x += ind + "<tt:UseCount>1</tt:UseCount>";
    x += ind + "<tt:Encoding>" + xml_escape(p.encoding) + "</tt:Encoding>";
    x += ind + "<tt:Resolution><tt:Width>" + std::to_string(p.width) +
         "</tt:Width><tt:Height>" + std::to_string(p.height) +
         "</tt:Height></tt:Resolution>";
    x += ind + "<tt:Quality>" + std::to_string(p.quality) + "</tt:Quality>";
    x += ind + "<tt:RateControl>";
    x += "<tt:FrameRateLimit>" + std::to_string(p.fps) + "</tt:FrameRateLimit>";
    x += "<tt:EncodingInterval>1</tt:EncodingInterval>";
    x += "<tt:BitrateLimit>" + std::to_string(p.bitrate_kbps) +
         "</tt:BitrateLimit>";
    x += "</tt:RateControl>";
    // H264 块里 GovLength 和 H264Profile 都是**必填**的。缺 GovLength 时
    // 大华那边会把 I 帧间隔当成 0 并判定这个 profile 非法。
    x += ind + "<tt:H264>";
    x += "<tt:GovLength>" + std::to_string(gov) + "</tt:GovLength>";
    x += "<tt:H264Profile>" + xml_escape(p.h264_profile) + "</tt:H264Profile>";
    x += "</tt:H264>";
    // **不给 Multicast。** 给了之后有的 NVR 会优先去连组播地址，而我们没有
    // 实现组播推流，结果是"添加成功但一直没画面"。
    x += ind + "<tt:SessionTimeout>PT60S</tt:SessionTimeout>";
    return x;
  }

  /// 一个完整的 Profile。NVR 的通道列表就是拿它渲染的。
  std::string profile_block(std::string const& ind,
                            OnvifStreamProfile const& p) const {
    std::string x;
    x += ind + "<trt:Profiles token=\"" + attr(p.token) + "\" fixed=\"true\">";
    x += "<tt:Name>" + xml_escape(p.name) + "</tt:Name>";
    x += "<tt:VideoSourceConfiguration token=\"" +
         std::string(kTokenVsConfig) + "\">";
    x += video_source_config("", p);
    x += "</tt:VideoSourceConfiguration>";
    x += "<tt:VideoEncoderConfiguration token=\"VideoEncoderConfig_" +
         attr(p.token) + "\">";
    x += encoder_config("", p);
    x += "</tt:VideoEncoderConfiguration>";
    x += "</trt:Profiles>";
    return x;
  }

  OnvifStreamProfile const* find_profile(std::string const& token) const {
    if (token.empty()) return nullptr;
    for (OnvifStreamProfile const& p : profiles()) {
      if (p.token == token) return &p;
    }
    return nullptr;
  }

  /// `SetVideoEncoderConfiguration` / `SetVideoSourceConfiguration` 里的那个
  /// token 指向哪一路。
  ///
  /// 我们发出去的编码器 token 是 `VideoEncoderConfig_<profile token>` 这个形状
  /// （见 `profile_block`），而客户端**照抄回来的就可能带前缀也可能不带** ——
  /// 大华会把整个串原样送回来，有些客户端只送后半截。三种写法都认。
  ///
  /// **认不出来时返回 nullptr**，由调用点决定怎么办。这里只负责"认出"这件事，
  /// 不替调用方做"那就当主码流吧"这种决定 —— 藏在解析函数里的兜底是查不出来的，
  /// 而它一旦生效，症状是"NVR 改了子码流、结果是主码流变了"。
  OnvifStreamProfile const* resolve_config_target(std::string const& token) const {
    static constexpr char kPrefix[] = "VideoEncoderConfig_";
    std::string t = token;
    if (t.compare(0, sizeof(kPrefix) - 1, kPrefix) == 0) {
      t = t.substr(sizeof(kPrefix) - 1);
    }
    return find_profile(t);
  }

  /// `tt:H264Profile` 的取值（首字母大写）→ `capture.profile` 的线值（小写）。
  ///
  /// 认不出来返回**空串**（而不是硬套一个 `main`）：我们不认识的档位被悄悄换成
  /// `main` 会让 NVR 那边显示"设成了 High"而编码器出的是 Main —— 报高了有 NVR
  /// 会据此选硬件解码通路，然后解不出来。
  static std::string h264_profile_from_onvif(std::string const& s) {
    if (s == "Baseline" || s == "baseline") return "baseline";
    if (s == "Main" || s == "main") return "main";
    // `Extended` 也落在 high 上：我们没有第四个档位，而报一个不存在的档位
    // 等于谎报。high 在兼容性上比 extended 保守。
    if (s == "High" || s == "high" || s == "Extended" || s == "extended") {
      return "high";
    }
    return {};
  }

  // --- OSD（水印）的 ONVIF 表示 -------------------------------------------
  //
  // 元素名、元素顺序、枚举字面量全部照 `onvif_wsdl/onvif.xsd` 里
  // `tt:OSDConfiguration` / `tt:OSDTextConfiguration` / `tt:OSDConfigurationOptions`
  // 那几段抄，操作形状照 `onvif_wsdl/media.wsdl` 的 "OSD Schema" 那一段。
  //
  // ⚠️ **这一段不能凭印象写。** 名字写错的后果是 NVR 那边**静默**不认
  // （OSD 页面空白，或者干巴巴一句"设备不支持"），而本地只要验"XML 能解析"
  // 就全绿 —— 所以 `tests/onvif_soap.py` 的 [11] 段是**逐字断言元素名与枚举
  // 字面量**，而不是只验严格解析过得去。

  /// 一项水印的 token：`OSD_<下标>`。
  ///
  /// ## 为什么是位置派生的
  ///
  /// 配置里的一项水印（`OsdItemConfig`）没有 id —— 它就是列表里的第几项。
  /// 硬加一个 id 字段要同时改配置文件、往返序列化、前端三处，而 NVR 拿到的
  /// token 只在"一份 `GetOSDs` 到下一次改动"之间有意义（任何一次改动之后它
  /// 都会重读）。
  ///
  /// ## 代价，写在这儿而不是留在文档里
  ///
  /// `DeleteOSD OSD_0` 之后，原来的 `OSD_1` **改叫** `OSD_0`（它确实成了列表里
  /// 的第 0 项）。一个把 token 缓存下来、删除后不复读的 NVR 会拿着 `OSD_1` 来调
  /// `SetOSD`，而那时它越界 —— 我们回 Fault，它看到的是"操作失败"。
  /// **这是可见的失败，不是静默改错**，而这条底线正是位置派生能成立的前提：
  /// 换成"认不出就夹到最后一项"的话，用户点一次删除会顺手改掉另一项水印。
  static std::string osd_token(std::size_t i) {
    return "OSD_" + std::to_string(i);
  }

  /// 反解 token。**认不出和越界都算失败**（见上面那段）。
  static bool osd_index_from_token(std::string const& t, std::size_t count,
                                   std::size_t& out) {
    static constexpr char kPrefix[] = "OSD_";
    if (t.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0) return false;
    int v = 0;
    if (!str_int_in_range(t.substr(sizeof(kPrefix) - 1), 0, 1000000, v)) {
      return false;
    }
    if (static_cast<std::size_t>(v) >= count) return false;
    out = static_cast<std::size_t>(v);
    return true;
  }

  /// 报文里的 OSD token。**两种形状都要认**，因为 schema 里就是两种：
  ///
  ///   * `GetOSD` / `DeleteOSD` —— `<trt:OSDToken>OSD_0</trt:OSDToken>` 子元素
  ///   * `SetOSD` —— `<trt:OSD token="OSD_0">` **属性**（`token` 是从
  ///     `tt:DeviceEntity` 继承来的必填属性，不在子元素里）
  ///
  /// 只认一种的后果是"删除能用、保存不能用"（或者反过来），而两条路上的
  /// 错都长成同一个 Fault。
  static std::string osd_token_from_body(std::string const& body_xml) {
    std::string const t = xml_text(body_xml, "OSDToken");
    if (!t.empty()) return t;
    return xml_attr(body_xml, "token");
  }

  /// 认不出的 OSD token 一律走这一条 Fault。
  ///
  /// 子码用 `InvalidArgVal` + `NoConfig`（ONVIF 错误命名空间里"参数值非法 /
  /// 没有这个配置"这一对），而不是笼统的 `ActionNotSupported`：客户端能靠它
  /// 区分"这台设备不支持 OSD"和"你要的那个 OSD 不存在"，而后者是重新
  /// `GetOSDs` 一次就能恢复的。
  std::string osd_token_fault(std::string const& body_xml) {
    // WARN 在这里是必须的：`DeleteOSD OSD_0` 之后原来的 `OSD_1` 改叫 `OSD_0`
    // （见 `osd_token` 那段），所以缓存了 token 的 NVR 迟早会送来一个越界的，
    // 而它看到的现象只是"操作失败"。这行日志是唯一能说清是为什么的地方。
    D2O_WARN("NVR 送来的 OSD token 我们不认得（自己发出去的是 OSD_<下标>）："
             "\"%s\"。多半是它缓存了删除/重建之前的那一份。",
             osd_token_from_body(body_xml).c_str());
    return fail({FaultKind::kBadRequest, "InvalidArgVal", "NoConfig",
                 "unknown OSD token"});
  }

  /// 配置里的位置 → `tt:OSDPosConfiguration/Type` 的字面量。
  /// 认不出的配置值退回 `UpperLeft`（配置层已经夹过一次，走到这里说明配置被
  /// 人手改过；报一个 schema 里没有的字符串会让整个 OSD 页面解析失败）。
  static char const* onvif_pos_from_cfg(std::string const& p) {
    if (p == "upper_right") return "UpperRight";
    if (p == "lower_left") return "LowerLeft";
    if (p == "lower_right") return "LowerRight";
    if (p == "custom") return "Custom";
    return "UpperLeft";
  }

  /// 反过来。**认不出返回空串**（= 不改），而不是退回左上 —— 退回的话，NVR
  /// 送一个我们没实现的位置（比如 `Center`）会把用户设好的位置**悄悄挪走**，
  /// 而它报回来的仍然是"成功"。
  static std::string cfg_pos_from_onvif(std::string const& p) {
    if (p == "UpperRight") return "upper_right";
    if (p == "LowerLeft") return "lower_left";
    if (p == "LowerRight") return "lower_right";
    if (p == "Custom") return "custom";
    if (p == "UpperLeft") return "upper_left";
    return {};
  }

  /// 百分比（0..100）→ `tt:Pos` 的**归一化**坐标（0..1）。固定六位小数，
  /// 这样同一个值在两次响应里长得一模一样（`%g` 会打成 `0.5` / `0.333333`）。
  static std::string unit_from_pct(int pct) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f",
                  static_cast<double>(clamp_int(pct, 0, 100)) / 100.0);
    return buf;
  }

  static int pct_from_unit(double v) {
    int const pct = static_cast<int>(v * 100.0 + 0.5);
    return clamp_int(pct, 0, 100);
  }

  /// 配置里的字号是**主码流下的像素高度**，而 ONVIF 的 `tt:FontSize` 单位是
  /// **pt**（schema 原文 "Font size of the text in pt."）。差一个 96/72 ——
  /// 不换算的话 NVR 上显示的字号和画面上的实际大小差三分之一，而"有一个看起来
  /// 合理的数字"让这件事很难被发现。
  static int pt_from_px(int px) { return (px * 3 + 2) / 4; }  // px * 72 / 96
  static int px_from_pt(int pt) { return (pt * 4 + 1) / 3; }  // pt * 96 / 72

  /// 这一项在主码流上实际画的字号（像素）。`0` = 自动，而自动的规则在
  /// `OsdOverlay` 里（帧高 / 32、下限 16）—— 这里必须和它**算出同一个数**，
  /// 否则 NVR 读到的字号和画面上的字不是一个大小，而两处都"看着有值"。
  int effective_font_px(OsdItemConfig const& it) const {
    int const main_h = profiles().front().height;
    int const px = it.font_size > 0 ? it.font_size : main_h / 32;
    return px < 16 ? 16 : px;
  }

  /// strftime 的格式串 → ONVIF 的 `DateFormat` / `TimeFormat` 记号。
  ///
  /// ## 为什么必须换算，不能把 `%Y-%m-%d` 直接报出去
  ///
  /// ONVIF 那一套**不是** strftime，是 .NET 风格的记号（`yyyy-MM-dd`）。报错
  /// 格式的后果是 NVR 的 OSD 预览里显示的字面量就是 `%Y-%m-%d` —— 而它对视频
  /// 本身没有任何影响，所以本地怎么测都测不出来。
  ///
  /// ## `MM` 是月、`mm` 是分（大小写敏感）
  ///
  /// 弄反的后果是"07:10:05 变成 10:07:05"这一类只在月和分的数字不同时才看得出来
  /// 的错位。认不出的记号（`%A`、`%j` 这些）**原样保留**：宁可让 NVR 看到一串
  /// 它认不出但确实是我们格式串的东西，也不要在这里悄悄替用户改成别的格式。
  static std::string onvif_format_from_strftime(std::string const& f) {
    std::string out;
    for (std::size_t i = 0; i < f.size(); ++i) {
      if (f[i] != '%' || i + 1 >= f.size()) {
        out += f[i];
        continue;
      }
      switch (f[++i]) {
        case 'Y': out += "yyyy"; break;
        case 'y': out += "yy"; break;
        case 'm': out += "MM"; break;
        case 'd': out += "dd"; break;
        case 'H': out += "HH"; break;
        case 'M': out += "mm"; break;
        case 'S': out += "ss"; break;
        case '%': out += "%"; break;
        default:
          out += '%';
          out += f[i];
          break;
      }
    }
    return out;
  }

  /// 反过来。**任何一个认不出的字母都让整条作废**（返回空串）。
  ///
  /// 两个方向不能对称着做：往外报的时候原样保留是安全的（NVR 只是显示），
  /// 而**收进来**的字符串是要交给 `strftime` 去跑的。放一个我们不认识的字母
  /// 进去，`format_time` 会把它原样打出来 —— 水印上就多出几个莫名其妙的字符，
  /// 而配置里看上去是一串正经的格式。分隔符（`-` `/` `:` 空格）照抄，
  /// 因为 `yyyy/MM/dd` 这种写法是 NVR 的正当要求。
  static std::string strftime_from_onvif_format(std::string const& f) {
    struct Map {
      char const* onvif;
      char const* strf;
    };
    static constexpr Map kMap[] = {
        {"yyyy", "%Y"}, {"yy", "%y"}, {"MM", "%m"}, {"dd", "%d"},
        {"HH", "%H"},   {"mm", "%M"}, {"ss", "%S"},
    };
    std::string out;
    std::size_t i = 0;
    while (i < f.size()) {
      bool hit = false;
      for (Map const& m : kMap) {
        std::size_t const n = std::strlen(m.onvif);
        if (f.compare(i, n, m.onvif) == 0) {
          out += m.strf;
          i += n;
          hit = true;
          break;
        }
      }
      if (hit) continue;
      char const c = f[i];
      bool const alpha = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
      // `%` 也拒掉：抄进格式串会让 `strftime` 把后面一个字符当成转换符。
      if (alpha || c == '%') return {};
      out += c;
      ++i;
    }
    return out;
  }

  /// 一项水印 → `tt:OSDConfiguration`。
  ///
  /// `wrapper` 是外面那层元素的**本地名**，两个取值：`GetOSDsResponse` 里的
  /// 列表项叫 `OSDs`，而 `GetOSD` / `SetOSD` / `CreateOSD` 里的单个都叫 `OSD`
  /// —— 这是 schema 定的（见 media.wsdl 的 "OSD Schema" 段），统一成一个的话
  /// 那一个一定错。类型是 `tt:OSDConfiguration`，所以**属性在 trt 上、子元素
  /// 在 tt 上**。
  std::string osd_block(std::string const& wrapper, std::string const& token,
                        OsdItemConfig const& it) const {
    std::string x = "<trt:" + wrapper + " token=\"" + attr(token) + "\">";
    // 元素顺序是 schema 的 `xs:sequence` 定的，不能换：Type 在
    // VideoSourceConfigurationToken 后面，Position 在 Type 后面。
    x += "<tt:VideoSourceConfigurationToken>" + std::string(kTokenVsConfig) +
         "</tt:VideoSourceConfigurationToken>";
    x += "<tt:Type>Text</tt:Type>";

    x += "<tt:Position>";
    x += std::string("<tt:Type>") + onvif_pos_from_cfg(it.position) +
         "</tt:Type>";
    // **只在 Custom 时给 `tt:Pos`。** 四个预定义位置由 `Type` 完全确定，再补
    // 一组数字只会让某些客户端改用数字（而我们那组数字是从同一处推出来的，
    // 没有任何信息量）。`tt:Vector` 的取值是归一化的 0..1，配置里存的是百分比。
    if (it.position == "custom") {
      x += "<tt:Pos x=\"" + unit_from_pct(it.x_pct) + "\" y=\"" +
           unit_from_pct(it.y_pct) + "\"/>";
    }
    x += "</tt:Position>";

    // ## 三种水印映射到 `tt:OSDTextConfiguration`
    //
    //   `time` → `DateAndTime`（或 `Date` / `Time`），走自动计时的那一套
    //   `name` → `Plain` + `PlainText`，内容是**当前**设备名（每帧现读的
    //            `DeviceState`，所以 NVR 改完名这里立刻就是新名字）
    //   `text` → `Plain` + `PlainText`
    x += "<tt:TextString>";
    if (it.type == "time") {
      // `Date` / `Time` / `DateAndTime` 由**哪几个格式串非空**推出来，而不是
      // 另存一个字段：配置里"空串 = 不显示"本来就是这个语义
      // （见 osd_overlay.cpp 里 `if (d.empty()) return t;`）。另存一个的话，
      // 两个字段说的同一件事迟早会不一致。
      char const* kind = "DateAndTime";
      if (it.date_format.empty() && !it.time_format.empty()) {
        kind = "Time";
      } else if (!it.date_format.empty() && it.time_format.empty()) {
        kind = "Date";
      }
      x += std::string("<tt:Type>") + kind + "</tt:Type>";
      // 规范要求 `DateAndTime` 这两项都在（"shall be present"）。空的那个也
      // 照发 —— 省略掉的话 NVR 会拿它自己的默认格式去显示，而那和画面上的
      // 字不是一个东西。
      x += "<tt:DateFormat>" + xml_escape(onvif_format_from_strftime(it.date_format)) +
           "</tt:DateFormat>";
      x += "<tt:TimeFormat>" + xml_escape(onvif_format_from_strftime(it.time_format)) +
           "</tt:TimeFormat>";
    } else {
      std::string plain = it.text;
      if (it.type == "name" && device != nullptr) {
        plain = device->channel_name();
      }
      x += "<tt:Type>Plain</tt:Type>";
      x += "<tt:PlainText>" + xml_escape(plain) + "</tt:PlainText>";
    }
    x += "<tt:FontSize>" + std::to_string(pt_from_px(effective_font_px(it))) +
         "</tt:FontSize>";
    x += "</tt:TextString>";

    x += "</trt:" + wrapper + ">";
    return x;
  }

  /// 从 `SetOSD` / `CreateOSD` 的报文里读出一项水印，**就地改在 `base` 上**。
  ///
  /// `base` 是"这一项原来是什么"（`CreateOSD` 时是一个新项）：报文里没提到的
  /// 字段**保留原值**，不填默认值 —— 和 `patch_from_body` 同一个道理，NVR 送
  /// 回来的多半就是它自己刚刚读到的那一份，只送一半也是合法的。
  ///
  /// 返回 false 表示这一项我们**表示不了**（比如 `Type=Image`），调用方应当回
  /// Fault：把一张图片水印当文本处理的后果是用户明明选的是图片、画面上出来一行
  /// 空字，而两边都说"设置成功"。
  bool osd_item_from_body(std::string const& body, OsdItemConfig& base) const {
    // 外面那个 `Type`（Text / Image / Extended）。**必须按块取**：下面
    // `TextString` 里还有**另一个** `Type`，扁平地找第一个虽然也能拿到外面
    // 这个，但读里面那个时就必然拿错（见 `xml_block` 的注释）。
    std::string const kind = xml_text(body, "Type");
    if (!kind.empty() && kind != "Text") {
      D2O_WARN("NVR 要建一个 %s 类型的水印，我们只会画文字（Text）。",
               kind.c_str());
      return false;
    }

    std::string const pos_block = xml_block(body, "Position");
    std::string const cfg_pos = cfg_pos_from_onvif(xml_text(pos_block, "Type"));
    if (!cfg_pos.empty()) base.position = cfg_pos;
    if (base.position == "custom") {
      double x = 0.0;
      double y = 0.0;
      std::string const tag = xml_tag(pos_block, "Pos");
      if (xml_attr_float(tag, "x", 0.0, 1.0, x)) base.x_pct = pct_from_unit(x);
      if (xml_attr_float(tag, "y", 0.0, 1.0, y)) base.y_pct = pct_from_unit(y);
    }

    std::string const text_block = xml_block(body, "TextString");
    std::string const ttype = xml_text(text_block, "Type");

    if (ttype == "Plain") {
      std::string const pt = xml_text(text_block, "PlainText");
      // ## 回声判别
      //
      // 名称项报出去的 `PlainText` 就是**当前设备名**，而 NVR 把整份 OSD 读
      // 一遍再原样写回来是常态。分不开的话症状是"NVR 上点一下保存，画面上的
      // 名称水印就冻在了当时那一刻的名字上，从此不跟着改名走"—— 而水印还在，
      // 看上去一切正常，只是内容再也不变。
      //
      // 和 `patch_from_body` 里 `Name` 那一段是同一个形状、同一个理由。
      // **空串也算回声**：它不携带任何信息，而把它当成"改成空文字"会把一个
      // 活的水印变成一个看不见的水印项 —— 用户要去掉它，手段是 `DeleteOSD`。
      bool const echo_of_name =
          (base.type == "name") &&
          (device == nullptr || pt.empty() || pt == device->channel_name());
      if (!echo_of_name) {
        base.type = "text";
        base.text = pt;
      }
    } else if (ttype == "Date" || ttype == "Time" || ttype == "DateAndTime") {
      base.type = "time";
      // 三个取值**决定了哪几个格式串有意义**：`Date` 只显示日期、`Time` 只显示
      // 时间，而配置里"空串 = 不显示"就是它的表示法。只改其中一半而不清空另
      // 一半的症状是"NVR 上把 OSD 改成只显示日期，画面上还是日期加时间"。
      bool const want_date = (ttype != "Time");
      bool const want_time = (ttype != "Date");
      if (want_date) {
        std::string const s =
            strftime_from_onvif_format(xml_text(text_block, "DateFormat"));
        if (!s.empty()) base.date_format = s;
      } else {
        base.date_format.clear();
      }
      if (want_time) {
        std::string const s =
            strftime_from_onvif_format(xml_text(text_block, "TimeFormat"));
        if (!s.empty()) base.time_format = s;
      } else {
        base.time_format.clear();
      }
    }
    // `ttype` 为空（报文里没有 TextString）= 这一项的种类没被提到，保持原样。

    int fs = 0;
    if (xml_int(text_block, "FontSize", 1, 10000, fs)) {
      base.font_size = px_from_pt(fs);
    }
    return true;
  }

  /// 改完之后推给调用方落盘，**并更新自己那份权威副本**。
  ///
  /// 顺序是"先更新内存、再回调"：回调失败时 SOAP 那边也不回 Fault
  /// （理由见 `OnvifServiceOptions::on_patch` 的注释），而内存里那份必须已经
  /// 是新的 —— 否则 NVR 会看到"我改了、它也说成功、回读还是老的"。
  /// 落盘失败的唯一痕迹是实现里那行 ERROR 日志。
  void apply_osd(OsdConfig const& c) {
    opt.osd = c;
    if (opt.on_osd_patch) opt.on_osd_patch(c);
  }

  /// 从 `SetVideoEncoderConfiguration` / `SetVideoSourceConfiguration` 的报文里
  /// 抠出我们能改的那几项。抠不到的**不设值**（= 这一次没提它），而不是填默认值。
  ///
  /// `tgt` 是这次报文指向的那一路，`allow_rename` 见下面的 `Name` 那一段。
  OnvifDevicePatch patch_from_body(std::string const& body,
                                   OnvifStreamProfile const& tgt,
                                   bool sub_target, bool allow_rename) const {
    OnvifDevicePatch p;

    // ## 什么时候 `Name` 算"改名"，什么时候算"回声"
    //
    // 改名走这条路（另外两个入口是启动和网页）。但**不能见到 `Name` 就认**：
    // NVR 把读到的配置整份写回来是常态 —— 用户在它自己的编码参数页上改个
    // 码率，整份 `Configuration` 都会被送回来，而那个 `Name` 是**我们自己**
    // 在 `GetProfiles` 里报出去的（每一路是 "main"/"sub"，源配置是
    // "VideoSourceConfig"）。分不开的话，症状是"在 NVR 上改码率，画面上的
    // 名称变成了 main"—— 没有人会把这两件事联系起来。
    //
    // `SetVideoSourceConfiguration` **一律不改名**：它的 `Name` 是源配置自己
    // 的标签，不是设备名。ONVIF 里 NVR 改通道名走的是编码器配置那一份。
    if (allow_rename) {
      std::string const name = xml_text(body, "Name");
      if (!name.empty() && name != tgt.name) p.name = name;
    }

    // 分辨率。两个元素的形状不一样：
    //   * `SetVideoEncoderConfiguration` 用子元素 `<tt:Resolution><tt:Height>`
    //   * `SetVideoSourceConfiguration` 用**属性** `<tt:Bounds ... height="1080"/>`
    // 只认高度，理由见 `OnvifDevicePatch` 的注释。
    int h = 0;
    bool has_h = xml_int(body, "Height", 1, 100000, h);
    if (!has_h) {
      // 属性那条路。**先切出 Bounds 那个标签再找属性**，不是在整份报文上找
      // `height="` —— 那样会命中任何一个同名属性，而取错的值在这里看起来
      // 只是一个**像是正确的数字**（比如取到另一路的分辨率）。
      has_h = xml_attr_int(xml_tag(body, "Bounds"), "height", 1, 100000, h);
    }
    if (has_h) {
      if (sub_target) {
        p.sub_height = h;
      } else {
        p.resolution_height = h;
      }
    }

    // 帧率、码率、GovLength 只在**编码器**配置里有（源配置没有 RateControl）。
    // 子码流那一侧配置层只有码率，没有独立的帧率/关键帧间隔字段，所以主码流
    // 独有这三项。改了子码流却把帧率写到主码流上会是个很难查的错。
    if (!sub_target) {
      int v = 0;
      if (xml_int(body, "FrameRateLimit", 1, 240, v)) p.fps = v;
      if (xml_int(body, "BitrateLimit", 1, 1000000, v)) p.bitrate_kbps = v;
      // **GovLength 是帧数，配置里存的是秒。** 这个换算在前面 `encoder_config()`
      // 里是反着做的（`gop_seconds * fps`），而两边差一个 fps —— 不换算的话
      // NVR 设"每 50 帧一个 I 帧"会变成"每 50 秒一个"，GOP 长了 25 倍，
      // 浏览器预览（只在关键帧处分片）会跟着卡死。
      int gov = 0;
      if (xml_int(body, "GovLength", 1, 100000, gov)) {
        int const fps = p.fps ? *p.fps : profiles().front().fps;
        p.gop_seconds = clamp_int(gov / (fps > 0 ? fps : 1), 1, 600);
      }
      std::string const prof = h264_profile_from_onvif(xml_text(body, "H264Profile"));
      if (!prof.empty()) p.h264_profile = prof;
    } else {
      int v = 0;
      if (xml_int(body, "BitrateLimit", 1, 1000000, v)) p.sub_bitrate_kbps = v;
    }

    return p;
  }

  /// 从报文里抠出 `ProfileToken` 的值。
  ///
  /// **不做完整解析**：这个值是我们自己发出去的 token，只用来查表，认不出就
  /// 当空（→ Fault），没有安全后果。为了它再走一遍 pugixml 不值得。
  /// 带不带命名空间前缀都能命中 —— 客户端加不加都合法。
  static std::string profile_token_from_body(std::string const& body_xml) {
    static char const kNeedle[] = "ProfileToken>";
    std::size_t const at = body_xml.find(kNeedle);
    if (at == std::string::npos) return {};
    std::size_t const from = at + sizeof(kNeedle) - 1;
    std::size_t const to = body_xml.find('<', from);
    if (to == std::string::npos) return {};
    return body_xml.substr(from, to - from);
  }

  std::string host_for(std::string const& peer) const {
    std::string host = pick_local_address_for(peer);
    if (host.empty()) host = default_local_address();
    if (host.empty()) host = "127.0.0.1";
    return host;
  }

  std::string url_for(std::string const& peer, char const* scheme, int port,
                      std::string const& path) const {
    return std::string(scheme) + "://" + host_for(peer) + ":" +
           std::to_string(port) +
           (path.empty() || path[0] != '/' ? "/" + path : path);
  }

  std::string stream_uri(std::string const& peer,
                         OnvifStreamProfile const& p) const {
    return url_for(peer, "rtsp", opt.rtsp_port, p.rtsp_path);
  }

  std::string snapshot_uri(std::string const& peer) const {
    return url_for(peer, "http", opt.http_port, opt.snapshot_path);
  }

  std::string device_op(uvcpp_soap_version v, std::string const& op,
                        std::string const& body_xml, std::string const& peer);
  std::string media_op(uvcpp_soap_version v, std::string const& op,
                       std::string const& body_xml, std::string const& peer);
};

std::string OnvifService::Impl::device_op(uvcpp_soap_version v,
                                          std::string const& op,
                                          std::string const& body_xml,
                                          std::string const& peer) {
  OnvifServiceOptions const& o = opt;

  if (op == "GetSystemDateAndTime") {
    // **报设备时间，不是主机时间。** 接了 `DeviceState` 之后它是一个
    // "主机时间 + NVR 下发的偏移"；没接（单跑录制那条路）就退回主机时间。
    std::chrono::system_clock::time_point const tp =
        device ? device->now() : std::chrono::system_clock::now();
    std::time_t const now =
        std::chrono::system_clock::to_time_t(tp);
    std::tm g{};
    gmtime_utc(now, g);
    std::string inner;
    // 两个都给：规范只要求给一个，但只给 UTC 时有些 NVR 会按本地时间直读，
    // 于是界面上的时间差 8 小时。
    inner += "<tds:UTCDateTime>" + date_time_fields(g) + "</tds:UTCDateTime>";
    if (o.prefer_local_time) {
      std::tm l{};
      localtime_local(now, l);
      inner += "<tds:LocalDateTime>" + date_time_fields(l) +
               "</tds:LocalDateTime>";
      inner += "<tds:TimeZone><tt:TZ>" + iso_tz_duration(utc_offset_minutes()) +
               "</tt:TZ></tds:TimeZone>";
    }
    // `DateTimeType` 是**模式驱动的**：`time.mode == "ntp"` 报 `NTP`，否则
    // `Manual`。问的是"这台设备的钟由谁维护"，而唯一能回答它的地方是配置。
    //
    // ⚠️ **这条判据从前是反的，而且反得很有道理。** 原文写的是
    // `synced = device->time_synced()`，`synced ? "Manual" : "NTP"` —— 当时的
    // 论据是"有人能设它了（`SetSystemDateAndTime` 已实现），被设过就报 Manual；
    // 没被设过就报 NTP，因为那时钟确实是主机在维护"。在那时这是**对的**：
    // 设备上没有任何东西会自动对时，"没被设过"和"跟着 NTP 走"是同一件事。
    //
    // 本轮接上真的 NTP 客户端之后，这个等式断了：一次**成功的** NTP 对时也会
    // 把 `DeviceState` 的 `synced_` 置真（它记的是"这个钟被改过"，不是"谁在
    // 维护它"），于是 NTP 模式下这里恰好报 `Manual` —— 一个说反了的事实陈述，
    // 而且它看上去完全合理（"被同步过所以是 Manual"读起来毫无破绽）。
    //
    // 回调没接（单跑录制那条路）时按 `manual` 走：那正是配置的默认值，而那条
    // 路上确实没有任何东西在自动对时。
    std::string const mode = o.time_mode ? o.time_mode() : std::string("manual");
    inner += std::string("<tds:DateTimeType>") + (mode == "ntp" ? "NTP" : "Manual") +
             "</tds:DateTimeType>";
    inner += "<tds:DaylightSavings>false</tds:DaylightSavings>";
    return soap_response(v, kNsDevice, "GetSystemDateAndTimeResponse", inner);
  }

  if (op == "SetSystemDateAndTime") {
    if (!device) {
      // 没有设备状态可写。回 Fault 比回"成功"诚实 —— 后者会让 NVR 认定
      // 时间设好了，然后在接下来的录像里带着一个错的时标。
      return fail({FaultKind::kNotSupported, "ActionNotSupported", {},
                   "SetSystemDateAndTime is not available"});
    }

    // 规范里 `DateTimeType` 可以是 Manual / NTP / ManualNTP。
    //
    // 从前这条分支是"打一行日志然后什么都不做"—— 那时设备上确实没有 NTP
    // 客户端，所以"不支持"是句实话。现在有了，于是它**有实事可做**：置一个位，
    // 等响应发出去之后真的去对一次时（见 `handle()` 的尾巴和
    // `OnvifServiceOptions::on_ntp_request`）。
    //
    // 两条纪律都没变：
    //
    //   * **照样回成功，绝不回 Fault。** 海康的添加向导会先发一次 NTP 的，
    //     回 Fault 会让整个添加流程停在时间那一步。
    //   * **动作不能在这里做。** 真对时要发一次可能阻塞两秒的 UDP 查询（地址
    //     还不止一个），而这条线程同时管着 WS 推送和别的请求。这就是"先回响应
    //     再动手"那条纪律的适用条件（同 `/api/reload` / `/api/restart` /
    //     `/api/time/sync`）。
    //
    // 当前是**手动模式**时这里也置位 —— 由 `on_ntp_request` 的实现去决定说什么
    // （它才知道该讲"把模式改成 NTP"这句话，ONVIF 这一层不该知道时间是怎么维护的）。
    std::string const type = xml_text(body_xml, "DateTimeType");
    if (type == "NTP") {
      ntp_requested = true;
      D2O_INFO("NVR 要求按 NTP 对时（SetSystemDateAndTime DateTimeType=NTP）："
               "响应先回，随后按当前的时间模式决定是否发起一次对时。");
      // 审计：**这一次请求本身不改变任何状态**（改时间的是随后那次对时），但
      // "这台 NVR 在什么时候要求过对时"仍然要能回答 —— 而随后那行 `ntp.sync`
      // 是 main.cpp 写的，它不知道要求是谁提的。所以两行各说一半，缺一不可。
      note_audit(peer, "onvif.SetSystemDateAndTime",
                 "要求按 NTP 对时（DateTimeType=NTP）；对时结果见随后的 ntp.sync 一行");
      return soap_response(v, kNsDevice, "SetSystemDateAndTimeResponse", "");
    }

    // 优先 `UTCDateTime`，没有才用 `LocalDateTime` + `TimeZone`。
    //
    // 两个都按块取（见 `xml_block`）：报文里很可能两块都给，而两块里各有
    // 一个 `Hour`/`Minute`/…，扁平地找第一个数字会读到一个自己都说不清是
    // 哪一块的值。
    std::tm fields{};
    bool ok = false;
    std::string const utc_block = xml_block(body_xml, "UTCDateTime");
    std::string const local_block = xml_block(body_xml, "LocalDateTime");
    std::string const src = !utc_block.empty() ? utc_block : local_block;
    if (!src.empty()) {
      int year = 0, mon = 0, day = 0, hour = 0, min = 0, sec = 0;
      ok = xml_int(src, "Year", 1970, 2200, year) &&
           xml_int(src, "Month", 1, 12, mon) &&
           xml_int(src, "Day", 1, 31, day) &&
           xml_int(src, "Hour", 0, 23, hour) &&
           xml_int(src, "Minute", 0, 59, min) &&
           xml_int(src, "Second", 0, 60, sec);
      if (ok) {
        fields.tm_year = year - 1900;
        fields.tm_mon = mon - 1;
        fields.tm_mday = day;
        fields.tm_hour = hour;
        fields.tm_min = min;
        fields.tm_sec = (sec == 60) ? 59 : sec;   // 闰秒：夹到 59 而不是溢出
      }
    }
    if (!ok) {
      return fail({FaultKind::kBadRequest, {}, {},
                   "SetSystemDateAndTime: no usable "
                   "UTCDateTime or LocalDateTime"});
    }

    std::time_t utc = utc_fields_to_time_t(fields);
    if (utc == static_cast<std::time_t>(-1)) {
      return fail({FaultKind::kBadRequest, {}, {},
                   "SetSystemDateAndTime: date out of range"});
    }
    if (utc_block.empty()) {
      // 本地时间那一份要减去时区偏移才是 UTC。没给 `TZ` 就当 UTC ——
      // 这是规范的默认，而且比"用本机时区"好：本机时区是**我们**的，
      // 不是 NVR 的。
      long tz_min = 0;
      std::string const tz = xml_text(body_xml, "TZ");
      if (!tz.empty() && !parse_tz_minutes(tz, tz_min)) {
        D2O_WARN("SetSystemDateAndTime 里的 TimeZone 认不出来（\"%s\"），"
                 "按 UTC 处理。", tz.c_str());
        tz_min = 0;
      }
      if (xml_text(body_xml, "DaylightSavings") == "true") tz_min += 60;
      utc -= tz_min * 60;
    }

    // 旧偏移要在**设之前**取。这句存在的唯一理由是审计那一行要写成
    // "偏移 +120 → +180 秒"：只写新值的话，"这台设备的时间原本就差着多少"
    // 这个问题在日志里就没有答案了 —— 而排查"录像时间对不上"恰恰要从它开始。
    auto const old_off = device->time_offset().count();
    device->set_device_time(std::chrono::system_clock::from_time_t(utc));
    auto const off = device->time_offset().count();
    // 偏移大于 5 分钟是**值得看一眼**的：要么 NVR 的钟不对，要么时区搞错了。
    // 而它下一帧就会出现在水印上，所以不能只在 DEBUG 里说。
    if (off > 300 || off < -300) {
      D2O_WARN("设备时间被 NVR 改了 %+lld 秒（超过 5 分钟）：水印上的时间会跟着变。"
               "在网页状态页可以一键恢复成主机时间。",
               static_cast<long long>(off));
    } else {
      D2O_INFO("设备时间已按 NVR 同步，偏移 %+lld 秒。",
               static_cast<long long>(off));
    }
    // 时钟已经走起来了（上面那一句），这里只问**要不要把这个偏移留下来**。
    // 分工的理由、以及"为什么必须留下来"，见 `on_device_time_set` 的注释。
    //
    // 传的是 `off` 而不是 `utc`：调用方要的是"配置里那个偏移该写多少"，而
    // `off` 就是刚算出来、也刚写进 `DeviceState` 的那个数 —— 传绝对时刻等于
    // 让调用方自己再减一次主机时间，那是同一件事的第二个副本，两份迟早不一致。
    if (o.on_device_time_set) {
      o.on_device_time_set(static_cast<int>(off));
    }
    // 审计记的是**结果**（新旧偏移），不是报文里的那个绝对时刻：报文里有
    // `UTCDateTime` 和 `LocalDateTime`+`TZ` 两种写法，同一个时刻写出来能差
    // 好几行字，而读日志的人要的是"水印上的时间被挪了多少"。
    note_audit(peer, "onvif.SetSystemDateAndTime",
               "设了设备时间，偏移 " + std::to_string(static_cast<long long>(old_off)) +
                   " → " + std::to_string(static_cast<long long>(off)) + " 秒");
    return soap_response(v, kNsDevice, "SetSystemDateAndTimeResponse", "");
  }

  if (op == "GetDeviceInformation") {
    std::string inner;
    inner += "<tds:Manufacturer>" + xml_escape(o.manufacturer) +
             "</tds:Manufacturer>";
    inner += "<tds:Model>" + xml_escape(o.model) + "</tds:Model>";
    inner += "<tds:FirmwareVersion>" + xml_escape(o.firmware_version) +
             "</tds:FirmwareVersion>";
    inner += "<tds:SerialNumber>" + xml_escape(o.serial_number) +
             "</tds:SerialNumber>";
    inner += "<tds:HardwareId>" + xml_escape(o.hardware_id) + "</tds:HardwareId>";
    return soap_response(v, kNsDevice, "GetDeviceInformationResponse", inner);
  }

  if (op == "GetServices") {
    // NVR 主要靠这个找到"媒体服务在哪个 URL"。地址按请求来源反推 ——
    // 和 WS-Discovery 的 XAddrs 同一套逻辑：多网卡机器上写死一个地址，
    // 就是让 NVR 去连一个它够不着的网段。
    std::string inner;
    inner += "<tds:Service><tds:Namespace>" + std::string(kNsDevice) +
             "</tds:Namespace><tds:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port,
                                "/onvif/device_service")) +
             "</tds:XAddr><tds:Version><tt:Major>2</tt:Major>"
             "<tt:Minor>6</tt:Minor></tds:Version></tds:Service>";
    inner += "<tds:Service><tds:Namespace>" + std::string(kNsMedia) +
             "</tds:Namespace><tds:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port, "/onvif/media_service")) +
             "</tds:XAddr><tds:Version><tt:Major>2</tt:Major>"
             "<tt:Minor>6</tt:Minor></tds:Version></tds:Service>";
    return soap_response(v, kNsDevice, "GetServicesResponse", inner);
  }

  if (op == "GetCapabilities") {
    std::string inner;
    inner += "<tt:Capabilities>";
    // 只报 Device 和 Media。Events/PTZ/Imaging/Analytics 一个都没有 ——
    // 报了我们就会被调，而被调了只能回 Fault，白白拉长一次添加流程。
    inner += "<tt:Device><tt:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port,
                                "/onvif/device_service")) +
             "</tt:XAddr>"
             "<tt:Network><tt:IPFilter>false</tt:IPFilter>"
             "<tt:ZeroConfiguration>false</tt:ZeroConfiguration>"
             "<tt:IPVersion6>false</tt:IPVersion6>"
             "<tt:DynamicDNS>false</tt:DynamicDNS></tt:Network>"
             "<tt:System><tt:DiscoveryResolve>true</tt:DiscoveryResolve>"
             "<tt:DiscoveryBye>true</tt:DiscoveryBye>"
             "<tt:RemoteDiscovery>false</tt:RemoteDiscovery>"
             "<tt:SystemBackup>false</tt:SystemBackup>"
             "<tt:SystemLogging>false</tt:SystemLogging>"
             "<tt:FirmwareUpgrade>false</tt:FirmwareUpgrade></tt:System>"
             "<tt:IO><tt:InputConnectors>0</tt:InputConnectors>"
             "<tt:RelayOutputs>0</tt:RelayOutputs></tt:IO>"
             "<tt:Security><tt:TLS1.1>false</tt:TLS1.1>"
             "<tt:TLS1.2>false</tt:TLS1.2>"
             "<tt:OnboardKeyGeneration>false</tt:OnboardKeyGeneration>"
             "<tt:AccessPolicyConfig>false</tt:AccessPolicyConfig>"
             "<tt:X.509Token>false</tt:X.509Token>"
             "<tt:SAMLToken>false</tt:SAMLToken>"
             "<tt:KerberosToken>false</tt:KerberosToken>"
             "<tt:RELToken>false</tt:RELToken></tt:Security></tt:Device>";
    inner += "<tt:Media><tt:XAddr>" +
             xml_escape(url_for(peer, "http", o.http_port, "/onvif/media_service")) +
             "</tt:XAddr><tt:StreamingCapabilities>"
             "<tt:RTPMulticast>false</tt:RTPMulticast>"
             "<tt:RTP_TCP>true</tt:RTP_TCP>"
             "<tt:RTP_RTSP_TCP>true</tt:RTP_RTSP_TCP>"
             "</tt:StreamingCapabilities></tt:Media>";
    inner += "</tt:Capabilities>";
    return soap_response(v, kNsDevice, "GetCapabilitiesResponse", inner);
  }

  if (op == "GetServiceCapabilities") {
    // 设备服务的能力表。**和媒体服务那个同名但内容完全不同** —— 这正是
    // 按路由（而不是按 Body 的命名空间）分派的原因。
    std::string inner;
    inner += "<tds:Capabilities>";
    inner += "<tds:Network IPFilter=\"false\" ZeroConfiguration=\"false\" "
             "IPVersion6=\"false\" DynDNS=\"false\"/>";
    inner += "<tds:Security TLS1.1=\"false\" TLS1.2=\"false\" "
             "OnboardKeyGeneration=\"false\" AccessPolicyConfig=\"false\" "
             "X.509Token=\"false\" SAMLToken=\"false\" KerberosToken=\"false\" "
             "RELToken=\"false\"/>";
    inner += "<tds:System DiscoveryResolve=\"true\" DiscoveryBye=\"true\" "
             "RemoteDiscovery=\"false\" SystemBackup=\"false\" "
             "SystemLogging=\"false\" FirmwareUpgrade=\"false\"/>";
    inner += "</tds:Capabilities>";
    return soap_response(v, kNsDevice, "GetServiceCapabilitiesResponse", inner);
  }

  if (op == "GetNetworkInterfaces") {
    std::string inner;
    for (LocalAddr const& la : local_ipv4_addresses()) {
      inner += "<tds:NetworkInterfaces token=\"" + attr(la.ip) + "\">";
      inner += "<tt:Enabled>true</tt:Enabled>";
      inner += "<tt:Info><tt:Name>" + xml_escape(la.adapter) + "</tt:Name>";
      if (!la.mac.empty()) {
        inner += "<tt:HwAddress>" + xml_escape(la.mac) + "</tt:HwAddress>";
      }
      inner += "<tt:MTU>1500</tt:MTU></tt:Info>";
      inner += "<tt:Link><tt:AdminSettings>"
               "<tt:AutoNegotiation>true</tt:AutoNegotiation>"
               "<tt:Speed>100</tt:Speed><tt:Duplex>Full</tt:Duplex>"
               "</tt:AdminSettings><tt:OperSettings>"
               "<tt:AutoNegotiation>true</tt:AutoNegotiation>"
               "<tt:Speed>100</tt:Speed><tt:Duplex>Full</tt:Duplex>"
               "</tt:OperSettings><tt:InterfaceType>6</tt:InterfaceType>"
               "</tt:Link>";
      inner += "<tt:IPv4><tt:Enabled>true</tt:Enabled><tt:Config>"
               "<tt:Manual><tt:Address>" + xml_escape(la.ip) + "</tt:Address>"
               "<tt:PrefixLength>" + std::to_string(la.prefix_len) +
               "</tt:PrefixLength></tt:Manual>"
               "<tt:DHCP>false</tt:DHCP></tt:Config></tt:IPv4>";
      inner += "</tds:NetworkInterfaces>";
    }
    return soap_response(v, kNsDevice, "GetNetworkInterfacesResponse", inner);
  }

  if (op == "GetDNS") {
    std::string inner =
        "<tds:DNSInformation><tt:FromDHCP>false</tt:FromDHCP>"
        "<tt:SearchDomain>local</tt:SearchDomain>"
        "</tds:DNSInformation>";
    return soap_response(v, kNsDevice, "GetDNSResponse", inner);
  }

  return {};
}

std::string OnvifService::Impl::media_op(uvcpp_soap_version v,
                                         std::string const& op,
                                         std::string const& body_xml,
                                         std::string const& peer) {
  std::vector<OnvifStreamProfile> const& ps = profiles();

  if (op == "GetProfiles") {
    std::string inner;
    for (OnvifStreamProfile const& p : ps) inner += profile_block("", p);
    return soap_response(v, kNsMedia, "GetProfilesResponse", inner);
  }

  if (op == "GetProfile") {
    OnvifStreamProfile const* p = find_profile(profile_token_from_body(body_xml));
    if (p == nullptr) return {};
    // GetProfile 的响应体**就是**一个 Profiles 元素，不再多包一层。
    return soap_response(v, kNsMedia, "GetProfileResponse", profile_block("", *p));
  }

  if (op == "GetStreamUri") {
    OnvifStreamProfile const* p = find_profile(profile_token_from_body(body_xml));
    if (p == nullptr) return {};
    // StreamSetup 里的 Transport 我们**看但不改**：RTP over TCP 还是 UDP 是
    // 客户端通过 RTSP 自己的 Transport 头选的，GetStreamUri 只负责给地址。
    std::string inner = "<trt:MediaUri>";
    inner += "<tt:Uri>" + xml_escape(stream_uri(peer, *p)) + "</tt:Uri>";
    inner += "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>";
    // InvalidAfterReboot=true：重启后这个会话就没了，客户端要重连。
    // 报 false 会让 NVR 一直拿着一个已经失效的会话。
    inner += "<tt:InvalidAfterReboot>true</tt:InvalidAfterReboot>";
    inner += "<tt:Timeout>PT0S</tt:Timeout>";
    inner += "</trt:MediaUri>";
    return soap_response(v, kNsMedia, "GetStreamUriResponse", inner);
  }

  if (op == "GetSnapshotUri") {
    std::string inner = "<trt:MediaUri>";
    inner += "<tt:Uri>" + xml_escape(snapshot_uri(peer)) + "</tt:Uri>";
    inner += "<tt:InvalidAfterConnect>false</tt:InvalidAfterConnect>";
    inner += "<tt:InvalidAfterReboot>false</tt:InvalidAfterReboot>";
    inner += "<tt:Timeout>PT0S</tt:Timeout>";
    inner += "</trt:MediaUri>";
    return soap_response(v, kNsMedia, "GetSnapshotUriResponse", inner);
  }

  if (op == "GetVideoSources") {
    OnvifStreamProfile const& p = ps.front();
    std::string inner =
        "<trt:VideoSources token=\"" + std::string(kTokenVideoSource) + "\">";
    inner += "<tt:Framerate>" + std::to_string(p.fps) + "</tt:Framerate>";
    inner += "<tt:Resolution><tt:Width>" + std::to_string(p.width) +
             "</tt:Width><tt:Height>" + std::to_string(p.height) +
             "</tt:Height></tt:Resolution>";
    // Imaging 是可选的；我们没有曝光/白平衡可调，报出来只会引出必然失败的调用。
    inner += "</trt:VideoSources>";
    return soap_response(v, kNsMedia, "GetVideoSourcesResponse", inner);
  }

  if (op == "GetVideoSourceConfigurations" || op == "GetVideoSourceConfiguration") {
    std::string inner =
        "<trt:Configurations token=\"" + std::string(kTokenVsConfig) + "\">";
    inner += video_source_config("", ps.front());
    inner += "</trt:Configurations>";
    return soap_response(v, kNsMedia, op + "Response", inner);
  }

  if (op == "GetVideoEncoderConfigurations" ||
      op == "GetVideoEncoderConfiguration") {
    std::string inner;
    for (OnvifStreamProfile const& p : ps) {
      inner += "<trt:Configurations token=\"VideoEncoderConfig_" +
               attr(p.token) + "\">";
      inner += encoder_config("", p);
      inner += "</trt:Configurations>";
    }
    return soap_response(v, kNsMedia, op + "Response", inner);
  }

  if (op == "GetVideoEncoderConfigurationOptions") {
    std::string inner = "<trt:Options>";
    inner += "<tt:QualityRange><tt:Min>1</tt:Min><tt:Max>6</tt:Max>"
             "</tt:QualityRange>";
    // 每一路的分辨率都列进可选范围。有些 NVR 会拿这个列表校验，
    // 而"当前用的分辨率不在可选列表里"会被判成配置非法。
    for (OnvifStreamProfile const& q : ps) {
      inner += "<tt:H264><tt:ResolutionsAvailable><tt:Width>" +
               std::to_string(q.width) + "</tt:Width><tt:Height>" +
               std::to_string(q.height) +
               "</tt:Height></tt:ResolutionsAvailable>";
      inner += "<tt:GovLengthRange><tt:Min>1</tt:Min><tt:Max>" +
               std::to_string(clamp_int(q.fps * 10, 1, 100000)) +
               "</tt:Max></tt:GovLengthRange>";
      inner += "<tt:FrameRateRange><tt:Min>1</tt:Min><tt:Max>" +
               std::to_string(q.fps) + "</tt:Max></tt:FrameRateRange>";
      inner += "<tt:EncodingIntervalRange><tt:Min>1</tt:Min><tt:Max>1</tt:Max>"
               "</tt:EncodingIntervalRange>";
      inner += "<tt:H264ProfilesSupported>" + xml_escape(q.h264_profile) +
               "</tt:H264ProfilesSupported></tt:H264>";
    }
    inner += "</trt:Options>";
    return soap_response(v, kNsMedia,
                         "GetVideoEncoderConfigurationOptionsResponse", inner);
  }

  // 两个 `Set*` 合在一起写：报文形状几乎一样（都是 `Configuration` 里带
  // `Name` + 一组几何字段），拆成两份的话迟早只改一份。
  //
  // ## token 是**属性**，不是子元素
  //
  // `<tt:Configuration token="VideoEncoderConfig_main">` —— 拿只认
  // `ProfileToken>` **子元素**的那个查找去找它，一个都找不到，而找不到的表现是
  // "NVR 改了名/改了分辨率，什么都没发生，也不报错"。
  //
  // ## 认不出来时当主码流，并打一行 WARN
  //
  // 单路设备上"NVR 发了一个我们没见过的 token"是常态（它可能存着上一次的），
  // 回 Fault 会让它的通道添加流程当场失败。而**不能静默**：WARN 是唯一还能被
  // 人看见的地方。
  //
  // ## 接口一律返回成功
  //
  // 能当场生效的只有名称：一处进 `DeviceState`（画面水印下一帧就是新名字），
  // 一处进 profile 的 `tt:Name`（下一份 `GetProfiles` 就是新名字）。分辨率/
  // 帧率/码率/GOP 要重建编码器，**如实进 `pending`**，由网页上那个「立即应用」
  // 说了算。`Set*` 没有"稍后生效"的语义，回 Fault 会让海康停在"配置编码器"
  // 那一步 —— 用户看到的是"加不上"而不是"要重启"。这一条写进 README。
  if (op == "SetVideoEncoderConfiguration" ||
      op == "SetVideoSourceConfiguration") {
    std::string const token = xml_attr(body_xml, "token");
    OnvifStreamProfile const* found = resolve_config_target(token);
    if (found == nullptr && !token.empty()) {
      D2O_WARN("%s 里的 token \"%s\" 我们不认得（自己发出去的是 "
               "VideoEncoderConfig_<main|sub>），按主码流处理。",
               op.c_str(), token.c_str());
    }
    OnvifStreamProfile const& tgt = (found != nullptr) ? *found : ps.front();
    bool const sub_target = (tgt.token != ps.front().token);
    bool const allow_rename = (op == "SetVideoEncoderConfiguration");

    OnvifDevicePatch const patch =
        patch_from_body(body_xml, tgt, sub_target, allow_rename);

    // 让我们**下一份 `GetProfiles` 就报这个名字**。
    //
    // 这一句是"改名到底生效了没有"在协议层唯一的证据。NVR 改完通道名之后会
    // 回读一次，而报旧名字的话它会认为这次改名没落地 —— 有些 NVR 会就此把
    // 名字**改回**它自己的缓存值，于是用户看到的是"改了又跳回来"，
    // 而服务端这边水印明明已经变了。
    //
    // **下标由 `tgt` 反推，不是由 `found`。** 认不出 token 时我们刚刚**对 NVR
    // 说了**"按主码流处理"，那就得整条路都按主码流：只改 `device_name` 而不改
    // profile 的 `tt:Name`，留下的是一个自相矛盾的设备 —— 水印上写着 NVR-CH01、
    // NVR 回读到的还是 main，而它只在"NVR 存着一个旧 token"时才发作。
    //
    // `found == nullptr` 在这里**不等于越界**：那时 `tgt` 就是 `ps.front()`，
    // 下标 0，指向的是 `ps` 自己那份数据。
    //
    // **但 `opt.profiles` 为空时必须跳过。** `profiles()` 那时返回的是静态兜底
    // 表 `kFallback`（`opt.profiles` 为空 = RTSP 没开 = 一路码流都没有），
    // `ps.data()` 指向**它**，而 `find_profile` 会在那张表里命中并返回指针 ——
    // 拿那个下标去写空的 `opt.profiles` 是越界写。兜底表是编译期常量，它本来
    // 就没有"改名"这回事，这里跳过是唯一的正确做法（跳过的后果只是那一路
    // 报的仍是占位名字，而那个配置下根本没有码流可加）。
    if (patch.name && !opt.profiles.empty()) {
      std::size_t const idx = static_cast<std::size_t>(&tgt - ps.data());
      opt.profiles[idx].name = *patch.name;
    }

    if (patch.empty()) {
      // 报文里没有一项我们能改的。**不是错误** —— NVR 常常把整份配置原样
      // 送回来（它只是"确认一下"）。但这值得一行 DEBUG：真在改东西却全被
      // 丢掉的话，这行是唯一的线索。
      D2O_DEBUG("%s 里没有我们能应用的字段（token=\"%s\"）。", op.c_str(),
                token.c_str());
    } else if (opt.on_patch) {
      // 落盘 + 推热字段 + 重算 pending 全在回调里（见 OnvifServiceOptions 的
      // 注释：那套东西的权威副本在 main.cpp）。
      opt.on_patch(patch);
    } else if (patch.name && device) {
      // 没有落盘回调（单跑录制那条路）：至少把名称推给 `DeviceState`，让水印
      // 跟着变 —— 这是"内存里的设备状态"与"配置"之间那条边界的唯一一处让步。
      device->set_channel_name(*patch.name);
    }

    // 审计：**只在报文里真有我们能改的字段时写**。
    //
    // 上面那个 `patch.empty()` 的分支是常态（NVR 把读到的整份配置原样送回来
    // 确认一下），把它也记一行的话，这份日志里"改名"那条会被几百行
    // "SetVideoEncoderConfiguration" 淹掉 —— 而用户要的正是"谁在什么时候改了
    // 摄像头名称"。
    //
    // 动作串带上 `op`：`SetVideoSourceConfiguration` 只会带 `Name`（改名），
    // 而 `SetVideoEncoderConfiguration` 带的是编码参数。两者都影响画面，但
    // 排查时要知道改的是哪一类。
    if (!patch.empty()) {
      note_audit(peer, "onvif." + op, patch_detail(patch));
    }
    return soap_response(v, kNsMedia, op + "Response", "");
  }

  // --- OSD（水印） --------------------------------------------------------
  //
  // 六个操作写在一处：它们共用 token 规则、共用 `osd_block`，拆开写的话迟早
  // 只改其中几个。元素的**前缀**照 `onvif_wsdl` 里那两个 schema 的归属分：
  //
  //   * `media.wsdl` 里声明的（`GetOSDsResponse`/`OSDs`/`OSD`/`OSDToken`/
  //     `OSDOptions`）→ `trt:`。那份内联 schema 的 `elementFormDefault` 是
  //     `qualified`，targetNamespace 是 media/wsdl ⇒ 这些元素都在 trt 里。
  //     连 `OSDToken` 也是 —— 它的**类型**是 `tt:ReferenceToken`，但元素本身
  //     是 trt 的。写 `tt:OSDToken` 是一个类型系统看不出来的错。
  //   * `onvif.xsd` 里声明的（`tt:OSDConfiguration` 的子元素、
  //     `tt:MaximumNumberOfOSDs` …）→ `tt:`。
  //
  // 判断依据是"这个 `xs:element` 写在哪个文件里"，不是"名字看起来像谁的"。

  if (op == "GetOSDs") {
    // 规范原文（media.wsdl 里 GetOSDs/ConfigurationToken 的 documentation）：
    // "If token not exist, request all available OSDs."
    // ⇒ 认不出的 `ConfigurationToken` **回全部**，不是 Fault。我们本来就只有
    // 一个视频源，所以这句话在这里等价于"忽略它"。
    std::string inner;
    for (std::size_t i = 0; i < opt.osd.items.size(); ++i) {
      inner += osd_block("OSDs", osd_token(i), opt.osd.items[i]);
    }
    return soap_response(v, kNsMedia, "GetOSDsResponse", inner);
  }

  if (op == "GetOSD") {
    std::size_t idx = 0;
    if (!osd_index_from_token(osd_token_from_body(body_xml),
                              opt.osd.items.size(), idx)) {
      return osd_token_fault(body_xml);
    }
    // 单个的那个元素叫 `OSD`，不是 `OSDs` —— 列表项和单项在 schema 里是**两个
    // 不同的元素名**，统一成一个的话那一个一定错，而客户端那边是静默的。
    return soap_response(v, kNsMedia, "GetOSDResponse",
                         osd_block("OSD", osd_token(idx), opt.osd.items[idx]));
  }

  if (op == "DeleteOSD") {
    OsdConfig next = opt.osd;
    std::size_t idx = 0;
    if (!osd_index_from_token(osd_token_from_body(body_xml),
                              next.items.size(), idx)) {
      return osd_token_fault(body_xml);
    }
    // **先把它长什么样记下来，再删。** 只记"删了 OSD_1"的话，过一会儿就没人
    // 知道那对应画面上曾经有过什么 —— 而 token 是位置派生的下标（见
    // `osd_token`），删完之后同一个字符串指的是另一项。这一行要回答的是
    // "谁把右下角那个名称水印删掉了"，不是一个下标。
    std::string const removed = osd_item_detail(next.items[idx]);
    next.items.erase(next.items.begin() +
                     static_cast<std::ptrdiff_t>(idx));
    apply_osd(next);
    note_audit(peer, "onvif.DeleteOSD", osd_token(idx) + "：" + removed);
    return soap_response(v, kNsMedia, "DeleteOSDResponse", "");
  }

  if (op == "SetOSD" || op == "CreateOSD") {
    OsdConfig next = opt.osd;
    std::size_t idx = 0;
    if (op == "CreateOSD") {
      if (next.items.size() >= OsdConfig::kMaxItems) {
        // 上限是配置层定的（`OsdConfig::kMaxItems`）。回 Fault 而不是"默默把
        // 多出来的丢掉"：丢掉的话 NVR 那边以为建好了、回读时又找不到它，
        // 而它没有别的办法知道是为什么。
        return fail({FaultKind::kBadRequest, "InvalidArgVal", "NoConfig",
                     "OSD limit reached (max " +
                         std::to_string(OsdConfig::kMaxItems) + ")"});
      }
      // 新项从**默认项**起步（`type = "time"`、左上角）。它是三种类型里唯一
      // 不需要外部输入就能画出东西的：`text` 要有文字、`name` 要有设备名，
      // 而报文里常常只给了一个位置。
      next.items.push_back(OsdItemConfig{});
      idx = next.items.size() - 1;
    } else if (!osd_index_from_token(xml_attr(body_xml, "token"),
                                     next.items.size(), idx)) {
      // `SetOSD` 的 token 是**属性**（`tt:DeviceEntity/@token`），不是子元素。
      return osd_token_fault(body_xml);
    }

    if (!osd_item_from_body(body_xml, next.items[idx])) {
      // 这一项我们表示不了（比如 `Type=Image`）。**把刚加进去的那一项撤掉**：
      // 失败之后在列表里留一个自己画不出来的项，NVR 回读时会看到它，
      // 而画面上什么都没有 —— 那是最难查的一种不一致。
      if (op == "CreateOSD") next.items.pop_back();
      return fail({FaultKind::kBadRequest, "InvalidArgVal", "NoConfig",
                   "only Text OSD is supported"});
    }

    apply_osd(next);
    // 审计：记的是**改完之后那一项的样子**（`next.items[idx]`），不是报文里
    // 提了哪几个字段 —— 水印是热生效的（Phase S），所以这一行读起来就是
    // "画面上现在是什么样"，与 `GetOSDs` 回读到的完全一致。
    //
    // 这一条**用得着 `note_audit` 的闸门**：NVR 的 OSD 页面每保存一次就会把
    // 整份水印写回来一遍，不管用户有没有真的改动。
    note_audit(peer, op == "CreateOSD" ? "onvif.CreateOSD" : "onvif.SetOSD",
               osd_token(idx) + "：" + osd_item_detail(next.items[idx]));
    if (op == "CreateOSD") {
      return soap_response(v, kNsMedia, "CreateOSDResponse",
                           "<trt:OSDToken>" + osd_token(idx) +
                               "</trt:OSDToken>");
    }
    return soap_response(v, kNsMedia, "SetOSDResponse", "");
  }

  if (op == "GetOSDOptions") {
    // **`ConfigurationToken` 认不出也不回 Fault。** 规范里它是必填，但它是
    // "要和哪个视频源配置兼容"的意思 —— 我们只有一个源，回 Fault 只会让
    // NVR 的 OSD 页面在拿到选项这一步就断掉（页面空白），而它本来完全能工作。
    std::string inner = "<trt:OSDOptions>";
    // `Total` 是 `use="required"`（onvif.xsd 的 `tt:MaximumNumberOfOSDs`）：
    // 少这个属性就是一个不合 schema 的响应，而解析器报的是"缺属性"，
    // 拿到这个响应的人很难联想到"OSD 上限没报"。
    inner += "<tt:MaximumNumberOfOSDs Total=\"" +
             std::to_string(OsdConfig::kMaxItems) +
             "\" Image=\"0\" PlainText=\"" +
             std::to_string(OsdConfig::kMaxItems) + "\" Date=\"1\" Time=\"1\" "
             "DateAndTime=\"1\"/>";
    // `Type` 是 `tt:OSDType`，取值只有 `Text` / `Image` / `Extended`。
    inner += "<tt:Type>Text</tt:Type>";
    // 五个预定义位置，一个不少 —— 少一个就有 NVR 的下拉框里缺一项。
    // `Custom` 也报，因为我们确实支持任意坐标（见 `osd_block` 里的 `tt:Pos`）。
    inner += "<tt:PositionOption>UpperLeft</tt:PositionOption>";
    inner += "<tt:PositionOption>UpperRight</tt:PositionOption>";
    inner += "<tt:PositionOption>LowerLeft</tt:PositionOption>";
    inner += "<tt:PositionOption>LowerRight</tt:PositionOption>";
    inner += "<tt:PositionOption>Custom</tt:PositionOption>";
    inner += "<tt:TextOption>";
    // 四个文字类型，和我们能表示的一一对应（`Plain`/`Date`/`Time`/
    // `DateAndTime`）。少报一个的症状是 NVR 上那个选项灰着，而它其实能用。
    inner += "<tt:Type>Plain</tt:Type>";
    inner += "<tt:Type>Date</tt:Type>";
    inner += "<tt:Type>Time</tt:Type>";
    inner += "<tt:Type>DateAndTime</tt:Type>";
    // 范围就是配置层夹的那个（`font_size` 0..512 像素，下限另受
    // `OsdOverlay` 的 16 像素限制），换算成 pt 报出去（见 `pt_from_px`）。
    // 报一个比实际能接受的更宽的范围，会让 NVR 设一个我们默默夹掉的值。
    inner += "<tt:FontSizeRange><tt:Min>" + std::to_string(pt_from_px(16)) +
             "</tt:Min><tt:Max>" + std::to_string(pt_from_px(512)) +
             "</tt:Max></tt:FontSizeRange>";
    // 只报我们**真的认**的那几个格式（`strftime_from_onvif_format` 认得全的
    // 就这些）。报多了的症状是 NVR 选了一个我们解析不出来 → 静默保持原样。
    inner += "<tt:DateFormat>yyyy-MM-dd</tt:DateFormat>";
    inner += "<tt:DateFormat>yyyy/MM/dd</tt:DateFormat>";
    inner += "<tt:DateFormat>dd/MM/yyyy</tt:DateFormat>";
    inner += "<tt:TimeFormat>HH:mm:ss</tt:TimeFormat>";
    inner += "<tt:TimeFormat>HH:mm</tt:TimeFormat>";
    inner += "</tt:TextOption>";
    inner += "</trt:OSDOptions>";
    return soap_response(v, kNsMedia, "GetOSDOptionsResponse", inner);
  }

  if (op == "GetServiceCapabilities") {
    // ## `OSD` 是属性，长在 `trt:Capabilities` 的开标签里
    //
    // 它是 `trt:Capabilities` 的 `xs:boolean` 属性（media.wsdl:34-76，
    // 原文 "Indicates if OSD is supported."）。**不报它就等于告诉 NVR
    // "这台设备不支持 OSD"** —— 海康/大华的 OSD 页面会整个灰掉，而下面那六个
    // handler 就在这儿等着，谁都不会去调。
    std::string inner = "<trt:Capabilities OSD=\"true\">";
    // ## 这两个子元素是 `trt:` 的，不是 `tt:` 的
    //
    // 它们是在 `media.wsdl` 那个内联 schema 里**就地声明**的（写在
    // `trt:Capabilities` 的 sequence 里），而那份 schema 的
    // `elementFormDefault="qualified"`、targetNamespace 是 media/wsdl ⇒ 它们
    // 落进 trt。这里以前写的是 `tt:`，而 `tt:` 前缀在信封上**声明过**
    // （别的元素在用），所以 `Runner.strict_xml` 一声不响 —— 这正是
    // "前缀没声明"和"元素名写错"是两类错、而后者只能靠**断言具体元素名**
    // 来挡的例子。
    inner += "<trt:ProfileCapabilities MaximumNumberOfProfiles=\"" +
             std::to_string(ps.size()) + "\"/>";
    inner += "<trt:StreamingCapabilities RTPMulticast=\"false\" "
             "RTP_TCP=\"true\" RTP_RTSP_TCP=\"true\" "
             "NonAggregateControl=\"false\"/>";
    inner += "</trt:Capabilities>";
    return soap_response(v, kNsMedia, "GetServiceCapabilitiesResponse", inner);
  }

  return {};
}

OnvifService::OnvifService() : impl_(std::make_unique<Impl>()) {}
OnvifService::~OnvifService() = default;

OnvifIdentity identity_from_config(OnvifConfig const& c) {
  OnvifIdentity id;
  id.user = c.user;
  id.password = c.password;
  id.manufacturer = c.manufacturer;
  id.model = c.model;
  id.firmware_version = c.firmware_version;
  id.hardware_id = c.hardware_id;
  id.serial_number = c.serial_number;
  id.device_name = c.device_name;
  id.location = c.location;
  return id;
}

void apply_identity(OnvifServiceOptions& o, OnvifIdentity const& id) {
  o.user = id.user;
  o.password = id.password;
  o.manufacturer = id.manufacturer;
  o.model = id.model;
  o.firmware_version = id.firmware_version;
  o.hardware_id = id.hardware_id;
  o.serial_number = id.serial_number;
  o.device_name = id.device_name;
  o.location = id.location;
}

void identity_to_config(OnvifIdentity const& id, OnvifConfig& c) {
  c.user = id.user;
  c.password = id.password;
  c.manufacturer = id.manufacturer;
  c.model = id.model;
  c.firmware_version = id.firmware_version;
  c.hardware_id = id.hardware_id;
  c.serial_number = id.serial_number;
  c.device_name = id.device_name;
  c.location = id.location;
}

void OnvifService::set_options(OnvifServiceOptions o) {
  impl_->opt = std::move(o);
  // 凭据可能变了，重建校验器。顺带把先前发出去的 nonce 全部作废 —— 换口令
  // 之后旧的 nonce 不该还能用。
  impl_->auth.reset();
}

void OnvifService::set_identity(OnvifIdentity const& id) {
  Impl& d = *impl_;
  // **逐字段赋值，不是整体替换 `opt`。** 整体替换会把 `profiles` 里的分辨率
  // 一起打回启动时的猜测值 —— 那两个数是被每秒一次的统计上报**持续改写**的
  // （`set_video_size`），而 Web 服务和采集是两个独立的启动阶段：替换之后
  // `GetProfiles` 会在下一条上报到达之前报错分辨率，NVR 恰恰可能正好在这一秒
  // 里来拉 profile。
  bool const creds_changed = (d.opt.user != id.user) || (d.opt.password != id.password);
  apply_identity(d.opt, id);
  if (creds_changed) d.auth.reset();
  // 名称也推给 `DeviceState`：水印那个「名称」项每帧现读它，所以 NVR 改完名
  // 下一帧画面上就是新名字，不需要重开管线（重开管线会把录制切段）。
  //
  // 放在这里而不是让调用方各写一遍：改名的入口有三个（启动、网页、NVR），
  // 而它们最终都汇到 `apply_identity` 这一个赋值上。
  if (d.device) d.device->set_channel_name(id.device_name);
}

void OnvifService::set_device_state(DeviceState* state) { impl_->device = state; }

void OnvifService::set_osd(OsdConfig const& c) {
  // **只换这一份，不换 `opt` 整体。** 和 `set_identity` 同一个理由：整体替换会
  // 把 `profiles` 里的分辨率打回启动时的猜测值，而那些数被每秒一次的统计上报
  // 持续改写（见 `set_identity` 那段）。
  //
  // **不调 `on_osd_patch`。** 这个 setter 的调用方是网页那条路
  // （`WebServer::apply_osd_config`），改动本来就是它自己写盘、自己算 pending
  // 的；再回调一次会让同一份配置写两遍盘，而第二遍是在 loop 线程上同步跑的
  // （`app.post` 在 loop 线程上是就地执行），等于在保存配置的那条路上再套一层
  // 保存。SOAP 那条路反过来 —— 它是**改动**的发起者，所以由 `apply_osd` 回调。
  impl_->opt.osd = c;
}

OnvifServiceOptions const& OnvifService::options() const { return impl_->opt; }

void OnvifService::set_anonymous_date_time(bool on) { impl_->anonymous_dt_ = on; }

bool OnvifService::verify_http_digest(uvcpp_web_request& req,
                                      std::string& challenge) {
  Impl& d = *impl_;
  std::string const hdr = req.header("Authorization");
  bool stale = false;
  // **顺序不能反：先 verify 再取 challenge。** `stale` 是 `verify()` 在
  // "摘要算对了、但 nonce 过期了"时置上的，而 `challenge(stale)` 要靠它决定
  // 发不发 `stale=true`。先取挑战串的话，nonce 过期的客户端拿到的是一份
  // 非 stale 的挑战，它会原样重试同一个过期 nonce —— 又 401，循环。
  DigestAuth& auth = d.authenticator();
  char const* const m = req.method_name();
  bool const ok = auth.verify(m != nullptr ? m : "GET", req.path(),
                              req.raw_url().empty() ? req.path() : req.raw_url(),
                              hdr, stale);
  challenge = auth.challenge(stale);
  if (!ok) {
    D2O_WARN("快照鉴权失败：来自 %s 的 %s %s，%s。", req.peer_ip().c_str(),
             m != nullptr ? m : "GET", req.path().c_str(),
             hdr.empty() ? "请求里既没有会话 cookie 也没有 Authorization 头"
                         : "发来了 Authorization 但摘要对不上（口令或算法不符）");
  }
  return ok;
}

void OnvifService::set_http_port(int p) { impl_->opt.http_port = p; }

void OnvifService::set_video_size(std::size_t index, int w, int h) {
  if (w <= 0 || h <= 0) return;
  // 越界静默返回而不是崩：这条路每秒走一次，而它唯一可能的错误是调用方把
  // 码流下标搞错了 —— 那种错该在启动时被发现，不该在一条来自网络的 SOAP
  // 请求路径上炸掉。profiles() 的空兜底也在这里生效。
  std::vector<OnvifStreamProfile>& ps = impl_->opt.profiles;
  if (index >= ps.size()) return;
  ps[index].width = w;
  ps[index].height = h;
}

std::uint64_t OnvifService::requests() const {
  return impl_->requests.load(std::memory_order_relaxed);
}
std::uint64_t OnvifService::auth_failures() const {
  return impl_->auth_failures.load(std::memory_order_relaxed);
}
std::uint64_t OnvifService::faults() const {
  return impl_->faults.load(std::memory_order_relaxed);
}
std::string OnvifService::last_unsupported() const {
  return impl_->last_unsupported;
}

bool OnvifService::handle(uvcpp_web_request& req, uvcpp_web_response& resp,
                          bool media_service) {
  Impl& d = *impl_;
  d.requests.fetch_add(1, std::memory_order_relaxed);

  // SOAP 请求都是几百字节。给到 256 KB 是留足余量，同时挡住"往这个端点
  // 灌 16 MB"这种最简单的骚扰。
  constexpr std::size_t kMaxSoapBytes = 256u * 1024u;
  if (req.body_size() > kMaxSoapBytes) {
    resp.status(413).text("SOAP body too large").end();
    return true;
  }

  // **必须用 body_data()/body_size()，不能用 content_length()。** 后者是
  // Content-Length 头里的**声明值**，和实到的字节数可能对不上 ——
  // 不一致本身就是攻击信号，而按声明值去读会越界。
  std::string const body(req.body_data(), req.body_size());

  // 凭据有无，解析之前就要知道 —— 解析失败的岔路要用它（见下）。
  std::string const hdr = req.header("Authorization");

  // 决定一次"未鉴权"的应答：401 + 挑战。**必须带挑战** —— 空挑战 = 不发
  // `WWW-Authenticate` 头，而一个没有挑战的 401 客户端只能放弃。
  bool auth_stale = false;
  auto challenge_401 = [&]() {
    d.auth_failures.fetch_add(1, std::memory_order_relaxed);
    resp.status(401)
        .text("Unauthorized")
        .set_header("WWW-Authenticate", d.authenticator().challenge(auth_stale))
        .end();
  };

  uvcpp_soap_message msg;
  char const* why = nullptr;
  if (uvcpp_soap_parse(body, msg, uvcpp::uvcpp_wsdl_limits(), &why) !=
      uvcpp::wsdl_status::OK) {
    // **没带凭据时回 401，不回 Fault。** 报文坏了当然是客户端的错，但一个
    // 连凭据都还没发过的客户端，此刻需要知道的是"请先鉴权"，不是"你的 SOAP
    // 有问题" —— 后者会让它永远不去算摘要。
    //
    // 这条不是理论问题，是 curl --digest 的现场（多数 NVR 的第一次请求也走
    // 同一条路）：它第一发故意不带凭据、只等挑战；我们回 500，它就放弃了，
    // 于是现象变成"客户端算出的摘要完全正确、服务端却一律 401"，看着像摘要
    // 算法写反了。带上凭据却发来坏报文，那才是 Fault —— 那时候确实该告诉他。
    if (hdr.empty()) {
      challenge_401();
      return true;
    }
    // 解析失败时**没法知道客户端说的是哪个 SOAP 版本** —— 连信封都没读出来。
    // 默认按 1.2 回（ONVIF 的绝大多数实现都是 1.2），并且给出一条结构良好的
    // Fault：断掉的 HTTP 响应会让部分 NVR 直接放弃添加设备。
    d.faults.fetch_add(1, std::memory_order_relaxed);
    FaultInfo f;
    f.kind = FaultKind::kBadRequest;
    f.subcode = "InvalidArgVal";
    f.reason = std::string("Malformed SOAP message: ") +
               (why != nullptr ? why : "parse error");
    std::string const xml = soap_fault_xml(uvcpp_soap_version::V1_2, f);
    resp.status(500);
    resp.set_content_type(uvcpp::uvcpp_soap_content_type(uvcpp_soap_version::V1_2));
    resp.body(xml);
    resp.end();
    return true;
  }

  uvcpp_soap_version const v = msg.version;
  std::string const& op = msg.body_local;
  std::string const ct = uvcpp::uvcpp_soap_content_type(v);

  auto send_fault = [&](FaultInfo const& f) {
    d.faults.fetch_add(1, std::memory_order_relaxed);
    resp.status(500);
    resp.set_content_type(ct);
    resp.body(soap_fault_xml(v, f));
    resp.end();
  };

  if (!msg.is_call()) {
    FaultInfo f;
    f.kind = FaultKind::kBadRequest;
    f.subcode = "InvalidArgVal";
    f.reason = "Body must contain exactly one non-Fault element";
    send_fault(f);
    return true;
  }

  // ---- 鉴权 ----------------------------------------------------------
  // `GetSystemDateAndTime` 是唯一的匿名入口：客户端要靠它拿到设备时间来纠正
  // 自己时钟的偏移，而 Digest 的摘要正是拿时间算的 —— 要求它鉴权是先有鸡
  // 还是先有蛋。
  bool const is_date_time = (!media_service && op == "GetSystemDateAndTime");
  if (!(is_date_time && d.anonymous_dt_)) {
    // 注意是 `d.authenticator()`（进程内唯一），不是就地新建一个。
    DigestAuth& auth = d.authenticator();
    char const* const m = req.method_name();
    bool const ok =
        auth.verify(m != nullptr ? m : "POST", req.path(),
                    req.raw_url().empty() ? req.path() : req.raw_url(), hdr, auth_stale);
    if (!ok) {
      // 只记"有没有发凭据"和来源，**不记摘要头本身** —— 它对排查没有帮助，
      // 却是一份可重放的凭据。这两条信息正好区分开两种最常见的现场：
      // 客户端压根没发 Authorization（口令没配），还是发了但算错了（口令不对）。
      D2O_WARN("ONVIF 鉴权失败：来自 %s 的 %s %s，%s。",
               req.peer_ip().c_str(), m != nullptr ? m : "POST", req.path().c_str(),
               hdr.empty() ? "请求里没有 Authorization 头"
                           : "发来了 Authorization 但摘要对不上（口令或算法不符）");
      challenge_401();
      return true;
    }
  }

  std::string const peer = req.peer_ip();
  std::string const xml = media_service ? d.media_op(v, op, msg.body_xml, peer)
                                        : d.device_op(v, op, msg.body_xml, peer);

  if (xml.empty()) {
    if (d.fault_pending) {
      d.fault_pending = false;
      send_fault(d.fault);
      return true;
    }
    // 两种情况会走到这里：不认识的接口，或者 ProfileToken 查不到。
    // 对客户端来说"你问的这个我没有"是同一件事，所以共用一条 Fault。
    d.last_unsupported = op;
    D2O_INFO("ONVIF 不支持的请求：%s（%s服务）",
             op.empty() ? "(空操作名)" : op.c_str(), media_service ? "媒体" : "设备");
    FaultInfo f;
    f.kind = FaultKind::kNotSupported;
    f.subcode = "ActionNotSupported";
    f.sub_subcode = "NoSuchService";
    f.reason = "Action " + op + " is not supported by this device";
    send_fault(f);
    return true;
  }

  resp.status(200);
  resp.set_content_type(ct);
  resp.body(xml);
  // **响应发出去之后**才发起对时。这一段的形状和 `/api/restart` 那一条完全
  // 一样（web_server.cpp:1170）：`on_sent` 的回调跑在循环线程上，所以它只能
  // 做"置位 + notify"这种瞬间完成的事 —— 真正阻塞的 UDP 查询在 NtpSyncer
  // 自己的线程上。照这个写，别发明第五种。
  //
  // 发送失败**不改主意**：对时改的是设备自己的内部状态，与这一次响应有没有
  // 送达无关。但要说一句 —— 不说的话，"NVR 那边超时了、可设备时间还是变了"
  // 只能靠人猜。
  if (d.ntp_requested) {
    d.ntp_requested = false;
    resp.on_sent([&d](uvcpp_web_sent_info const& info) {
      if (!info.ok) {
        D2O_WARN("NTP 对时的响应没能送达（连接已断，状态码 %d），对时照常发起。",
                 info.status_code);
      }
      if (d.opt.on_ntp_request) d.opt.on_ntp_request();
    });
  }
  resp.end();
  return true;
}

}  // namespace d2o
