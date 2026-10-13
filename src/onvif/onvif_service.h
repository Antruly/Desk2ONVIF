#pragma once

/**
 * @file src/onvif/onvif_service.h
 * @brief ONVIF 的 SOAP 服务：设备服务 + 媒体服务。
 *
 * ## 为什么响应是手写信封，而不是 `uvcpp_soap_dump_response`
 *
 * 计划里原本打算用 libuvcpp 的信封层生成响应。实际做的时候发现那样做
 * **一定会产出错误命名空间的响应**，理由如下（已核对源码
 * `wsdl/uvcpp_soap_message.cpp:556` 的 `response_wrapper_block`）：
 *
 * 它只做两件事 —— 在包装元素上写一个 `xmlns="<op_ns>"`，然后把 `inner_xml`
 * **原样**塞进去。它不会扫描 `inner_xml` 里用到了哪些前缀。
 *
 * 而 ONVIF 的响应恰恰是**跨命名空间**的。以 `GetProfiles` 为例：
 *
 *     <trt:GetProfilesResponse>          ← media wsdl
 *       <trt:Profiles token="main">      ← media wsdl
 *         <tt:Name>main</tt:Name>        ← onvif schema！换命名空间了
 *
 * 包装元素上那个 `xmlns="…/media/wsdl"` 是个**默认**命名空间，会被所有不带
 * 前缀的后代继承。于是"内层不带前缀"这条规避办法在这里不但没用，还是错的：
 * `<Name>` 会落进 media wsdl，而 `tt:` 前缀又从未声明 —— 严格解析器直接拒收
 * 整条响应。
 *
 * 补救办法有两种，都不好：把 `xmlns:tt` 重复声明在**每一个**顶层子元素上
 * （声明的作用域是该元素及其后代，管不到兄弟），或者放弃包装元素自己去拼。
 * 后者只有十几行，而且**所有前缀只在 Envelope 上声明一次**就全局有效 ——
 * 于是这里选了后者。
 *
 * **解析仍然走 `uvcpp_soap_parse`。** 那才是这个库真正值钱的地方：报文来自
 * 网络，可能是恶意的，而它带了深度/节点数上限和良好的失败语义。生成那一侧
 * 的输入全是我们自己的常量，没有这类风险。
 */

#include "common/audit_log.h"
#include "common/config.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uvcpp {
class uvcpp_web_request;
class uvcpp_web_response;
}  // namespace uvcpp

namespace d2o {

class DeviceState;

/// 一路码流。ONVIF 的 Profile 就是"视频源 + 编码配置"的一套绑定。
struct OnvifStreamProfile {
  /// NVR 拿它来引用这一路（GetStreamUri 的 ProfileToken）。**不要带空格**
  /// —— 有些 NVR 会把它原样拼进 URL 查询串而不做转义。
  std::string token = "main";
  std::string name = "main";
  int width = 1920;
  int height = 1080;
  int fps = 30;
  int bitrate_kbps = 4000;
  int gop_seconds = 2;
  int quality = 4;
  std::string encoding = "H264";
  /// `tt:H264Profile` 的取值：`Baseline` / `Main` / `High` / `Extended`。
  /// **必须和编码器实际产出的档位一致** —— 有 NVR 会拿它决定要不要开硬件
  /// 解码通路，报高了会被拒。注意 `preset=ultrafast` 会强制降到
  /// Constrained Baseline（见 README 里那条），那种情况下应当报 `Baseline`。
  std::string h264_profile = "Baseline";
  /// RTSP 的路径部分（如 `/live`）。主机和端口由 OnvifServiceOptions 提供。
  std::string rtsp_path = "/live";
};

/// ONVIF 的 `Set*` 这一次想改的那几项。
///
/// **每一项都是"可以没有"** —— `Set*` 的报文体里只带客户端想改的字段，没提到的
/// 一律不该被碰。用 `std::optional` 而不是 `has_xxx` 布尔加裸值：后者是两个变量
/// 说同一件事，迟早有一个忘了同步，而那种错的表现是"NVR 只是改了个名字，帧率被
/// 打回了默认值"。
///
/// **没有 `width`**：配置层只认高度（`capture.max_height` 是上限，`rtsp.sub_height`
/// 就是高度），宽度一律按主码流的宽高比推 —— 见 `config.h` 里那两个字段的注释。
/// 让配置去迁就一个会变形的 `Width` 会在 16:10、21:9 这些比例上把画面拉扁。
struct OnvifDevicePatch {
  /// 摄像头名称。NVR 改名走的就是这里（另外两个入口是启动和网页）。
  std::optional<std::string> name;

  /// 主码流。
  std::optional<int> resolution_height;
  std::optional<int> fps;
  std::optional<int> gop_seconds;
  /// `capture.profile` 的线值（小写）：`baseline` / `main` / `high`。
  std::optional<std::string> h264_profile;
  std::optional<int> bitrate_kbps;

  /// 子码流。
  std::optional<int> sub_height;
  std::optional<int> sub_bitrate_kbps;

  /// 一项都没有 = 这一次 `Set*` 只带了我们认不出来的东西。
  bool empty() const {
    return !name && !resolution_height && !fps && !gop_seconds &&
           !h264_profile && !bitrate_kbps && !sub_height && !sub_bitrate_kbps;
  }
};

struct OnvifServiceOptions {
  /// `urn:uuid:...`。**必须跨重启稳定**，否则 NVR 会重复添加同一台设备。
  std::string uuid;
  std::string manufacturer = "Desk2ONVIF";
  std::string model = "Desk2ONVIF Virtual Camera";
  std::string firmware_version = "1.0.0";
  std::string hardware_id = "1.0";
  std::string serial_number;
  std::string device_name = "Desk2ONVIF";
  std::string location = "Desktop";

  /// RTSP 对外的端口。主机**不在这里** —— 它要按每条请求的来源地址反推
  /// （多网卡机器上写死一个地址，NVR 拿到的就是连不上的那个）。
  int rtsp_port = 554;
  std::vector<OnvifStreamProfile> profiles;

  /// Digest 凭据。`GetSystemDateAndTime` 之外的操作都要过这一关。
  std::string user = "admin";
  std::string password;
  std::string realm = "Desk2ONVIF";

  /// 快照 URL 用的 HTTP 端口。
  int http_port = 8080;
  std::string snapshot_path = "/snapshot.jpg";

  /// 设备时间：返回 `UTCDateTime` 还是 `LocalDateTime+TimeZone`。
  /// Windows 上 `localtime_s` 拿得到时区偏移，所以默认两者都给 —— NVR 的
  /// 时间显示依赖它，而只给 UTC 时有些 NVR 会按本地时间直读，差 8 小时。
  bool prefer_local_time = true;

  /// ONVIF 的 `Set*` 真的改动了设备配置时调用。**在循环线程上**（SOAP 处理器
  /// 就在那条线程），实现里可以直接改内存快照，不必加锁。空 = 只改内存里的
  /// 设备状态、不落盘（单跑录制时就是这样）。
  ///
  /// ## 为什么由调用方落盘，而不是 `OnvifService` 自己写
  ///
  /// 写盘要处理"以哪一份为底、要不要 `validate_config`、热字段推不推、`pending`
  /// 怎么重算"这一整套 —— 那套东西的权威副本在 `main.cpp`（它还管着命令行覆盖
  /// 和持久化路径）。`OnvifService` 连 `AppConfig` 都不该认全。
  ///
  /// ## 返回 void 是**故意的**
  ///
  /// 落盘失败时 SOAP 那边也不回 Fault：`Set*` 没有"稍后生效"的语义，回 Fault
  /// 会让 NVR 的通道添加流程当场失败（海康会停在"配置编码器"那一步，用户看到
  /// 的是"加不上"而不是"存不上"）。所以实现失败时必须自己打 ERROR 日志 ——
  /// 那是唯一还能被人看见的地方。
  std::function<void(OnvifDevicePatch const&)> on_patch;

  /// 水印（OSD）配置的**权威副本**。
  ///
  /// ## 为什么这一份可以整个放在服务里，`profiles` 和身份却不行
  ///
  /// `profiles` 的分辨率被采集侧每秒一次地上报改写（`set_video_size`），身份
  /// 那一份被三个入口写 —— 那两个的权威都在别处，这里只能跟着同步。而水印是
  /// **纯数据**：它只在循环线程上被读写（SOAP 请求就在那条线程），既不碰编码器
  /// 也不碰 socket。所以这里持有一份拷贝没有一致性问题，`GetOSDs` 直接答它。
  ///
  /// **这里持的是权威副本，画面上那份是它的影子。** 水印现在是**热生效**的：
  /// `on_osd_patch` 会把改动推进 `LiveOsd`，采集线程每帧取当期值，所以这里改完
  /// **画面下一帧就变**，既不重建管线，也不进 `pending`。
  ///
  /// ⚠️ 这句话在本轮之前写的是反的 —— "水印用的是启动时那一份（`SessionOptions::osd`），
  /// 所以改了之后要重建一次管线才变，那部分变化如实进 `pending`"。**它当时是对的**
  /// （`apply_osd_config` 恒返回 false，那段设计是刻意的），但热生效落地之后它就成了
  /// 一句误导：下一个人会按它去解释一个不存在的延迟，或者更糟 —— 按它把热生效
  /// "修"回去。这类"过期但当时正确"的注释，危害与写错的注释一样大。
  ///
  /// `SessionOptions::osd` 那个值字段**仍然在**，角色变成了兜底：手搓
  /// `SessionOptions` 的测试与 harness 不设 `live_osd`，走的就是那一份。
  OsdConfig osd;

  /// 水印被 `SetOSD` / `CreateOSD` / `DeleteOSD` 改动时调用。**在循环线程上**，
  /// 收到的是改动后的**整份** `OsdConfig`。
  ///
  /// 为什么给整份，而不是像 `on_patch` 那样给一个 patch 结构：水印是一棵纯数据
  /// 的树，没有"部分应用"这回事 —— 实现里要做的就是把 `cfg.osd` 换成它、存盘、
  /// 重算 `pending`。给 patch 只会让"把改动合并进去"这段逻辑有两个副本，而两份
  /// 迟早不一致，不一致的那一份一定是后加字段时漏掉的那份。
  ///
  /// 失败时同样**不回 Fault**（理由见上面 `on_patch` 那段），实现里自己打
  /// ERROR 日志。
  std::function<void(OsdConfig const&)> on_osd_patch;

  /// 当前的时间模式：`"manual"` 或 `"ntp"`。留空 = 按 `"manual"`。
  ///
  /// ## 为什么是回调，而不是一个 `std::string` 成员
  ///
  /// 因为**这个值在运行中会变** —— 用户在网页上把模式从手动切到 NTP，而
  /// `set_options()` 只在启动时调过一次。写成成员的话，`GetSystemDateAndTime`
  /// 会永远报启动时那个模式：用户在界面上看到的、和他从 NVR 上看到的，
  /// 是两句话，而两句话各自都"有出处"，于是谁都不会觉得可疑。
  ///
  /// 回调**在循环线程上**被调（SOAP 处理器就在那条线程），实现里直接读
  /// `cfg.time.mode` 即可，不用加锁 —— 它和写它的网页 handler 是同一条线程。
  ///
  /// ## 它只影响一件事：`DateTimeType` 报什么
  ///
  /// `GetSystemDateAndTime` 的 `DateTimeType` 是个**枚举式的事实陈述**：
  /// `NTP` = "我的钟是自己对着 NTP 走的"，`Manual` = "我的钟是被人设的"。
  /// 从前这条的判据是 `DeviceState::time_synced()`，而在 NTP 真接上之后它
  /// **恰好反了** —— 一次成功的 NTP 对时也会把 `synced_` 置真，于是 NTP 模式
  /// 反而报 `Manual`。现在是模式驱动的，见 `device_op` 里那一段。
  std::function<std::string()> time_mode;

  /// NVR 要求按 NTP 对时（`SetSystemDateAndTime` 的 `DateTimeType=NTP`）时调用。
  ///
  /// ## 为什么是**两**个回调，而不是合成一个
  ///
  /// 上面那个是**纯读取**（只给 `GetSystemDateAndTime` 用），这个是**动作**。
  /// 合成一个 "问一次模式、顺手对不对时" 的接口，会让每一个只是想读一下模式的
  /// 调用点都在偷偷改设备时间。
  ///
  /// ## 调用时机是"响应发出去之后"
  ///
  /// 真去对时意味着一次可能阻塞两秒的 UDP 查询（而且地址数还不止一个）。它
  /// **不能**在 SOAP handler 里做 —— 那条线程同时管着 WS 推送和别的请求。所以
  /// `device_op` 只置一个位，`handle()` 用 `resp.on_sent(...)` 在响应真的发出去
  /// 之后才调这里。这和 `/api/reload` 的注释、`/api/restart`、`/api/time/sync`
  /// 是**同一个形状**（这棵树里这是第四处），照那个写，别发明第五种。
  ///
  /// 留空 = 什么都不做（`--rtsp` 单跑时就是这样，那里连 Web 都没有）。
  std::function<void()> on_ntp_request;

  /// NVR 用 `SetSystemDateAndTime` 里的 `UTCDateTime`/`LocalDateTime` 把设备时间
  /// 设成某个具体时刻之后调用，参数是**算出来的新偏移**（整秒，与 `DeviceState`
  /// 同一个量纲）。
  ///
  /// ## 它不负责"让时钟走" —— 那件事在这一层已经做完了
  ///
  /// `device->set_device_time()` 在上面几行就调了，水印和 `GetSystemDateAndTime`
  /// 下一帧就跟着走。这个回调只回答**另一个问题：这个偏移要不要留下来。**
  /// 把两件事合成一个回调（"设时间并落盘"）看起来更省，但那会让"落盘"这件事
  /// 的**失败**变成"设时间"的失败 —— 而设时间是无论如何都要成功的（回 Fault
  /// 会让海康的添加向导停在时间那一步）。
  ///
  /// ## 为什么必须留下来（Phase U 收尾时补的）
  ///
  /// 网页上那条手动校准的路**是落盘的**（`cfg.time.offset_s`），理由写在
  /// `TimeConfig` 的注释里：手动校准出来的偏移重启后要还在，不能指望它自己
  /// 再对一次。NVR 设的这一份是**同一类事实** —— 它也是"人（或那台机器）把钟
  /// 定在了这里"，也不会自己再对一次（除非 NVR 下次重连时重发）。不落盘的话，
  /// 重启之后设备时间弹回主机时间，于是**一段连续录像在重启前后带着两个时间
  /// 基准**，而水印上的时间正是取证时要和录像机对齐的那一项。
  ///
  /// ## 调用时机与线程
  ///
  /// **同步**，就在 SOAP handler 里（与 `on_patch` / `on_osd_patch` 同一个形状）
  /// —— 它做的事是一次小文件的写盘，不是一次 UDP 往返，所以不需要
  /// `on_ntp_request` 那套 `on_sent` 手法。回调在**循环线程**上被调，实现里
  /// 可以直接读写 `cfg`：写它的网页 handler 也是同一条线程。
  ///
  /// **要不要按模式区分（比如 NTP 模式下不落盘）由实现决定** —— 那属于
  /// "这台设备的时间是怎么维护的"，不是 ONVIF 该知道的事。和 `on_ntp_request`
  /// 那条一样，判断放在实现里。
  ///
  /// 留空 = 只改设备时钟、不落盘（`--rtsp` 单跑时就是这样）。
  std::function<void(int offset_s)> on_device_time_set;

  /// 操作日志（审计）的写入点。留空 = 不记。
  ///
  /// ## 为什么由 ONVIF 这一层写，而不是由上面那几个回调顺手写
  ///
  /// 那几个回调的签名里**都没有"谁"**（`on_patch` 收一个 `OnvifDevicePatch`、
  /// `on_osd_patch` 收一个 `OsdConfig`）—— 因为它们是"要做这件事"，而
  /// "这件事是谁要求做的"只有 SOAP 处理器知道（它手上有 `req.peer_ip()` 和
  /// Digest 里的用户名）。给它们各加一个 `who` 参数才是把这件事做坏的走法：
  /// 那是本轮改动里最容易碰坏别处的两处签名（计划里那条纪律）。
  ///
  /// 所以两边各写各的：这条负责"NVR 那侧谁来改了什么"，`on_patch` 那侧继续
  /// 只管"改什么"。
  ///
  /// **签名与 `WebOptions::audit` 一字不差** —— 读日志的时候不该需要知道
  /// 这一行是网页写的还是 NVR 写的（`who.source` 里那个前缀已经说明了）。
  ///
  /// 回调在**循环线程**上被调（就在 SOAP handler 里），所以它必须很快：
  /// 写一行、`fflush` 一次，没有别的。**不要在这里做网络或磁盘扫描** ——
  /// 它挡着 WS 推送与其它请求。
  std::function<void(AuditWho const& who, std::string const& action,
                     std::string const& detail)>
      audit;
};

/// `AppConfig::onvif` 里**可以运行中改**的那一段：身份与凭据。
///
/// ## 为什么只有这一段能热生效
///
/// 热生效的代价取决于"改它要动什么"。这一段**什么都不用动**：它整个生命周期
/// 里只在**循环线程**上被读（SOAP 请求就在那条线程上处理），既不碰编码器，
/// 也不碰 socket，也不参与任何已经算好的东西。改它就是把几个字符串换掉。
///
/// 反过来，其余每一项都做不到：
///
///   - `capture.*`（分辨率/码率/fps/gop/preset/线程数）要在编码器上生效。
///     **码率这条实测过**：`avctx->bit_rate` 中途改了 FFmpeg 的 libx264 封装层
///     根本不看（见 `tests/x264_reconfig_check.cpp`），而唯一能让 x264 改主意
///     的 `x264_encoder_reconfig` 没有从 `avcodec-63.dll` 里导出（导出表里
///     零个 x264 符号）。所以真实做法只能是重建编码器 —— 而重建就意味着
///     RTSP 会话重来、**所有客户端掉线**。那就是重启，只是把失败模式搬进了
///     一个正在跑进程里，更糟。
///   - `rtsp.port` / `web.port` / `web.host` 要重新 bind。
///   - `rtsp.path` / `sub_path` 要重建 ServerMediaSession。
///   - `onvif.enabled` 要增删路由，而它和 WS-Discovery 是联动的。
///
/// 这些一律留在 `pending` 里告诉用户重启，不在这个结构里。
///
/// **不含 `uuid` / `rtsp_port` / `http_port` / `profiles`**：前两个跟着监听
/// 端口走，后一个被每秒一次的分辨率上报**持续改写** —— 整体替换会把真实
/// 分辨率打回启动时的猜测值（详见 `set_identity` 的实现注释）。
struct OnvifIdentity {
  std::string user = "admin";
  std::string password;
  std::string manufacturer = "Desk2ONVIF";
  std::string model = "Desk2ONVIF Virtual Camera";
  std::string firmware_version = "1.0.0";
  std::string hardware_id = "1.0";
  /// 空 = 调用方应当填上派生的稳定值（见 main.cpp 里 `make_serial` 那一段）。
  /// **这里的空不是"没配"**，交给一个明确的地方去派生，而不是在这里猜。
  std::string serial_number;
  std::string device_name = "Desk2ONVIF";
  std::string location = "Desktop";
};

/// 从配置里取出身份那一段。**不做任何派生** —— 派生要用 `machine_seed()`，
/// 而这个头不该知道它。
OnvifIdentity identity_from_config(OnvifConfig const& c);

/// 把身份写进完整的选项结构：**只动身份那几个字段**，`uuid` / `rtsp_port` /
/// `http_port` / `profiles` / `realm` 原样保留。
void apply_identity(OnvifServiceOptions& o, OnvifIdentity const& id);

/// 把身份写回**配置**结构，和 `apply_identity` 成对。
///
/// 存在的理由和上面那对一样：`main.cpp` 要维护一份"运行中那一侧"的配置快照，
/// 热改成功之后得把身份同步进去。手写那九个赋值的话，字段集就有三份了
/// （读配置的、写选项的、写快照的），加一个身份字段漏掉哪一份都不报错。
void identity_to_config(OnvifIdentity const& id, OnvifConfig& c);

class OnvifService {
 public:
  OnvifService();
  ~OnvifService();
  OnvifService(OnvifService const&) = delete;
  OnvifService& operator=(OnvifService const&) = delete;

  /// 启动时设一次。之后不再改（改了就要考虑并发，而它整个生命周期里
  /// 都在循环线程上被读）。
  void set_options(OnvifServiceOptions o);
  OnvifServiceOptions const& options() const;

  /// 改身份与凭据，**运行中生效**。这是整个配置里唯一能热生效的一块。
  ///
  /// **只能在循环线程上调**（和 `handle()` 同一条线程）。这条约束是硬性的，
  /// 但它同时也就足够安全了：SOAP 请求在循环线程上处理，所以不可能有一条
  /// 请求正读到一半时被改。Web 的 POST 处理器本身就在循环线程上跑，
  /// 所以那边直接调即可，不需要 `app.post()` 绕一圈。
  ///
  /// 口令变了会把已发出的 nonce 全部作废（重建校验器）—— 换口令之后旧的
  /// nonce 不该还能用，而这是 `DigestAuth` 的既有语义（见陷阱 30）。
  ///
  /// **顺带把 `device_name` 同步进 `DeviceState`**（如果接了的话）。这一步是
  /// 水印那个「名称」项能在 NVR 改名之后立刻跟着变的原因 —— 采集线程每帧现读
  /// `DeviceState`，所以不需要重开管线。放在这里而不是放在调用方，是为了让
  /// "改了名字"这件事只有一处落点：启动时（`onvif.device_name`）、网页上改
  /// （走这里）、NVR 改名（也走这里）三条路都经过同一个赋值。
  void set_identity(OnvifIdentity const& id);

  /// 改 HTTP 端口。**只能在循环线程上调**（开机时用 `app->post()` 投一帧过去）。
  /// 单独开一个 setter 是因为它和别的选项不一样：监听端口要等 bind 成功才
  /// 知道（配置里写 0 表示由系统分配），而路由必须在那之前就注册好。
  void set_http_port(int p);

  /// 接上设备状态（设备时钟、摄像头名称）。**不持有它**，调用方保证它活得比
  /// 本对象长 —— 在 `main.cpp` 里它是 `cmd_run` 作用域的局部对象，比 WebServer
  /// 先声明、后析构。
  ///
  /// 接上之后有两件事变了：
  ///
  ///   - `GetSystemDateAndTime` 报的是**设备时间**（主机时间 + NVR 下发的偏移），
  ///     不是主机时间；状态页和水印因此和 NVR 的时间轴对齐。
  ///   - `SetSystemDateAndTime` 能真的生效，而不只是回一个"成功"。
  ///
  /// 没接上时退回主机时间，`SetSystemDateAndTime` 回 `ter:ActionNotSupported`
  /// —— "单跑录制、没有 ONVIF"那条路依然自洽。
  ///
  /// **只能在循环线程上调**（和 `set_options` 同一条线程，在它之后）。
  void set_device_state(DeviceState* state);

  /// 换一份水印配置。**和 `set_options` / `set_identity` 同一条线程**（循环
  /// 线程），网页保存配置时由 `WebServer::apply_osd_config` 调过来。
  ///
  /// 单独开一个 setter，而不是让调用方重建整个 `OnvifServiceOptions`：重建会把
  /// `profiles` 里的分辨率打回启动时的猜测值（理由和 `set_identity` 那段一模
  /// 一样，见那里的注释）。
  ///
  /// **画面上的水印不会因此改变** —— 采集线程用的是启动时那一份。这里改的是
  /// "NVR 读到的 OSD 列表"，它必须和存盘的配置一致，否则用户在网页上删了一项
  /// 水印、打开 NVR 的 OSD 页面又看到它，只能理解成"没删掉"。
  void set_osd(OsdConfig const& c);

  /// 上报第 `index` 路码流的真实分辨率（0 = 主，1 = 子）。采集开始后由统计
  /// 回调带过来 —— 报错分辨率有些 NVR 会拒绝这个 profile，而症状只是
  /// "添加失败"，没有任何线索。
  ///
  /// `index` 越界是静默忽略。**不能写成"给所有 profile 设同一个尺寸"**：
  /// 子码流的分辨率本来就不同，那样会把两路报成一样大，NVR 拿到的
  /// GetProfiles 与实际码流对不上。
  ///
  /// 同样**只能在循环线程上调**。
  void set_video_size(std::size_t index, int w, int h);

  /// 处理一条 SOAP 请求。返回 true 表示响应**已经写完**（含 401 和 Fault）。
  ///
  /// `media_service` 为真表示路由到媒体服务，否则设备服务。两个服务的操作名
  /// 有重叠的可能（都有 `GetServiceCapabilities`），所以按**路由**而不是按
  /// Body 的命名空间来分派 —— 后者的行为取决于客户端写了什么，而路由是我们
  /// 自己定的，稳定得多。
  bool handle(uvcpp::uvcpp_web_request& req, uvcpp::uvcpp_web_response& resp,
              bool media_service);

  /// 关掉之后连 `GetSystemDateAndTime` 也要鉴权。默认开着 —— 规范要求它是
  /// 匿名可访问的（客户端要先拿到时间才能算 Digest 的时钟偏移）。
  /// 实现在 .cpp 里：状态在 Impl 里，头文件里写不出来。
  void set_anonymous_date_time(bool on);

  /// 用 ONVIF 那一份 Digest 凭据校验一个**普通 HTTP** 请求。
  ///
  /// 用途只有一个：`GET /snapshot.jpg`。它同时被两种客户端拿 —— 浏览器走
  /// Web 会话 cookie，NVR 走 `GetSnapshotUri` 报出去的那个地址并按 ONVIF 的
  /// 规矩带 Digest。所以那个路由上的鉴权得认两套凭据，而 Digest 这一半必须
  /// 复用这里。
  ///
  /// **绝对不能就地新建一个 `DigestAuth`**：nonce 是用每进程一份的随机密钥
  /// 签的，重建对象就换了密钥，于是已经发出去的 nonce 全部失配 —— 客户端
  /// 会陷进"401 → 拿新 nonce → 还是 401"，而每一次往返看上去都完全合规。
  ///
  /// 返回 true 表示凭据正确；false 时调用方应当回 401。无论真假，`challenge`
  /// 都会被填成一份可以直接塞进 `WWW-Authenticate` 的挑战串。
  bool verify_http_digest(uvcpp::uvcpp_web_request& req, std::string& challenge);

  std::uint64_t requests() const;
  std::uint64_t auth_failures() const;
  std::uint64_t faults() const;

  /// 最近一次被拒绝的操作名，诊断用（NVR 调了我们没实现的接口时能一眼看到）。
  std::string last_unsupported() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace d2o
