#pragma once

/**
 * @file src/common/config.h
 * @brief 运行期配置：结构、JSON 往返、读写。
 *
 * 配置文件的解析必须**容忍**，不能**猜**。这里的分工是：
 *
 *   - 键**缺失** → 保留默认值（老配置文件拿到新代码上照样能跑）；
 *   - 键**类型不对** → 报错并指出是哪个字段（手改坏了要能一眼看出来，
 *     不能默默退化成默认值 —— 那会让人对着一个"改了没反应"的配置查半天）；
 *   - 值**超范围** → 由 `validate_config()` 夹到合法区间并**逐条报告**，
 *     而不是静默修正。静默修正等于"你写的 0 变成了 30，而没人告诉你"。
 */

#include <string>
#include <vector>

namespace d2o {

struct CaptureConfig {
  unsigned display_index = 0;   ///< --list-displays 的序号
  int fps = 30;
  int bitrate_kbps = 4000;
  int gop_seconds = 2;
  /// 编码线程数。<= 0 = 按"延迟优先"自动选（封顶 6，见 EncoderConfig::threads）。
  int threads = 0;
  std::string preset = "ultrafast";
  std::string tune = "zerolatency";
  std::string profile = "main";
};

struct RtspConfig {
  bool enabled = false;
  int port = 554;
  std::string path = "/live";

  /// 子码流：**第二路编码器**，不是"编一次再抽帧"（那要先解码，纯浪费）。
  ///
  /// 默认**开**。它存在的唯一理由是 NVR 的多画面网格 —— 而 NVR 默认拉的是
  /// 主码流，于是 16 路网格会同时拉 16 条 2880x1920 的流。开着子码流，NVR
  /// 在网格里走子码流、点开单画面才切主码流，带宽和 CPU 都省一个数量级。
  /// 关掉它的代价比开着大，所以默认开着；不需要时显式写 `false`。
  ///
  /// **只有 `rtsp.enabled` 为真时才有意义** —— 子码流的唯一出口就是 RTSP。
  bool sub_enabled = true;
  std::string sub_path = "/live_sub";
  /// 按**高度**给，宽度按主码流的宽高比推。NVR 的界面是按高度分档的
  /// （D1 / 720p / 360p），写死宽度在 16:10、21:9 这些比例下会变形。
  int sub_height = 360;
  /// 只进 SDP 的 `b=AS:`，不影响编码 —— 实际码率由编码器按这个值算。
  int sub_bitrate_kbps = 512;
};

struct WebConfig {
  bool enabled = false;
  std::string host = "0.0.0.0";
  int port = 8080;
  /// Vue 构建产物的目录。相对路径按**进程工作目录**解析（libuvcpp 的
  /// serve_static 就是这个语义，见 uvcpp_web_static 的说明）。
  std::string static_root = "web/dist";

  /// Web 控制台要不要登录。默认**开**。
  ///
  /// 关掉它等于把"读配置、改配置、看桌面画面"三件事一起交给同一个局域网 ——
  /// 而 `start.bat` 默认监听 `0.0.0.0`。以前这个界面就是完全敞开的，唯一的
  /// 退路是记得改用 `start-local.bat`。
  bool auth_enabled = true;
  std::string user = "admin";
  /// 默认 `admin`。**不能默认成空串** —— 空口令在这个服务里不是"不鉴权"，
  /// 而是"口令就是空"：登录框照常出现、校验照走，任何知道用户名的人按一下
  /// 回车就进去了。那比明说"没鉴权"更坏，因为它看起来有门。
  /// （ONVIF 那份凭据的同一个坑见 OnvifConfig::password。）
  std::string password = "admin";
};

/// ONVIF 的身份与凭据。**没有端口** —— ONVIF 的 SOAP 端点和 Web UI 共用
/// 同一个 HTTP 服务（`POST /onvif/device_service`），分开配只会让人以为
/// 要开两个端口。
struct OnvifConfig {
  /// 默认**开**。ONVIF 就是这个程序存在的理由，而且它和 Web UI 共用同一个
  /// HTTP 端口，所以"`--http` 起来的时候它也起来"是唯一合理的默认。
  ///
  /// 以前这里是 `false`，结果是**全新安装下 `desk2onvif --http` 一个 ONVIF
  /// 端点都不提供**，而 `--no-onvif` 这个"关掉它"的开关因此永远无事可做 ——
  /// 一个必须靠命令行开关才能打开的默认功能，和一个只用来关它的开关同时
  /// 存在，本身就是自相矛盾的。现在两个方向都能走：配置里写 `"enabled":
  /// false` 可以持久关掉，`--onvif` 可以临时打开，`--no-onvif` 可以临时关掉。
  bool enabled = true;
  std::string user = "admin";
  /// 默认 `admin`，和 `--help` 里写的一致。**不能默认成空串**：空口令下
  /// Digest 挑战照发、校验照走，于是任何知道用户名（默认 `admin`，写在帮助
  /// 里）的人用空口令就能过 —— 看上去有鉴权、实际谁都能进。这种"以为配了
  /// 口令其实没配"的状态比明说无鉴权更危险。
  /// 真要空口令（比如本机自用），显式在配置里写 `"password": ""` —— 那是
  /// 明确的选择，不是默认值，main.cpp 会为此打一条警告。
  std::string password = "admin";
  std::string manufacturer = "Desk2ONVIF";
  std::string model = "Desk2ONVIF Virtual Camera";
  std::string firmware_version = "1.0.0";
  std::string hardware_id = "1.0";
  /// 空 = 启动时按机器名派生一个稳定的值。ONVIF 的 UUID 必须是
  /// `urn:uuid:...` 形式且**跨重启不变**，否则 NVR 会当成一台新设备重复添加。
  std::string serial_number;
  std::string device_name = "Desk2ONVIF";
  /// GetDeviceInformation 的 Location 字段，NVR 界面上会显示。
  std::string location = "Desktop";
};

struct AppConfig {
  CaptureConfig capture;
  RtspConfig rtsp;
  WebConfig web;
  OnvifConfig onvif;
};

/// `%APPDATA%\Desk2ONVIF\config.json`；%APPDATA% 取不到时退到 exe 同级。
std::string default_config_path();

/// 读配置。**文件不存在不算错误** —— 返回 true 且 out 保持默认值，
/// created 置 false。真正的读取/解析失败才返回 false 并填 err。
bool load_config(AppConfig& out, std::string const& path, std::string& err,
                 bool* existed = nullptr);

/// 写配置。会按需建目录。先写临时文件再改名，避免写到一半掉电留下半个文件。
bool save_config(AppConfig const& c, std::string const& path, std::string& err);

/// 把越界的值夹到合法区间。每夹一项就往 notes 里写一条人话。
/// 返回是否有任何改动。
bool validate_config(AppConfig& c, std::vector<std::string>& notes);

/// 生效值 → JSON。密码按 `redact` 决定是否抹掉（默认抹）。
/// 抹掉时写 `"__unchanged__"` 哨兵，POST 回来时该字段被忽略。
std::string config_to_json(AppConfig const& c, bool redact_secrets = true);

/// 逐字段比两份配置，把**取值不同**的那些按点分路径**追加**到 `out`
/// （如 `rtsp.port`、`onvif.device_name`）。
///
/// **结果会按键名字典序排序并去重**（整个 `out`，不只是本次追加的部分），
/// 所以调用方可以直接对它做二分查找 —— "这次变了的" 减去 "还没生效的"
/// 就是这个功能的核心运算。
///
/// 用途只有一个，但很要紧：`POST /api/config` 保存的是一份**文件**，而运行中
/// 的服务用的是启动时锁定的另一份。不把两者的差异列出来，界面上就只能说
/// "需要重启"或者"已生效" —— 而一次保存里两种字段常常同时存在，任选一种都是
/// 在骗人。有了它，前端可以逐条说清哪些当场生效、哪些要重启。
///
/// 口令**参与比较**（`redact_secrets = false`）：改口令恰恰是最需要被看见的
/// 一项，而默认的哨兵抹除会让两份配置在这一项上"看起来永远一样"。
void diff_config(AppConfig const& a, AppConfig const& b,
                 std::vector<std::string>& out);

/// JSON 文本 → 配置。缺失的键保留默认值；类型错误返回 false 并填 err。
/// 密码字段等于哨兵时**不覆盖** out 里已有的值。
bool config_from_json(std::string const& text, AppConfig& out, std::string& err);

/// 密码字段被抹掉时用的哨兵值。
extern char const* const kSecretSentinel;

}  // namespace d2o
