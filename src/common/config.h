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
  int fps = 25;
  int bitrate_kbps = 2048;
  int gop_seconds = 2;
  /// 编码线程数。<= 0 = 按"延迟优先"自动选（封顶 6，见 EncoderConfig::threads）。
  int threads = 0;
  std::string preset = "ultrafast";
  std::string tune = "zerolatency";
  std::string profile = "main";

  /// 画质档位：`high` / `balanced` / `smooth` / `verylow` / `custom`。
  ///
  /// **它不是一份独立的设置，而是一个"预设展开器"**：只要它不是 `custom`，
  /// `validate_config()` 就用这个档位的值去覆盖 fps / bitrate_kbps /
  /// max_height / rtsp.sub_enabled 四项，并且被覆盖的项**逐条记进 notes**。
  /// 这样存盘、`GET /api/config`、`diff_config` 看到的永远是**具体值**，
  /// 而 `quality` 只是"这套值是哪来的"的标签 —— 反过来（档位只是个建议、
  /// 具体值各自为政）会得到一个"选了流畅但帧率还是 30"的界面。
  ///
  /// 用户在界面上手改任何一项具体字段，前端就把这里翻成 `custom`。
  /// 这也正是 `custom` 存在的唯一理由：它是"我自己配的，别动"的标记。
  std::string quality = "balanced";

  /// 主码流的高度上限。**0 = 用桌面原生尺寸**（不缩放）。
  ///
  /// 「高性能」和「均衡」两档是 0；「流畅」1080、「极低」720。只写**上限**：
  /// 桌面本来就比它小的话不放大 —— 把小桌面拉大既费 CPU 又不会多出信息。
  int max_height = 0;

  /// 延时档位：`realtime` / `balanced` / `quality` / `custom`。
  ///
  /// 与 `quality` 一样是**预设展开器**，但展开的字段集合与它**不相交**：
  /// 画质档动 fps / bitrate_kbps / max_height / rtsp.sub_enabled，延时档只动
  /// **threads / gop_seconds**。两个集合没有交集，所以「流畅 + 极致低延迟」
  /// 这种组合天然合法，**不需要任何优先级规则** —— 这也是把它写成第二个展开器
  /// 而不是塞进画质档位表的原因（塞进去就会出现 4×4 的组合表，而其中大多数
  /// 组合是重复的）。
  ///
  /// ⚠️ **三档的 `preset` / `tune` / `profile` 一律不动。** 这不是保守：
  /// `preset=ultrafast` 会关掉 CABAC/8x8dct/weightp，x264 于是把档位降到
  /// **Constrained Baseline，不管 `profile` 写的是什么**。一旦某一档把 preset
  /// 放松，同一个设备的 H.264 profile 就会从 Constrained Baseline 变成 Main ——
  /// 而 NVR 的兼容性、`GetProfiles` 报出去的编码参数、子码流的解码开销**没有一样
  /// 在档位表上**。判据：这一档换来的画质提升，必须能**只看 threads 和
  /// gop_seconds 这两个数**就解释得了。
  std::string latency = "balanced";
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

/// 音频（AAC 音轨）。它是**一条音轨**，同时进 MP4 与 RTSP 主码流 ——
/// 不是"录像带声音、推流不带"或反过来。想让 NVR 那侧回到纯视频就关掉总开关，
/// 那也同时关掉了录像里的音轨：一个开关一条语义，不是"关了一半"。
struct AudioConfig {
  /// 默认**关**。默认打开会**改变每一台设备的 SDP**，而真实 NVR 的兼容性只能
  /// 靠实测 —— 打开之后 NVR 不接受，用户看到的症状是"加了音频就没画面"，
  /// 而归因会指向编码器。这个决定不该替用户做。（陷阱 79）
  bool enabled = false;

  /// 两个源**独立**，可以同时开（同时开时各乘 1/√2 求和，免得恒定削顶）。
  ///
  /// 两个默认都是 false：`enabled` 打开却没勾任何一个源，是一份**自相矛盾**的
  /// 配置 —— 它想要一条音轨，却没说这条音轨该录什么。`AudioTrack::start()`
  /// 对这种配置直接拒绝，于是录音里没有音轨；这里不如照样保持关闭姿态，
  /// 让"没声"这件事在配置里就看得出来。
  bool system = false;
  bool microphone = false;

  /// 麦克风的设备 id（`GET /api/audio/devices` 给的那个）。
  /// **空 = 系统默认端点** —— 设备被拔掉之后"默认"会跟着系统走，这正是想要的；
  /// 写死了 id 的话换一台机器/换一副耳机就变成"设备不存在"。
  std::string device;

  /// AAC 码率。64 kbps 对"听得清"足够（AAC-LC 64k 约等于 MP3 128k 的观感），
  /// 而它**不占视频的码率预算** —— 两路各自独立编码。
  int bitrate_kbps = 64;

  /// 两路各自的增益。1.0 = 原样。麦克风常常偏轻、环回常常偏响，
  /// 所以两个数分开给，而不是一个"音量"。
  float mic_gain = 1.0f;
  float system_gain = 1.0f;
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
  /// 设备 UUID（`urn:uuid:...`）。**启动时为空就推导一次、写回配置文件**，
  /// 此后永远用文件里那一份 —— 和 `serial_number` 同一套规则。
  ///
  /// 为什么不"每次启动重推、反正结果一样"：`machine_seed()` 从第五轮起只吃
  /// hostname + MachineGuid（**不再吃 MAC**，见 `net_identity.cpp` 里那段），
  /// 所以重推今天确实得到同一个值。但"推导出来的"和"存过盘的"是两件事：
  /// 种子的组成只要再变一次，所有已装的机器就会在升级后**集体换 UUID**，
  /// 而 NVR 那边看到的是一夜之间多出一排"新"摄像机、原来那些全部离线。
  /// 存下来就没有这个窗口。
  std::string device_uuid;
  std::string device_name = "Desk2ONVIF";
  /// GetDeviceInformation 的 Location 字段，NVR 界面上会显示。
  std::string location = "Desktop";
};

/// 一项烧进画面里的水印。
struct OsdItemConfig {
  bool enabled = true;
  /// `time` = 日期时间（跟着设备时钟走）、`name` = 摄像头名称、`text` = 固定文字。
  std::string type = "time";
  /// 只有 `type == "text"` 时用。
  std::string text;
  /// `upper_left` / `upper_right` / `lower_left` / `lower_right` / `custom`。
  std::string position = "upper_left";
  /// 只有 `position == "custom"` 时用，都是**百分比**（0..100），指水印左上角。
  int x_pct = 0;
  int y_pct = 0;
  /// 字号的像素高度。**0 = 自动**（帧高 / 32，见 OsdOverlay）。
  /// 给的是**主码流的分辨率**下的像素数 —— 子码流是同一帧缩下去的，
  /// 所以那里的字会等比变小（见 README 的"没做的"）。
  int font_size = 0;
  /// `type == "time"` 时的 strftime 格式。
  std::string date_format = "%Y-%m-%d";
  std::string time_format = "%H:%M:%S";
};

/// 水印总配置。
struct OsdConfig {
  bool enabled = true;

  /// **自适应配色**：先量水印要落在的那块区域的亮度，黑底就写白字、
  /// 白底就写黑字。海康那套 OSD 就是这个行为。
  bool adaptive_color = true;
  /// 反色描边。有了它，即使自适应的判断在渐变背景上选错了色，字也读得出来。
  bool outline = true;
  /// 文字底下的半透明底板的不透明度（0 = 关）。默认关：海康的默认 OSD
  /// 没有底板，而底板会盖住画面，是个更重的决定。
  int plate_alpha = 0;

  /// 默认两项：**时间在左上、名称在右下**（用户明确指定，与海康自己的
  /// 默认正好相反 —— 海康是名称在左上、时间在右下）。
  std::vector<OsdItemConfig> items = [] {
    OsdItemConfig t;
    t.type = "time";
    t.position = "upper_left";
    OsdItemConfig n;
    n.type = "name";
    n.position = "lower_right";
    return std::vector<OsdItemConfig>{t, n};
  }();

  /// 上限。`CreateOSD` 会撞到它（多出来的返回 Fault 而不是静默丢弃）。
  static constexpr std::size_t kMaxItems = 4;
};

/// 性能不足时的自适应降载。
struct AdaptiveConfig {
  /// `off` = 完全不干预；`throttle` = 自动丢帧，仍不够就只**提示**等确认
  /// （默认，也是用户选的那个）；`auto` = 连提示都不用，自动降档重开管线。
  ///
  /// 三档的差别是**代价**带来的：throttle 不中断任何东西，auto 会在无人
  /// 操作时把录制分段、把 RTSP 客户端踢下线。
  std::string mode = "throttle";
  /// 丢帧的下限帧率。duty 的上限 = fps / min_fps，所以它同时决定"最多丢到
  /// 几分之一"。低于 5 fps 的画面已经不像监控了，再丢不如降档。
  int min_fps = 5;
};

/// 设备时间：手动校准 / SNTP 对时。
///
/// **作用范围是"设备时间"（= 主机时间 + 偏移），不是 Windows 系统时钟。**
/// 水印上那个时间、ONVIF `GetSystemDateAndTime` 报出去的那个时间，用的都是它。
/// 好处是既不需要管理员权限，也不会连累这台机器上别的程序；代价是它可以和
/// 系统时间对不上（见 README 里那条边界）。
struct TimeConfig {
  /// `manual` = 只认手动校准（含 NVR 下发的 `SetSystemDateAndTime`）；
  /// `ntp`    = 按 `ntp_server` 周期对时，对时结果同样落成偏移。
  std::string mode = "manual";
  std::string ntp_server = "ntp.aliyun.com";
  /// 对时间隔（秒）。**0 是有意义的值**：只在启动时对一次。上界一天。
  int ntp_interval_s = 3600;
  /// 设备时间 = 主机时间 + 这个（秒）。
  ///
  /// **落进配置是刻意的**：手动校准出来的偏移重启后要还在（NTP 那条路可以自己
  /// 再对一次，手动那条不行）。恢复出厂删掉 config.json 时它自然归零，不需要
  /// 额外代码 —— 这与 Phase R 那条"恢复的是配置文件"的语义正好合上。
  int offset_s = 0;
};

/// 操作日志（审计）。**与调试日志是两份文件**，理由见 `audit_log.h`。
struct LogConfig {
  /// 关掉之后一行都不写。默认**开**：审计的价值全在「事后回头查」，
  /// 而默认关着的话，真出事那天才发现它从来没开过。
  bool audit = true;
  /// 留存天数。默认 **180**（用户原话「本机默认存半年」）。**0 = 永不剪枝**。
  int audit_keep_days = 180;
};

/// 附加通道（**通道 2 起**）的一项。
///
/// ## 通道 1 不在这里
///
/// 通道 1 继续用顶层的 `capture.*` / `rtsp.*` / `onvif.*`，**永远不出现在
/// `items[]` 里**。这不是省事，是兼容边界：`channels` 这个键缺失（或
/// `items` 为空）时，从 `config_to_json` 到运行期的行为必须和第五轮之前
/// **一个字节都不差** —— 那两百多条既有断言、以及所有已装机器的
/// `config.json` 都压在这条线上。把通道 1 搬进 `items[0]` 会让每个字段
/// 都多一层"到底哪份是权威"的分叉。
///
/// ## `items[]` 里的显示器项是**覆盖**，不是"新增"
///
/// 运行期会自动发现所有可捕获的显示器并各建一个通道（默认开、默认「流畅」）。
/// `items[]` 里某一项的作用是**改写**它对应的那个自动通道：要关掉某个显示器，
/// 就写一条 `enabled:false` 的项。**摄像头通道永远只来自显式的 `items[]`**
/// —— 没有任何"自动发现摄像头"这回事（用户明确选的）。
///
/// 所以 `items[]` 的条目数**不等于**通道数，而等于"被覆盖的显示器 + 全部摄像头"。
struct ChannelConfig {
  bool enabled = true;

  /// 空 → 按序号取「通道 N」。**不在这里填默认值** —— 序号要由它在数组里的
  /// 位置决定，而结构体不知道自己的位置（填在 `validate_config` 里）。
  std::string name;

  /// `display` = 抓一块屏幕；`webcam` = 抓本机摄像头。
  ///
  /// 这个字段决定**另外两个字段里哪个有用**：`display` 看 `display_index`、
  /// `webcam` 看 `camera`。所以配置里留着另一个是正常的（切回来时不用重填）。
  std::string source = "display";

  /// `source == "display"` 时抓哪一块屏（`--list-displays` 的序号）。
  unsigned display_index = 0;

  /// `source == "webcam"` 时的设备（`--list-cameras` 给的符号链接，或友好名）。
  /// **空 = 第一台**：摄像头被拔掉再插上时符号链接会变，写死了就变成
  /// "设备不存在"，而"第一台"跟着系统走正是想要的。
  std::string camera;

  /// 画质档位，语义与 `CaptureConfig::quality` **完全一样**（是一个预设展开器，
  /// 不是独立设置）。默认 **`smooth`（流畅）** 而不是 `balanced`：通道 2 起
  /// 多半是给 NVR 的多画面网格用的，而每个通道都是一路独立的编码器 ——
  /// 都按「均衡」开会把 CPU 吃光。主屏那个通道仍然是顶层 `capture.quality`
  /// 说了算（默认「均衡」），用户想看清楚的还是那一块。
  std::string quality = "smooth";

  int fps = 20;
  int bitrate_kbps = 1024;
  /// 0 = 源的原生尺寸（不缩放）。档位展开会把「流畅」写成 1080。
  int max_height = 1080;

  bool sub_enabled = true;
  int sub_height = 360;
  int sub_bitrate_kbps = 512;

  /// RTSP 路径。**空 → 按序号取 `/chN` / `/chN_sub`**，同 `name`。
  /// 非空时 `validate_config` 会补上前导 `/`、并保证与别的路由不撞。
  std::string rtsp_path;
  std::string rtsp_sub_path;
};

/// 多通道配置。**`items` 为空 = 只有通道 1**，也就是今天的行为。
struct ChannelsConfig {
  /// **上限 8**。定这个数不是因为算不过来，而是因为每个通道站一路独立编码器
  /// 加一个 ONVIF 端口：8 路 1080p 已经能把这台机器按住，再多给只是让人
  /// 一次性把机器配死、然后归因到"这软件卡"。
  static constexpr std::size_t kMaxChannels = 8;

  std::vector<ChannelConfig> items;
};

struct AppConfig {
  CaptureConfig capture;
  RtspConfig rtsp;
  AudioConfig audio;
  WebConfig web;
  OnvifConfig onvif;
  OsdConfig osd;
  AdaptiveConfig adaptive;
  TimeConfig time;
  LogConfig log;
  ChannelsConfig channels;
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
///
/// **画质档位的展开也在这里**（`quality != "custom"` 时用档位覆盖四项具体值），
/// 所以调用方不必自己再展开一次 —— 走到这之后的 `AppConfig` 就是自洽的。
bool validate_config(AppConfig& c, std::vector<std::string>& notes);

/// 画质档位。`key` 就是配置里写的那个字面量。
struct QualityTier {
  char const* key;
  char const* label;  ///< 中文名，界面直接显示
  int fps;
  int bitrate_kbps;
  int max_height;  ///< 0 = 桌面原生
  bool sub_enabled;
};

/// 四档预设，**从最费性能到最省**排列（界面的顺序、降档的方向都靠它）。
std::vector<QualityTier> const& quality_tiers();

/// 按 key 找档位。`custom` 和不认识的 key 都返回 nullptr。
QualityTier const* find_quality_tier(std::string const& key);

/// 延时档位。展开的是 `threads` / `gop_seconds` 两项，与 `QualityTier`
/// 展开的四项**不相交**（见 `CaptureConfig::latency` 的注释）。
struct LatencyTier {
  char const* key;
  char const* label;  ///< 中文名，界面直接显示
  /// 编码流水线深度（帧）。帧级并行下每个线程约等于一帧的延迟，所以它就是
  /// "编码器这一环吃掉多少毫秒"的那个数：@25fps 时 1 帧 = 40 ms。
  int threads;
  /// 关键帧间隔（秒）。它决定的**不是**"看到之后有多滞后"，而是
  /// "新观众多久能看到第一帧"（分片只在关键帧处产生），以及网页预览里
  /// "分片节奏"那一项的粗细。
  int gop_seconds;
};

/// 三档预设，**从延迟最低到延迟最高**排列（界面的顺序靠它）。
///
/// ⚠️ 表里的 threads 是**钉死的值**，选档位就等于把 `capture.threads` 钉成它。
/// `0`（= 按延迟优先自动选、封顶 6）只留给 `custom`：否则"均衡"就不是一个
/// 确定的点，而是"跟着自动值走"，档位表也就没意义了。代价是
/// "我什么都没改，怎么跟以前不一样了" —— 用默认档 + 一条 note 说清楚。
std::vector<LatencyTier> const& latency_tiers();

/// 按 key 找档位。`custom` 和不认识的 key 都返回 nullptr。
LatencyTier const* find_latency_tier(std::string const& key);

/// 当前这一档的**下一档**（更省性能的那一档）的 key；已经在最低档时返回空串。
///
/// 判据是**帧率**：不是因为它最能代表负载，而是因为它是自适应**唯一能实时改**
/// 的那一项（码率在这条链路上改不了，见 README）。所以"下一档"问的实际就是
/// "帧率还能往哪降"。`quality == "custom"` 时按当前 fps 反推落在哪一档。
std::string suggest_lower_quality(AppConfig const& c);

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

/// 水印配置 → 一行 JSON 文本，**只用来比"改没改"**（不是给人读的）。
///
/// 为什么需要它：NVR 会周期性地把同一份 OSD 重复下发（`SetOSD` 不是幂等的
/// 语义，它只是"设成这样"），而我们每次收到都要决定"要不要存盘 + 要不要
/// 热生效 + 要不要写一行日志"。三件事都做的话，日志会被淹没，存盘也在白干。
/// 判据就是这份文本相不相等 —— 用它而不是逐字段手写比较，是因为手写的那份
/// 迟早会和 `config_to_json` 里的 osd 子树漂移（陷阱 52 的同一个形状）：
/// 两边共用 `osd_json_node()`，字段集不可能漏。
std::string osd_to_json(OsdConfig const& o);

/// JSON 文本 → 配置。缺失的键保留默认值；类型错误返回 false 并填 err。
/// 密码字段等于哨兵时**不覆盖** out 里已有的值。
bool config_from_json(std::string const& text, AppConfig& out, std::string& err);

/// 密码字段被抹掉时用的哨兵值。
extern char const* const kSecretSentinel;

}  // namespace d2o
