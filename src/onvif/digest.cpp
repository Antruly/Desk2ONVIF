#include "onvif/digest.h"

#include "common/log.h"

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

namespace d2o {
namespace {

std::uint64_t now_seconds() {
  auto const t = std::chrono::system_clock::now().time_since_epoch();
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::seconds>(t).count());
}

std::string hex32(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "%08x", v);
  return std::string(buf);
}

bool iequal(char a, char b) {
  return std::tolower(static_cast<unsigned char>(a)) ==
         std::tolower(static_cast<unsigned char>(b));
}

/// 跳过空白。逗号由调用方负责跳。
std::size_t skip_ws(std::string const& s, std::size_t i) {
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  return i;
}

/// 值可能是 quoted-string（带反斜杠转义），也可能是裸 token。
std::string read_value(std::string const& s, std::size_t& i) {
  std::string out;
  if (i < s.size() && s[i] == '"') {
    ++i;
    while (i < s.size() && s[i] != '"') {
      if (s[i] == '\\' && i + 1 < s.size()) ++i;  // 转义：原样取下一个字符
      out.push_back(s[i]);
      ++i;
    }
    if (i < s.size()) ++i;  // 吃掉收尾的引号（没找到就是畸形，也照样返回）
  } else {
    while (i < s.size() && s[i] != ',' && !std::isspace(static_cast<unsigned char>(s[i]))) {
      out.push_back(s[i]);
      ++i;
    }
  }
  return out;
}

}  // namespace

std::string md5_hex(std::string const& data) {
  // OpenSSL 3 把 MD5() 这类一次性函数标了 deprecated，走 EVP 才没有警告。
  // fetch 返回的 EVP_MD 要还回去，否则每次调用漏一小块。
  EVP_MD* md = EVP_MD_fetch(nullptr, "MD5", nullptr);
  if (md == nullptr) {
    D2O_ERROR("OpenSSL 里取不到 MD5（EVP_MD_fetch 返回空）。Digest 鉴权无法工作。");
    return {};
  }

  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int len = 0;
  char const* p = data.empty() ? "" : data.data();
  int const ok = EVP_Digest(p, data.size(), digest, &len, md, nullptr);
  EVP_MD_free(md);
  if (ok != 1) return {};

  static char const kHex[] = "0123456789abcdef";
  std::string out(len * 2, '\0');
  for (unsigned int i = 0; i < len; ++i) {
    out[2 * i] = kHex[digest[i] >> 4];
    out[2 * i + 1] = kHex[digest[i] & 0x0F];
  }
  return out;
}

bool secure_equals(std::string const& a, std::string const& b) {
  if (a.size() != b.size()) return false;
  if (a.empty()) return true;
  return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

DigestResponse parse_authorization(std::string const& header) {
  DigestResponse r;

  std::size_t i = skip_ws(header, 0);
  // 认证方案名大小写不敏感（RFC 7235）。别的地方（字段名）同理。
  static char const kScheme[] = "Digest";
  if (header.size() - i >= 6) {
    bool match = true;
    for (std::size_t k = 0; k < 6; ++k) {
      if (!iequal(header[i + k], kScheme[k])) {
        match = false;
        break;
      }
    }
    if (match) i += 6;
  }

  std::size_t guard = 0;
  while (i < header.size() && guard++ < 64) {
    i = skip_ws(header, i);
    if (i < header.size() && header[i] == ',') {
      ++i;
      continue;
    }
    if (i >= header.size()) break;

    // 键
    std::size_t const key_begin = i;
    while (i < header.size() && header[i] != '=' && header[i] != ',') ++i;
    std::string key = header.substr(key_begin, i - key_begin);
    while (!key.empty() && std::isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
    if (i >= header.size() || header[i] != '=') {
      // 没有 `=` 的垃圾段：跳过，别把后面全带歪
      continue;
    }
    ++i;  // '='
    i = skip_ws(header, i);

    std::string const value = read_value(header, i);

    std::string lower;
    lower.reserve(key.size());
    for (char c : key) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "username") r.username = value;
    else if (lower == "realm") r.realm = value;
    else if (lower == "nonce") r.nonce = value;
    else if (lower == "uri") r.uri = value;
    else if (lower == "response") r.response = value;
    else if (lower == "algorithm") r.algorithm = value.empty() ? "MD5" : value;
    else if (lower == "qop") r.qop = value;
    else if (lower == "nc") r.nc = value;
    else if (lower == "cnonce") r.cnonce = value;
    else if (lower == "opaque") r.opaque = value;
    // 其余（userhash、charset 之类）忽略
  }

  r.present = !r.username.empty() && !r.nonce.empty() && !r.uri.empty() &&
              !r.response.empty();
  return r;
}

std::string make_www_authenticate(DigestChallenge const& c, bool stale) {
  std::string out = "Digest realm=\"" + c.realm + "\", ";
  out += "nonce=\"" + c.nonce + "\", ";
  out += "algorithm=" + (c.algorithm.empty() ? std::string("MD5") : c.algorithm);
  if (!c.opaque.empty()) out += ", opaque=\"" + c.opaque + "\"";
  if (!c.qop.empty()) out += ", qop=\"" + c.qop + "\"";
  // stale 只该在"nonce 过期了、口令是对的"时出现。它告诉客户端**别重新问用户
  // 要口令**，拿新 nonce 直接重算 —— 少了它，某些客户端会再弹一次登录框。
  if (stale) out += ", stale=true";
  return out;
}

std::string digest_response_hex(std::string const& method, std::string const& uri,
                                std::string const& username, std::string const& realm,
                                std::string const& password, std::string const& nonce,
                                std::string const& qop, std::string const& nc,
                                std::string const& cnonce) {
  std::string const ha1 = md5_hex(username + ":" + realm + ":" + password);
  std::string const ha2 = md5_hex(method + ":" + uri);
  if (ha1.empty() || ha2.empty()) return {};
  if (qop.empty()) return md5_hex(ha1 + ":" + nonce + ":" + ha2);
  return md5_hex(ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2);
}

DigestAuth::DigestAuth(std::string realm, std::string username, std::string password,
                       std::string opaque_seed)
    : realm_(std::move(realm)),
      username_(std::move(username)),
      password_(std::move(password)) {
  if (!opaque_seed.empty()) {
    secret_ = std::move(opaque_seed);
    return;
  }
  // 每进程一份的随机密钥。有了它 nonce 可以无状态地验真伪 —— 不用维护
  // 一张 nonce 表，也就不用为它加锁、不用担心它无限增长。
  std::random_device rd;
  std::mt19937_64 gen(rd());
  std::uniform_int_distribution<std::uint64_t> dist;
  secret_ = std::to_string(dist(gen)) + "-" + std::to_string(dist(gen));
}

std::string DigestAuth::make_nonce() const {
  std::string const ts = hex32(static_cast<std::uint32_t>(now_seconds()));
  return ts + md5_hex(ts + secret_).substr(0, 16);
}

bool DigestAuth::nonce_ok(std::string const& nonce, bool& stale_out) const {
  stale_out = false;
  if (nonce.size() != 24) return false;

  std::string const ts = nonce.substr(0, 8);
  std::string const mac = nonce.substr(8);
  std::string const want = md5_hex(ts + secret_).substr(0, 16);
  if (want.empty() || !secure_equals(mac, want)) return false;  // 不是我们发的

  auto const issued = static_cast<std::uint64_t>(std::strtoul(ts.c_str(), nullptr, 16));
  // 取模运算让 2106 年的时间戳回绕不会把"很旧"误判成"刚从未来来的"。
  //
  // 判据是 `age >= ttl` 而不是 `>`：age 是**整秒**，所以 `age >= 3600` 的含义
  // 正好是"签发满 3600 秒了"，也就是"有效期为 3600 秒"。写成 `>` 的话每个
  // nonce 都多活一秒，而且 `ttl=0`（"立刻过期"）会变成一个**永远不过期**的
  // 设置 —— 边界上正好反过来，是自检里那项先抓出来的。
  std::uint64_t const age = (now_seconds() - issued) & 0xFFFFFFFFull;
  if (age >= nonce_ttl_s_) {
    stale_out = true;
    return false;
  }
  return true;
}

std::string DigestAuth::challenge(bool stale) const {
  DigestChallenge c;
  c.realm = realm_;
  c.nonce = make_nonce();
  c.opaque = md5_hex(realm_ + secret_).substr(0, 16);
  return make_www_authenticate(c, stale);
}

bool DigestAuth::verify(std::string const& method, std::string const& path,
                        std::string const& full_url, std::string const& auth_header,
                        bool& stale_out) const {
  stale_out = false;
  if (auth_header.empty()) return false;

  DigestResponse const r = parse_authorization(auth_header);
  if (!r.present) return false;
  if (r.username != username_) return false;
  if (!r.realm.empty() && r.realm != realm_) return false;

  std::string alg = r.algorithm;
  if (alg.empty()) alg = "MD5";
  if (alg != "MD5" && alg != "md5") {
    D2O_WARN("Digest 用了不支持的 algorithm=%s（只支持 MD5），拒绝。", alg.c_str());
    return false;
  }

  if (!nonce_ok(r.nonce, stale_out)) return false;

  // uri 的两难：RFC 说摘要里写请求行里的那个，但真实客户端两种都写过 ——
  // 只写路径，或者写完整 URL。先按它给的原样算，再判断这个 uri 是否
  // "确实指着我们收到的这个请求"。
  bool const uri_ok = (r.uri == path) || (r.uri == full_url) ||
                      (!full_url.empty() && full_url.size() > r.uri.size() &&
                       full_url.compare(full_url.size() - r.uri.size(), r.uri.size(),
                                        r.uri) == 0);
  if (!uri_ok) return false;

  if (!r.qop.empty() && r.qop != "auth") {
    D2O_WARN("Digest 用了不支持的 qop=%s（只支持 auth），拒绝。", r.qop.c_str());
    return false;
  }
  if (r.qop == "auth" && (r.nc.empty() || r.cnonce.empty())) return false;

  // 注意 uri 用的是**客户端写在头里的那个**，不是我们收到的路径 —— 摘要本身
  // 必须按客户端所写去算，否则永远对不上。这个 uri 是否指向本次请求，已经在
  // 上面那道 uri_ok 里单独判过了。
  std::string const expect = digest_response_hex(method, r.uri, username_, realm_,
                                                 password_, r.nonce, r.qop, r.nc,
                                                 r.cnonce);
  if (expect.empty()) return false;
  return secure_equals(r.response, expect);
}

}  // namespace d2o
