#pragma once

/**
 * @file src/onvif/wss.h
 * @brief WS-Security UsernameToken（只做 ONVIF 用得到的那一档）。
 *
 * ONVIF 的 SOAP 端点同时接受 HTTP Digest 和 WS-UsernameToken：前者是
 * NVR 添加通道时最常用的一套，后者出现在一部分客户端/网关里。两边**并存**，
 * 任一通过即放行。
 *
 * ## 只实现了 UsernameToken，而且是其中两种口令形态
 *
 *   * `PasswordText`   —— `Password` 里就是明文口令；
 *   * `PasswordDigest` —— `Base64(SHA1(Base64Decode(Nonce) + Created + Password))`。
 *
 * 定时比较的 `Created` 必须是 UTC ISO8601（允许毫秒），窗口 ±5 分钟。
 *
 * 别的 WS-Security 机制（签名、加密、SAML、Kerberos、X.509）**一概不碰**。
 * 这不是"暂时没做"：ONVIF Profile S 的设备服务不要求它们，而收下一个看不懂
 * 的安全头然后放行，比明确拒绝更糟。
 *
 * ## 为什么 XML 用字符串定位，而不是再引一个 XML 库
 *
 * `header_xml` 来自 `uvcpp_soap_parse`，**已经是良构 XML 的序列化结果**：
 * 实体、递归深度、节点数这些风险都在那一层挡过了（它有 max_depth / max_nodes）。
 * 这里要取的只是 `<Security><UsernameToken>` 下面四个标量，按局部名定位即可。
 * 另外 SOAP Header 是**很小**的一段，`verify()` 自己也有一道上限（见
 * `kWsseHeaderLimitBytes`），不会把一整份大报文当 header 来扫。
 *
 * ## 安全日志
 *
 * 这个模块**一行日志都不打**。失败原因通过 `WsseCheck::reason` 返回，由
 * 调用方（`OnvifService::handle`）连同来源地址、用户名一起记 —— 而 nonce、
 * Created、摘要、口令**永远不进日志**。拆开还有一个好处：wss.cpp 不依赖
 * `common/log.h`，这个文件里的纯函数可以单独拿来验。
 */

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>

namespace d2o {

/// `verify()` 的三种结局。**"没带"和"带了但不对"必须分开**：
/// 前者要落回原有的 Digest 401 流程，后者也要 401，但日志要指出是 WSSE 失败。
enum class WsseStatus {
  kNotPresent,  ///< Header 里没有 `<Security>/<UsernameToken>`
  kOk,          ///< 凭据校验通过（且 nonce 不在重放窗口里）
  kRejected,    ///< 带了 UsernameToken，但缺字段/超窗/口令或摘要不符/重放
};

struct WsseCheck {
  WsseStatus status = WsseStatus::kNotPresent;
  /// 报文里的用户名，**仅供日志**（已截断到 64 字节并把控制字符换成 `?`）。
  /// 被拒时也可能有值。
  std::string username;
  /// 一句简短原因，**不含任何凭据材料**（nonce/Created/摘要/口令）。
  std::string reason;
};

/// `wsse:Password/@Type` 里两种形态的 URI 尾部。留空按规范默认成 PasswordText。
constexpr char kWssePasswordTextType[] = "#PasswordText";
constexpr char kWssePasswordDigestType[] = "#PasswordDigest";

/// Created 的允许偏移：±5 分钟。
constexpr int kWsseClockSkewSeconds = 5 * 60;

/// `header_xml` 的字节上限。正常的一条 Security 头是几百字节；64 KB 已经
/// 比任何真实客户端都宽，同时挡住"把整个 Body 塞进 Header"这种形状。
constexpr std::size_t kWsseHeaderLimitBytes = 64u * 1024u;

/// 解析 `YYYY-MM-DDThh:mm:ss[.fff]Z`（UTC，毫秒可选）。成功时 `out` 是
/// 那一秒对应的 UTC 时刻（小数部分截断 —— 窗口是 ±5 分钟，亚秒无关）。
bool wsse_parse_created_utc(std::string const& created,
                            std::chrono::system_clock::time_point& out);

/// 严格的 Base64 解码（标准字母表，必须有正确填充；容忍空白字符）。
/// 失败返回 false 且 `out` 不被使用。
bool wsse_base64_decode(std::string const& in, std::string& out);

/// `Base64(SHA1(Base64Decode(nonce_b64) + created + password))`。
/// 失败（nonce 不是 base64、SHA1 取不到）时返回空串。
std::string wsse_password_digest_b64(std::string const& nonce_b64,
                                     std::string const& created,
                                     std::string const& password);

/// 一个长期存活的 UsernameToken 校验器。
///
/// **nonce 重放缓存挂在它身上**，所以 Instances 必须像 `DigestAuth` 一样
/// 进程内只建一个（见 `OnvifService::Impl::wsse_verifier()`）。按请求新建
/// 等于把重放保护清零。
///
/// 凭据（用户名/口令）**不存进来**，每次 `verify` 由调用方传入 —— 这样
/// 热改口令时不存在"校验器里还是旧口令"的第二份状态。重放缓存不随口令
/// 变化清空：旧 nonce 在新口令下本来就过不了摘要校验。
class WsseVerifier {
 public:
  WsseVerifier();
  ~WsseVerifier();
  WsseVerifier(WsseVerifier const&) = delete;
  WsseVerifier& operator=(WsseVerifier const&) = delete;

  /// 用系统时钟校验一次。`header_xml` 是 `uvcpp_soap_message::header_xml`。
  WsseCheck verify(std::string const& header_xml,
                   std::string const& expected_user,
                   std::string const& expected_password) const;

  /// 同上，但时钟由调用方给。**测试用** —— 时间窗口的边界值不该靠
  /// `sleep(300)` 去碰。
  WsseCheck verify_at(std::string const& header_xml,
                      std::string const& expected_user,
                      std::string const& expected_password,
                      std::chrono::system_clock::time_point now) const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
