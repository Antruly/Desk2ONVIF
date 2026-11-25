#include "onvif/wss.h"

#include "onvif/digest.h"

#include <openssl/evp.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>

namespace d2o {
namespace {

// ---------------------------------------------------------------------------
// 时间
// ---------------------------------------------------------------------------

/// Unix 秒。**不用 `timegm` / `_mkgmtime`**：两个名字各平台不同（本项目的
/// ONVIF 代码里已经为同一件事绕过一次）。这里只做 `time_since_epoch()`。
std::int64_t unix_seconds(std::chrono::system_clock::time_point tp) {
  return std::chrono::duration_cast<std::chrono::seconds>(tp.time_since_epoch())
      .count();
}

/// 公历日期 → 1970-01-01 起的天数（Howard Hinnant 的 days_from_civil）。
/// 全是整数运算，没有时区、没有 `tm`，也就没有"夏令时让窗口差一小时"。
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= (m <= 2);
  std::int64_t const era = (y >= 0 ? y : y - 399) / 400;
  unsigned const yoe = static_cast<unsigned>(y - era * 400);
  unsigned const mp = (m > 2) ? m - 3 : m + 9;
  unsigned const doy = (153u * mp + 2u) / 5u + d - 1u;
  unsigned const doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

bool is_digit(char c) { return c >= '0' && c <= '9'; }

int two_digits(std::string const& s, std::size_t at) {
  return (s[at] - '0') * 10 + (s[at + 1] - '0');
}

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------

int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

std::string b64_encode(unsigned char const* p, std::size_t n) {
  if (n > static_cast<std::size_t>(0x7fffffff)) return {};
  std::string out(4u * ((n + 2u) / 3u), '\0');
  if (out.empty()) return {};
  int const len = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(&out[0]), p,
                                  static_cast<int>(n));
  if (len < 0) return {};
  out.resize(static_cast<std::size_t>(len));
  return out;
}

bool sha1_raw(std::string const& data, unsigned char out[20]) {
  EVP_MD* md = EVP_MD_fetch(nullptr, "SHA1", nullptr);
  if (md == nullptr) return false;
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  int const ok = EVP_Digest(data.data(), data.size(), digest, &len, md, nullptr);
  EVP_MD_free(md);
  if (ok != 1 || len != 20) return false;
  std::memcpy(out, digest, 20);
  return true;
}

// ---------------------------------------------------------------------------
// XML 局部解析
//
// 输入是 `uvcpp_soap_parse` 的 `header_xml`：**已经良构**，所以这里只按
// 局部名定位元素，不重做实体/深度/节点数的防护（那些在解析那一层）。
// ---------------------------------------------------------------------------

bool is_xml_space(char c) {
  return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_name_end(char c) {
  return is_xml_space(c) || c == '>' || c == '/' || c == '=';
}

/// `wsse:Username` → `Username`。认不出前缀（没有冒号）就原样返回。
std::string local_name(std::string const& qname) {
  std::size_t const at = qname.rfind(':');
  return at == std::string::npos ? qname : qname.substr(at + 1);
}

/// 跳过引号里的 `>`（属性值里可以有）。返回 `>` 的下标。
std::size_t find_tag_end(std::string const& xml, std::size_t from,
                         std::size_t limit) {
  char quote = 0;
  for (std::size_t i = from; i < limit && i < xml.size(); ++i) {
    char const c = xml[i];
    if (quote != 0) {
      if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '>') {
      return i;
    }
  }
  return std::string::npos;
}

/// 在 `[from, limit)` 里找 `</qname ...>` 的起始位置。
bool find_close_tag(std::string const& xml, std::string const& qname,
                    std::size_t from, std::size_t limit, std::size_t& out) {
  std::size_t p = from;
  while (p < limit) {
    p = xml.find("</", p);
    if (p == std::string::npos || p >= limit) return false;
    std::size_t n = p + 2;
    while (n < limit && n < xml.size() && !is_name_end(xml[n])) ++n;
    if (n <= limit && xml.compare(p + 2, n - p - 2, qname) == 0) {
      out = p;
      return true;
    }
    p += 2;
  }
  return false;
}

struct Element {
  std::size_t tag_begin = 0;
  std::size_t tag_end = 0;      ///< `>` 之后
  std::size_t content_begin = 0;
  std::size_t content_end = 0;  ///< 结束标签的 `<`（自闭合时等于 begin）
};

/// 找第一个局部名等于 `local` 的元素。跳过注释/PI/结束标签，但**不跳过**
/// 不匹配元素的子树 —— 要找的 `Security` 正是嵌在 `Header` 里的。
bool find_element(std::string const& xml, std::string const& local,
                  std::size_t from, std::size_t limit, Element& out) {
  std::size_t lt = from;
  while (lt < limit) {
    lt = xml.find('<', lt);
    if (lt == std::string::npos || lt >= limit) return false;
    if (lt + 1 >= limit || lt + 1 >= xml.size()) return false;

    char const c = xml[lt + 1];
    if (c == '!') {
      // 注释可能含 `<` 甚至 `>`；必须整体跳过。
      if (xml.compare(lt, 4, "<!--") == 0) {
        std::size_t const e = xml.find("-->", lt + 4);
        if (e == std::string::npos || e + 3 > limit) return false;
        lt = e + 3;
      } else {
        std::size_t const e = xml.find('>', lt + 1);
        if (e == std::string::npos || e >= limit) return false;
        lt = e + 1;
      }
      continue;
    }
    if (c == '?' || c == '/') {
      std::size_t const e = xml.find('>', lt + 1);
      if (e == std::string::npos || e >= limit) return false;
      lt = e + 1;
      continue;
    }

    std::size_t n = lt + 1;
    while (n < limit && n < xml.size() && !is_name_end(xml[n])) ++n;
    std::string const qname = xml.substr(lt + 1, n - lt - 1);
    std::size_t const gt = find_tag_end(xml, n, limit);
    if (gt == std::string::npos) return false;

    if (local_name(qname) == local) {
      std::size_t back = gt;
      while (back > n && is_xml_space(xml[back - 1])) --back;
      bool const self_closing = back > n && xml[back - 1] == '/';

      out.tag_begin = lt;
      out.tag_end = gt + 1;
      out.content_begin = gt + 1;
      if (self_closing) {
        out.content_end = gt + 1;
        return true;
      }
      std::size_t close_lt = 0;
      if (!find_close_tag(xml, qname, gt + 1, limit, close_lt)) return false;
      std::size_t const close_gt = find_tag_end(xml, close_lt + 2, limit);
      if (close_gt == std::string::npos) return false;
      out.content_end = close_lt;
      return true;
    }
    lt = gt + 1;
  }
  return false;
}

/// 把 XML 文本里的实体还原（定义在本节末尾；`element_text` 要用）。
std::string xml_unescape(std::string const& s);

/// 元素内容的第一段文本（去掉标签与两端空白）。这四个字段都是纯文本；
/// 里面若冒出子元素，我们只取它前面的文本 —— 取不到就是空串（→ 拒绝）。
std::string element_text(std::string const& xml, Element const& e) {
  std::size_t end = e.content_end;
  std::size_t const lt = xml.find('<', e.content_begin);
  if (lt != std::string::npos && lt < end) end = lt;
  if (end < e.content_begin) return {};
  std::string s = xml.substr(e.content_begin, end - e.content_begin);
  std::size_t const a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return {};
  std::size_t const z = s.find_last_not_of(" \t\r\n");
  return xml_unescape(s.substr(a, z - a + 1));
}

/// 把 XML 文本里的实体还原。**口令/用户名里出现 `&`、`<` 很正常**，而
/// `header_xml` 是序列化后的文本（`&amp;` 这种），不还原就会拿着一个
/// 多了四个字符的口令去比，永远对不上。
bool append_utf8(std::string& out, unsigned long cp) {
  if (cp <= 0x7Ful) {
    out.push_back(static_cast<char>(cp));
  } else if (cp <= 0x7FFul) {
    out.push_back(static_cast<char>(0xC0u | (cp >> 6)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0xFFFFul) {
    out.push_back(static_cast<char>(0xE0u | (cp >> 12)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else if (cp <= 0x10FFFFul) {
    out.push_back(static_cast<char>(0xF0u | (cp >> 18)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu)));
    out.push_back(static_cast<char>(0x80u | (cp & 0x3Fu)));
  } else {
    return false;
  }
  return true;
}

std::string xml_unescape(std::string const& s) {
  std::string out;
  out.reserve(s.size());
  std::size_t i = 0;
  while (i < s.size()) {
    if (s[i] != '&') {
      out.push_back(s[i++]);
      continue;
    }
    std::size_t const semi = s.find(';', i + 1);
    // 一个实体名不可能很长。找不到/过长就把它当普通字符，而不是把后面
    // 一大段都吞掉。
    if (semi == std::string::npos || semi - i > 12) {
      out.push_back(s[i++]);
      continue;
    }
    std::string const ent = s.substr(i + 1, semi - i - 1);
    bool ok = true;
    if (ent == "amp") {
      out.push_back('&');
    } else if (ent == "lt") {
      out.push_back('<');
    } else if (ent == "gt") {
      out.push_back('>');
    } else if (ent == "quot") {
      out.push_back('"');
    } else if (ent == "apos") {
      out.push_back('\'');
    } else if (ent.size() > 1 && ent[0] == '#') {
      unsigned long cp = 0;
      bool hex = (ent[1] == 'x' || ent[1] == 'X');
      std::size_t k = hex ? 2 : 1;
      if (k >= ent.size()) {
        ok = false;
      } else {
        for (; k < ent.size(); ++k) {
          char const c = ent[k];
          int v = -1;
          if (c >= '0' && c <= '9') v = c - '0';
          else if (hex && c >= 'a' && c <= 'f') v = c - 'a' + 10;
          else if (hex && c >= 'A' && c <= 'F') v = c - 'A' + 10;
          if (v < 0) {
            ok = false;
            break;
          }
          cp = cp * (hex ? 16ul : 10ul) + static_cast<unsigned long>(v);
          if (cp > 0x10FFFFul) {
            ok = false;
            break;
          }
        }
        if (ok) ok = append_utf8(out, cp);
      }
    } else {
      ok = false;
    }
    if (!ok) {
      out.push_back(s[i++]);
      continue;
    }
    i = semi + 1;
  }
  return out;
}

/// 从标签里取一个属性（名字大小写敏感；`Type` 与少数客户端写的 `type` 都认）。
bool find_attr(std::string const& tag, std::string const& name,
               std::string& out) {
  if (name.empty()) return false;
  std::size_t i = 0;
  while ((i = tag.find(name, i)) != std::string::npos) {
    std::size_t const after_name = i + name.size();
    i = after_name;
    if (after_name >= tag.size()) return false;
    if (i == name.size()) continue;  // 标签开头不可能就是属性名
    char const prev = tag[i - name.size() - 1];
    if (!is_xml_space(prev)) continue;
    std::size_t p = after_name;
    while (p < tag.size() && is_xml_space(tag[p])) ++p;
    if (p >= tag.size() || tag[p] != '=') continue;
    ++p;
    while (p < tag.size() && is_xml_space(tag[p])) ++p;
    if (p >= tag.size() || (tag[p] != '"' && tag[p] != '\'')) continue;
    char const quote = tag[p++];
    std::size_t const end = tag.find(quote, p);
    if (end == std::string::npos) return false;
    out = xml_unescape(tag.substr(p, end - p));
    return true;
  }
  return false;
}

// ---------------------------------------------------------------------------
// UsernameToken
// ---------------------------------------------------------------------------

constexpr std::size_t kMaxUsernameBytes = 256;
constexpr std::size_t kMaxPasswordBytes = 1024;
constexpr std::size_t kMaxNonceB64Bytes = 512;
constexpr std::size_t kMaxCreatedBytes = 64;
constexpr std::size_t kMaxDecodedNonceBytes = 256;

enum class PasswordKind { kText, kDigest, kUnknown };

PasswordKind password_kind(std::string const& type) {
  // 规范里 `Type` 可省略，省略时的默认值是 PasswordText。
  if (type.empty()) return PasswordKind::kText;
  auto ends_with = [&type](char const* suffix) {
    std::size_t const n = std::strlen(suffix);
    return type.size() >= n && type.compare(type.size() - n, n, suffix) == 0;
  };
  if (ends_with(kWssePasswordDigestType) || type == "PasswordDigest") {
    return PasswordKind::kDigest;
  }
  if (ends_with(kWssePasswordTextType) || type == "PasswordText") {
    return PasswordKind::kText;
  }
  return PasswordKind::kUnknown;
}

struct UsernameToken {
  std::string username;
  std::string password;
  std::string nonce_b64;
  std::string created;
  std::string password_type;
};

/// 抽出 UsernameToken 的五个字段。
///
/// 返回 `kNotPresent` 表示"压根没有 Security/UsernameToken"；`kRejected`
/// 表示**带了**但字段不全或过长（此时 `check.reason` 已经写好）。
WsseStatus extract_token(std::string const& header_xml, UsernameToken& t,
                         WsseCheck& check) {
  if (header_xml.empty()) return WsseStatus::kNotPresent;
  if (header_xml.size() > kWsseHeaderLimitBytes) {
    check.reason = "SOAP Header 超过 " +
                   std::to_string(kWsseHeaderLimitBytes) + " 字节";
    return WsseStatus::kRejected;
  }

  Element security;
  if (!find_element(header_xml, "Security", 0, header_xml.size(), security)) {
    return WsseStatus::kNotPresent;
  }
  Element token;
  if (!find_element(header_xml, "UsernameToken", security.content_begin,
                    security.content_end, token)) {
    return WsseStatus::kNotPresent;
  }

  Element user;
  Element password;
  Element nonce;
  Element created;

  // 用户名先取出来：失败日志要能指认"是谁在试"。
  if (find_element(header_xml, "Username", token.content_begin,
                   token.content_end, user)) {
    t.username = element_text(header_xml, user);
  }
  if (t.username.size() > kMaxUsernameBytes) {
    t.username.clear();
    check.reason = "Username 过长";
    return WsseStatus::kRejected;
  }
  if (t.username.empty()) {
    check.reason = "缺少 Username";
    return WsseStatus::kRejected;
  }

  if (!find_element(header_xml, "Password", token.content_begin,
                    token.content_end, password)) {
    check.reason = "缺少 Password";
    return WsseStatus::kRejected;
  }
  t.password = element_text(header_xml, password);
  if (t.password.size() > kMaxPasswordBytes) {
    check.reason = "Password 过长";
    return WsseStatus::kRejected;
  }
  if (t.password.empty()) {
    check.reason = "Password 为空";
    return WsseStatus::kRejected;
  }
  std::string const password_tag =
      header_xml.substr(password.tag_begin, password.tag_end - password.tag_begin);
  if (!find_attr(password_tag, "Type", t.password_type) &&
      !find_attr(password_tag, "type", t.password_type)) {
    // 没有 Type 是合法的（默认 PasswordText）。
    t.password_type.clear();
  }

  if (!find_element(header_xml, "Nonce", token.content_begin,
                    token.content_end, nonce)) {
    check.reason = "缺少 Nonce";
    return WsseStatus::kRejected;
  }
  t.nonce_b64 = element_text(header_xml, nonce);
  if (t.nonce_b64.size() > kMaxNonceB64Bytes) {
    check.reason = "Nonce 过长";
    return WsseStatus::kRejected;
  }
  if (t.nonce_b64.empty()) {
    check.reason = "Nonce 为空";
    return WsseStatus::kRejected;
  }

  if (!find_element(header_xml, "Created", token.content_begin,
                    token.content_end, created)) {
    check.reason = "缺少 Created";
    return WsseStatus::kRejected;
  }
  t.created = element_text(header_xml, created);
  if (t.created.size() > kMaxCreatedBytes) {
    check.reason = "Created 过长";
    return WsseStatus::kRejected;
  }
  if (t.created.empty()) {
    check.reason = "Created 为空";
    return WsseStatus::kRejected;
  }
  return WsseStatus::kOk;
}

/// 日志用：把控制字符换掉并截断。用户名来自网络，原样打进一行日志的话，
/// 一个带 `\n` 的用户名就能伪造出第二条日志行。
std::string sanitize_for_log(std::string const& s) {
  std::string out;
  out.reserve(s.size() > 64 ? 67 : s.size());
  for (std::size_t i = 0; i < s.size() && i < 64; ++i) {
    unsigned char const c = static_cast<unsigned char>(s[i]);
    out.push_back((c < 0x20 || c == 0x7f) ? '?' : static_cast<char>(c));
  }
  if (s.size() > 64) out += "...";
  return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// 纯函数（可单独验）
// ---------------------------------------------------------------------------

bool wsse_parse_created_utc(std::string const& created,
                            std::chrono::system_clock::time_point& out) {
  // YYYY-MM-DDThh:mm:ss[.fff]Z
  if (created.size() < 20) return false;
  for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u,
                        11u, 12u, 14u, 15u, 17u, 18u}) {
    if (!is_digit(created[i])) return false;
  }
  if (created[4] != '-' || created[7] != '-' || created[10] != 'T' ||
      created[13] != ':' || created[16] != ':') {
    return false;
  }

  std::size_t p = 19;
  if (p < created.size() && created[p] == '.') {
    ++p;
    std::size_t const start = p;
    while (p < created.size() && is_digit(created[p])) ++p;
    // 至少一位，最多九位（纳秒）。真实客户端给 3 位（毫秒）。
    if (p == start || p - start > 9) return false;
  }
  if (p >= created.size() || (created[p] != 'Z' && created[p] != 'z')) {
    return false;
  }
  if (p + 1 != created.size()) return false;

  int const year = (created[0] - '0') * 1000 + (created[1] - '0') * 100 +
                   (created[2] - '0') * 10 + (created[3] - '0');
  int const month = two_digits(created, 5);
  int const day = two_digits(created, 8);
  int const hour = two_digits(created, 11);
  int const minute = two_digits(created, 14);
  int const second = two_digits(created, 17);

  if (year < 1970 || year > 9999) return false;
  if (month < 1 || month > 12) return false;
  static int const kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool const leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
  int const max_day = kDays[month - 1] + ((month == 2 && leap) ? 1 : 0);
  if (day < 1 || day > max_day) return false;
  if (hour > 23 || minute > 59 || second > 59) return false;

  // 小数秒直接截断：窗口是 ±5 分钟，亚秒不会把边界判反。
  std::int64_t const secs = days_from_civil(year, static_cast<unsigned>(month),
                                            static_cast<unsigned>(day)) *
                                86400 +
                            hour * 3600 + minute * 60 + second;
  out = std::chrono::system_clock::time_point(std::chrono::seconds(secs));
  return true;
}

bool wsse_base64_decode(std::string const& in, std::string& out) {
  std::string clean;
  clean.reserve(in.size());
  for (char const c : in) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    clean.push_back(c);
  }
  if (clean.empty() || clean.size() % 4 != 0) return false;

  std::string result;
  result.reserve(clean.size() / 4 * 3);
  for (std::size_t i = 0; i < clean.size(); i += 4) {
    bool const last = (i + 4 == clean.size());
    int const v0 = b64_value(clean[i]);
    int const v1 = b64_value(clean[i + 1]);
    if (v0 < 0 || v1 < 0) return false;

    int v2 = 0;
    int v3 = 0;
    int pad = 0;
    if (clean[i + 2] == '=') {
      if (!last || clean[i + 3] != '=') return false;
      pad = 2;
    } else {
      v2 = b64_value(clean[i + 2]);
      if (v2 < 0) return false;
      if (clean[i + 3] == '=') {
        if (!last) return false;
        pad = 1;
      } else {
        v3 = b64_value(clean[i + 3]);
        if (v3 < 0) return false;
      }
    }

    std::uint32_t const n = (static_cast<std::uint32_t>(v0) << 18) |
                            (static_cast<std::uint32_t>(v1) << 12) |
                            (static_cast<std::uint32_t>(v2) << 6) |
                            static_cast<std::uint32_t>(v3);
    result.push_back(static_cast<char>((n >> 16) & 0xFFu));
    if (pad < 2) result.push_back(static_cast<char>((n >> 8) & 0xFFu));
    if (pad < 1) result.push_back(static_cast<char>(n & 0xFFu));
  }
  out = std::move(result);
  return true;
}

std::string wsse_password_digest_b64(std::string const& nonce_b64,
                                     std::string const& created,
                                     std::string const& password) {
  std::string nonce;
  if (!wsse_base64_decode(nonce_b64, nonce)) return {};
  if (nonce.empty() || nonce.size() > kMaxDecodedNonceBytes) return {};
  unsigned char digest[20];
  if (!sha1_raw(nonce + created + password, digest)) return {};
  return b64_encode(digest, sizeof(digest));
}

// ---------------------------------------------------------------------------
// WsseVerifier
// ---------------------------------------------------------------------------

struct WsseVerifier::Impl {
  /// 重放缓存的一个条目。**key 只放 username + nonce**，不放摘要/口令。
  struct Entry {
    std::string key;
    std::chrono::system_clock::time_point expires;
  };

  /// 有界：最多 2048 条（约几百 KB）。满了先清过期的，再丢最旧的一条 ——
  /// 在暴力刷 nonce 的情况下宁可让最老的一条失去保护，也不让内存无限涨。
  static constexpr std::size_t kMaxEntries = 2048;
  /// 比授受窗口（±5 分钟）宽一倍：`Created` 可以来自未来 5 分钟，那一条
  /// 直到 `Created+5min`（= 现在 +10 分钟）都可能被重放。
  static constexpr int kReplayRetentionSeconds = 2 * kWsseClockSkewSeconds;

  mutable std::mutex mu;
  mutable std::deque<Entry> entries;

  /// 返回 true = 这个 (user, nonce) 在窗口里已经出现过（拒绝）。
  /// 否则登记并返回 false。
  bool seen_or_add(std::string const& key,
                   std::chrono::system_clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mu);
    while (!entries.empty() && entries.front().expires <= now) {
      entries.pop_front();
    }
    for (Entry const& e : entries) {
      if (e.key == key) return true;
    }
    while (entries.size() >= kMaxEntries) entries.pop_front();
    entries.push_back(Entry{
        key, now + std::chrono::seconds(kReplayRetentionSeconds)});
    return false;
  }
};

WsseVerifier::WsseVerifier() : impl_(std::make_unique<Impl>()) {}
WsseVerifier::~WsseVerifier() = default;

WsseCheck WsseVerifier::verify(std::string const& header_xml,
                               std::string const& expected_user,
                               std::string const& expected_password) const {
  return verify_at(header_xml, expected_user, expected_password,
                   std::chrono::system_clock::now());
}

WsseCheck WsseVerifier::verify_at(
    std::string const& header_xml, std::string const& expected_user,
    std::string const& expected_password,
    std::chrono::system_clock::time_point now) const {
  WsseCheck check;
  UsernameToken t;
  WsseStatus const parsed = extract_token(header_xml, t, check);
  // 用户名在"缺字段"这种失败里也拿得到 —— 日志要能说清是谁在试。
  check.username = sanitize_for_log(t.username);
  if (parsed != WsseStatus::kOk) {
    check.status = parsed;
    return check;
  }

  // ---- Created 的格式与窗口 ----
  std::chrono::system_clock::time_point created_at;
  if (!wsse_parse_created_utc(t.created, created_at)) {
    check.status = WsseStatus::kRejected;
    check.reason = "Created 不是 UTC ISO8601（应形如 2026-01-02T03:04:05.000Z）";
    return check;
  }
  std::int64_t const delta = unix_seconds(created_at) - unix_seconds(now);
  if (delta > kWsseClockSkewSeconds || delta < -kWsseClockSkewSeconds) {
    check.status = WsseStatus::kRejected;
    // **不把具体偏移、也不把 Created 写进日志**：它们是从凭据材料里推出来的，
    // 排障只需要知道"超窗了"。
    check.reason = "Created 不在 ±5 分钟窗口内";
    return check;
  }

  // ---- Nonce ----
  // 规范里它是 base64Binary。**解码一次，重放缓存用解码后的字节做 key**：
  // 否则同一个 nonce 换一种"非规范但解码结果相同"的 base64 写法就能绕过
  // 重放检测，而 PasswordDigest 仍然算得对（摘要用的正是解码后的字节）。
  std::string nonce_raw;
  if (!wsse_base64_decode(t.nonce_b64, nonce_raw)) {
    check.status = WsseStatus::kRejected;
    check.reason = "Nonce 不是合法的 Base64";
    return check;
  }
  if (nonce_raw.empty() || nonce_raw.size() > kMaxDecodedNonceBytes) {
    check.status = WsseStatus::kRejected;
    check.reason = "Nonce 长度不合法";
    return check;
  }

  // ---- 凭据 ----
  // 用户名也用常量时间比较：口令那一侧的时序不该被"用户名对不对"提前暴露。
  if (!secure_equals(t.username, expected_user)) {
    check.status = WsseStatus::kRejected;
    check.reason = "用户名不匹配";
    return check;
  }

  PasswordKind const kind = password_kind(t.password_type);
  if (kind == PasswordKind::kUnknown) {
    check.status = WsseStatus::kRejected;
    check.reason = "不支持的 Password Type";
    return check;
  }
  if (kind == PasswordKind::kText) {
    if (!secure_equals(t.password, expected_password)) {
      check.status = WsseStatus::kRejected;
      check.reason = "口令不匹配";
      return check;
    }
  } else {
    std::string const expected =
        wsse_password_digest_b64(t.nonce_b64, t.created, expected_password);
    if (expected.empty()) {
      // Nonce 与长度上面已经验过了，走到这里只剩"OpenSSL 里取不到 SHA1"。
      check.status = WsseStatus::kRejected;
      check.reason = "无法计算 SHA1（OpenSSL 不可用）";
      return check;
    }
    if (!secure_equals(t.password, expected)) {
      check.status = WsseStatus::kRejected;
      check.reason = "PasswordDigest 不匹配";
      return check;
    }
  }

  // ---- 重放 ----
  // **放在凭据校验之后**：否则一个不知道口令的人可以拿受害者的用户名 +
  // 一个猜出来的 nonce 先把缓存里的那一格占掉，让合法请求被当成重放。
  // key 用**解码后**的 nonce 字节（理由见上面 Nonce 那一段）。
  std::string key = t.username;
  key.push_back('\x1f');
  key += nonce_raw;
  if (impl_->seen_or_add(key, now)) {
    check.status = WsseStatus::kRejected;
    check.reason = "Nonce 重放（同一用户名 + nonce 在窗口内出现过）";
    return check;
  }

  check.status = WsseStatus::kOk;
  check.reason.clear();
  return check;
}

}  // namespace d2o
