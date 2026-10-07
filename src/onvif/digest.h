#pragma once

/**
 * @file src/onvif/digest.h
 * @brief HTTP Digest 鉴权（RFC 2617 / 7616），只实现 ONVIF 实际用到的那一档。
 *
 * 为什么用 OpenSSL 的 EVP 而不是自己写那 100 行 MD5 轮函数：项目已经因为
 * Live555 的 TLS/SRTP 静态链进了 OpenSSL，MD5 是白拿的。自己写一份不共享
 * 任何测试向量，出错了还只有"某个 NVR 连不上"这一个症状。
 *
 * **只支持 qop=auth + MD5**，这是 ONVIF Profile S 设备服务的实际取值：
 *   - `qop=auth-int`（要 MD5-sess 之外的实体摘要）在摄像机里基本不出现；
 *   - `algorithm=MD5-sess` 同理。
 * 遇到这些取值直接判失败并记日志 —— 比"算出一个错的摘要然后 401"好排查。
 *
 * ## 与 NVR 打交道的两个现实问题
 *
 * 1. **uri 写什么**：RFC 说摘要里的 uri 就是请求行里的那个。但真实客户端里
 *    有的填 `/onvif/device_service`，有的填完整 URL
 *    `http://192.168.1.5/onvif/device_service`。两种都见过，所以校验时**两种
 *    都试**，只要有一种对得上就算过。
 *
 * 2. **nonce 的生命周期**：NVR 会拿着同一个 nonce 连着发很多请求（每次把 nc
 *    加一）。所以 nonce 不能设太短，否则长连接跑到一半开始 401，而客户端未必
 *    会重新挑战。这里给 1 小时，过期时按 RFC 回 `stale=true`，让守规矩的
 *    客户端**不再弹框、直接重试**。
 *
 * ## 已知取舍（诚实说明）
 *
 * 不跟踪 `nc`，所以同一个 nonce 的请求理论上可以在有效期内被重放。对一个
 * 跑在局域网里的摄像机模拟器，这个代价换的是"完全无状态的鉴权"—— 不引入
 * 一张会随时间膨胀、还要在多个 loop 线程间加锁的 nonce 表。真要防重放，
 * 就在 `DigestAuth` 里加一张 `nonce -> max_nc` 的表，签名不用变。
 */

#include <cstdint>
#include <string>

namespace d2o {

/// 小写十六进制的 MD5。
std::string md5_hex(std::string const& data);

/// 常量时间比较，用来比对摘要。普通 `==` 会在第一个不同的字节返回，
/// 理论上可以被计时攻击逐字节试出来 —— 这里没有明显必要，但成本是零。
bool secure_equals(std::string const& a, std::string const& b);

/// 服务端发出的挑战，对应 `WWW-Authenticate: Digest ...`。
struct DigestChallenge {
  std::string realm;
  std::string nonce;
  std::string opaque;
  std::string qop = "auth";  ///< 只有 auth 会被接受
  std::string algorithm = "MD5";
};

/// 客户端 `Authorization: Digest ...` 里解析出来的字段。
struct DigestResponse {
  bool present = false;  ///< 头存在且能解析出最少必需字段
  std::string username;
  std::string realm;
  std::string nonce;
  std::string uri;
  std::string response;  ///< 十六进制摘要
  std::string algorithm = "MD5";
  std::string qop;       ///< 空 = 客户端没用 qop（老设备有这种）
  std::string nc;
  std::string cnonce;
  std::string opaque;
};

/// 解析 `Authorization` 头的值。输入可以是带 `Digest ` 前缀的完整值，
/// 也可以是不带前缀的参数串。
DigestResponse parse_authorization(std::string const& header);

/// 组装 `WWW-Authenticate` 的值。stale=true 时带上 `stale=true`。
std::string make_www_authenticate(DigestChallenge const& c, bool stale);

/// 按 RFC 2617 §3.2.2 算一次客户端摘要（十六进制）。
///
/// 单独抽出来是为了**能用标准里的测试向量验它**。如果它只被 `DigestAuth::verify`
/// 调用，那么唯一能测的就是"服务端和自己算得一样" —— 两边一起错的时候这个性质
/// 照样成立，而症状只有"某个 NVR 连不上"。
///
/// qop 为空串时走 RFC 2069 的老式算法（`H(HA1:nonce:HA2)`），否则按 qop=auth
/// 的六段式。`nc`/`cnonce` 只在 qop 非空时有意义。
std::string digest_response_hex(std::string const& method, std::string const& uri,
                                std::string const& username, std::string const& realm,
                                std::string const& password, std::string const& nonce,
                                std::string const& qop, std::string const& nc,
                                std::string const& cnonce);

/// 一个用户名/口令的 Digest 校验器。构造一次，之后只读，可多线程共用。
class DigestAuth {
 public:
  /// **空口令不是"不鉴权"，是"口令为空"。** 挑战照发、`HA1` 照算
  /// （`md5(user:realm:)`），所以任何知道用户名的人用空口令就能过 ——
  /// 结果和"无鉴权"一样，但从外面看它有一整套鉴权流程。
  ///
  /// 这个类**分不出**"调用方是故意要空口令"还是"忘了传口令"，所以这道
  /// 判断不放在这里，放在调用点上：`OnvifService` 用 `opt.password` 构造，
  /// 而 `opt.password` 的默认值在 `config.h`（`admin`，非空），命令行和配置
  /// 两条路都走得到，main.cpp 里对"最终生效口令为空"打一条警告。
  /// （这里原本写着"必须用 `allow_empty_password` 显式说明"，但签名里从来没有
  /// 这个参数 —— 注释描述了一个不存在的防护，比没有注释更糟。）
  DigestAuth(std::string realm, std::string username, std::string password,
             std::string opaque_seed = std::string());

  /// 发一个新的挑战。stale=true 表示"你上次那个 nonce 过期了，用新的重试"。
  std::string challenge(bool stale = false) const;

  /// 校验一次请求。
  ///
  /// @param method    HTTP 方法，大写（`GET` / `POST`）
  /// @param path      请求行里的路径（不含查询串），如 `/onvif/device_service`
  /// @param full_url  完整 URL；摘要里的 uri 对不上 path 时拿它再试一次
  /// @param auth_header  `Authorization` 头的值（可以是空串）
  /// @param stale_out 出参：失败是因为 nonce 过期时为 true
  bool verify(std::string const& method, std::string const& path,
              std::string const& full_url, std::string const& auth_header,
              bool& stale_out) const;

  std::string const& realm() const { return realm_; }
  std::string const& username() const { return username_; }

  /// nonce 有效期（秒）。测试和调优用。
  void set_nonce_ttl(std::uint64_t seconds) { nonce_ttl_s_ = seconds; }

 private:
  std::string make_nonce() const;
  /// 校验 nonce 的完整性与新鲜度。返回 false 时 stale_out 说明是不是"过期"。
  bool nonce_ok(std::string const& nonce, bool& stale_out) const;

  std::string realm_;
  std::string username_;
  std::string password_;
  /// 随机构造的 nonce 密钥。有了它，nonce 不必存表也能验真伪。
  ///
  /// **代价是这个对象必须长期存活。** nonce 里没有别的可验真伪的东西，密钥
  /// 一换，先前发出去的 nonce 全部作废。曾经按请求新建 `DigestAuth`，于是
  /// 每个挑战都由一个只活一次的对象签发、又被下一个对象拒收 —— 现象是
  /// 挑战完全合规、客户端算出的摘要也完全正确，服务端却一律 401，看上去
  /// 像摘要算法写反了。`tests/onvif_soap.py` 的 [2]→[3] 两步专门盯这个：
  /// 它跨**两个 HTTP 请求**取挑战再应答，单实例的单元测试测不出来。
  std::string secret_;
  std::uint64_t nonce_ttl_s_ = 3600;
};

}  // namespace d2o
