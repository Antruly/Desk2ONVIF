/**
 * @file src/main.cpp
 * @brief Desk2ONVIF 入口。
 *
 * 入口接下十来种形态（`--help` 有完整清单：控制台 / 服务 / 录制 / 推流 / Web /
 * ONVIF / 防火墙 / 音频设备……）。其中最老的三条是：
 *
 *   --selfcheck       把依赖真的链上、真的调一次
 *   --list-displays   列出所有显示输出，并**真的**探一遍哪些能录
 *   --record <文件>    录一段桌面到 MP4
 *
 * 另加一条**刻意不做任何事**的：
 *
 *   -v / --version    只打一行版本号就退。它和 --selfcheck 分开是有理由的 ——
 *                     见 cmd_version() 上面那段注释。
 *
 * 自检那部分值得单独说明：依赖出问题时，下面这三种失败方式在编译期一个都
 * 看不出来 ——
 *
 *   1. 静态库链接顺序错：报"未定义符号"，但指不到真正该补的那个库；
 *   2. 运行期 DLL 没就位：双击只弹一个 0xc0000135，没有任何上下文；
 *   3. 模块没编进来：头文件和符号都在，只是行为不是你要的。libuvcpp 的
 *      SOAP 模块正是这种（所有预编译包的 UVCPP_WSDL_ENABLE 都是 0）。
 *
 * 所以那里的每一项都**真的调用**了对应库的代码，而不是只读头文件里的宏 ——
 * 读宏只能证明头文件在，证明不了库链上了。
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
// ShellExecuteA（"已经在运行"时把用户领到那个网页上）。**windows.h 不带它** ——
// 少了这一句报的是 `'::ShellExecuteA' has not been declared`，看不出该 include 什么。
#  include <shellapi.h>
#endif

// ---------------------------------------------------------------------------
// FFmpeg
// ---------------------------------------------------------------------------
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

// ---------------------------------------------------------------------------
// Live555
//
// 头文件是四个模块平铺进同一个目录的（含那个唯一的 .h —— NetCommon.h），
// 模块之间用 "xxx.hh" 互相引用，所以只要一个 -I，不要试图按模块分。
// ---------------------------------------------------------------------------
#include "BasicUsageEnvironment.hh"
#include "liveMedia.hh"

// ---------------------------------------------------------------------------
// libuvcpp
//
// 走 C ABI 那一层（capi/）而不是 C++ 层：C 层的版本函数就是为"验一下头与库
// 对不对得上"设计的，而且它们是 extern "C" 的真符号 —— 链接没链上会当场
// 报错，不会像 inline 函数那样悄悄退化成读头文件里的宏。
// ---------------------------------------------------------------------------
#include <uvcpp/uvcpp_version.h>   // UVCPP_VERSION_STRING —— 伞头 capi/uvcpp_c.h 不带它
#include <capi/uvcpp_c.h>

#include "app/capture_session.h"
#include "audio/audio_device.h"
// 通道 2 起的摄像头来源，以及 `--list-cameras` / `--camera`。
#include "capture/webcam_capture.h"
#include "common/audit_log.h"
#include "common/config.h"
#include "common/device_state.h"
#include "common/exit_code.h"
#include "common/firewall.h"
#include "common/ntp_syncer.h"
#include "common/text.h"
#include "common/version.h"
#include "encoder/aac_encoder.h"   // AacEncoder::asc_hex_for —— 起服务器时要预测 ASC
#include "onvif/digest.h"
#include "onvif/net_identity.h"
#include "service/service_host.h"
#include "web/web_server.h"

// `/api/audio/devices` 那份 JSON 要在这里拼（见下面 `wo.audio_devices`）。
// **不手拼字符串**：设备友好名里出现一个 `"` 或 `\` 就能造出一份非法 JSON，
// 而它的症状是"下拉框空着"，与"没插麦克风"一模一样。
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>
#include "capture/display_info.h"
#include "common/log.h"

namespace {

int g_failed = 0;
int g_warned = 0;

void section(char const* title) {
  std::printf("\n%s\n", title);
  for (char const* p = title; *p; ++p) std::putchar('-');
  std::putchar('\n');
}

// 标签列的对齐不能用 printf 的 %-*s：它按**字节**算宽度，而一个汉字是 3 字节，
// 于是纯中文的标签比 ASCII 标签少占一大截，两行右边界对不齐。
// 这里自己算"显示宽度"：按 UTF-8 解码成码点，CJK 那一片算 2 列。
//
// 判据用 cp >= 0x1100：这个界涵盖了韩文、CJK、全角形式、以及常见的 CJK 扩展区。
// 不追求 Unicode 的 East Asian Width 表那么准 —— 这个程序的标签就那么几个字，
// 落在界两侧的都被正确分类了。
std::size_t display_width(std::string const& s) {
  std::size_t w = 0;
  for (std::size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    unsigned cp = c;
    int len = 1;
    if (c >= 0xF0) { cp = c & 0x07u; len = 4; }
    else if (c >= 0xE0) { cp = c & 0x0Fu; len = 3; }
    else if (c >= 0xC0) { cp = c & 0x1Fu; len = 2; }
    for (int k = 1; k < len && i + k < s.size(); ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
    }
    w += (cp >= 0x1100) ? 2 : 1;
    i += static_cast<std::size_t>(len);
  }
  return w;
}

void emit(char const* tag, char const* what, std::string const& detail) {
  std::string const label(what);
  std::size_t const kColumn = 26;
  std::size_t const w = display_width(label);
  std::printf("  [%s] %s", tag, label.c_str());
  std::printf("%*s", static_cast<int>((w < kColumn ? kColumn - w : 1) + 1), "");
  std::printf("%s\n", detail.c_str());
}

void ok(char const* what, std::string const& detail) { emit(" OK ", what, detail); }
void bad(char const* what, std::string const& detail) { emit("FAIL", what, detail); ++g_failed; }
void note(char const* what, std::string const& detail) { emit(" .. ", what, detail); }
void warn(char const* what, std::string const& detail) { emit("WARN", what, detail); ++g_warned; }

std::string fmt_version(unsigned v) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "%u.%u.%u",
                AV_VERSION_MAJOR(v), AV_VERSION_MINOR(v), AV_VERSION_MICRO(v));
  return buf;
}

// `"2026-10-08T15:30:00"`（或空格分隔的那一种）→ **本地**时刻。
//
// 只认 `YYYY-MM-DD[ T]HH:MM:SS` 这一种形状，而且**严格**：位数不对、分隔符
// 不对、范围越界、或者 `mktime` 把越界的日子悄悄"归一化"了（2 月 30 日 →
// 3 月 2 日），一律返回 false。
//
// 宽松解析在这里是有害的：用户的意图是"把设备时间设成这个"，猜错一位就是
// 几小时甚至几个月 —— 而他看到的是"设置成功"，并且水印上印的就是那个错时间。
// 所以宁可报一句"时间格式看不懂"，也不要替他猜。
//
// 走 `std::mktime`（本地时间）而不是 UTC：网页上那个输入框就是不带时区的
// `datetime-local`，用户读的是手边那个钟。**这条路的正确性边界到此为止** ——
// 跨时区的对齐不在本层的职责里（网页本来就只面向本机用户）。
bool parse_local_datetime(std::string const& s, std::time_t& out) {
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  // `%*[ Tt]` 吃掉那个分隔符而不占参数：HTML 的 datetime-local 给的是 `T`，
  // 手打的往往是空格，两种都收。
  int const n = std::sscanf(s.c_str(), "%4d-%2d-%2d%*[ Tt]%2d:%2d:%2d", &y, &mo,
                            &d, &h, &mi, &se);
  if (n != 6) return false;
  if (y < 1970 || y > 2200 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 ||
      h > 23 || mi < 0 || mi > 59 || se < 0 || se > 59) {
    return false;
  }
  std::tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = se;
  // -1 = "按目标日期当地的规则判夏令时"。写 0 的话，夏令时的那几个小时会
  // 静默差一小时，而那正是"设了时间但对不上"最难查的一种。
  tm.tm_isdst = -1;
  std::time_t const t = std::mktime(&tm);
  if (t == static_cast<std::time_t>(-1)) return false;
  // **回读一遍再比。** `mktime` 对越界字段的行为是"归一化"而不是报错，上面
  // 那几条范围检查只挡得住 32 号以外的荒唐值 —— 而 2 月 30 日是**通过**范围
  // 检查的，它会被算成 3 月 2 日。回读比对是唯一能同时挡住这两类的方式。
  std::tm const back = d2o::local_time(t);
  if (back.tm_year != y - 1900 || back.tm_mon != mo - 1 || back.tm_mday != d ||
      back.tm_hour != h || back.tm_min != mi || back.tm_sec != se) {
    return false;
  }
  out = t;
  return true;
}

std::string exe_dir() {
#ifdef _WIN32
  char buf[MAX_PATH] = {0};
  DWORD const n = ::GetModuleFileNameA(nullptr, buf, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return "(未知)";
  std::string s(buf, n);
  std::string::size_type const slash = s.find_last_of("\\/");
  return slash == std::string::npos ? s : s.substr(0, slash);
#else
  return ".";
#endif
}

// ---------------------------------------------------------------------------
// 自检
// ---------------------------------------------------------------------------

void check_ffmpeg() {
  section("FFmpeg");

  note("avutil", av_version_info());
  note("libavcodec", fmt_version(avcodec_version()));
  note("libavformat", fmt_version(avformat_version()));
  note("libswscale", fmt_version(swscale_version()));
  note("libswresample", fmt_version(swresample_version()));

  // libx264 是硬需求：整个编码链路靠它。gyan.dev 的 full_build 才有，
  // essentials 包没有 —— 所以这一项必须真的查一次。
  AVCodec const* x264 = avcodec_find_encoder_by_name("libx264");
  if (x264) {
    ok("libx264 编码器", x264->long_name ? x264->long_name : "(无描述)");
  } else {
    bad("libx264 编码器", "没找到 —— 需要带 libx264 的构建（gyan.dev 的 full_build）");
  }

  // 硬件编码是后续可选项，本阶段只报告，不算失败。
  char const* hw[] = {"h264_nvenc", "h264_qsv", "h264_amf", "h264_mf"};
  std::string found;
  for (char const* name : hw) {
    if (avcodec_find_encoder_by_name(name)) {
      if (!found.empty()) found += ", ";
      found += name;
    }
  }
  note("硬件 H.264 编码器", found.empty() ? "(无，只用 libx264)" : found);

  // configure 行里有 --enable-libx264 之类的开关，出问题时这一行最有用。
  // 它有一两千字符，所以只在最后打印一次，不参与判定。
  char const* cfg = avcodec_configuration();
  if (cfg) {
    note("configure", std::strstr(cfg, "--enable-libx264") ? "有 --enable-libx264"
                                                           : "没有 --enable-libx264");
  }
}

void check_live555() {
  section("Live555");

  ok("liveMedia", LIVEMEDIA_LIBRARY_VERSION_STRING);
  note("groupsock", GROUPSOCK_LIBRARY_VERSION_STRING);
  note("UsageEnvironment", USAGEENVIRONMENT_LIBRARY_VERSION_STRING);
  note("BasicUsageEnvironment", BASICUSAGEENVIRONMENT_LIBRARY_VERSION_STRING);

  // 光读上面那几个宏，库没链上照样能打印 —— 它们只是头文件里的字面量。
  // 真正证明链接成立的是这一段：把事件循环建起来，再让 RTSPServer 绑一个
  // TCP 端口。这一步会依次用到 BasicUsageEnvironment、UsageEnvironment、
  // liveMedia、groupsock 和 ws2_32，五个都得到位才过得去。
  //
  // 顺带把 Phase 2 最容易翻车的地方先验掉：本机能不能绑 554 那一带的端口。
  // 这里用 8554 而不是 554 —— 554 在 Windows 上属于保留端口范围，非管理员
  // 起不来，而 RTSP 端口本来就是可配的。
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  if (!scheduler) {
    bad("事件循环", "BasicTaskScheduler::createNew() 返回空");
    return;
  }
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  RTSPServer* rtsp = RTSPServer::createNew(*env, 8554);
  if (!rtsp) {
    bad("RTSP 端口绑定", std::string("8554 绑不上：") + env->getResultMsg());
  } else {
    char* prefix = rtsp->rtspURLPrefix();
    ok("RTSP 端口绑定", prefix ? prefix : "(URL 前缀取不到)");
    delete[] prefix;
    Medium::close(rtsp);
  }

  // 停机顺序也是 Phase 2 要用的：先 reclaim 再 delete scheduler。
  // reclaim 返回 False 表示还有 source/sink 持有库状态 —— 那是个真问题。
  if (!env->reclaim()) {
    warn("reclaim", "返回 False —— 还有对象没释放，Live555 内部状态没清干净");
  }
  delete scheduler;
}

void check_uvcpp() {
  section("libuvcpp");

  unsigned const abi = uvcpp_c_abi_version();
  if (abi == UVCPP_C_ABI_VERSION) {
    ok("C ABI", "v" + std::to_string(abi) + "（与头一致）");
  } else {
    bad("C ABI", "库是 v" + std::to_string(abi) + "，头是 v" + std::to_string(UVCPP_C_ABI_VERSION));
  }

  // 库版本和头版本必须一致。不一致说明链到了别处的一份 libuvcpp
  // （比如误链了 C:\Users\...\Downloads 里那个 MSVC 预编译包）。
  char const* libver = uvcpp_c_version_string();
  if (!libver) {
    bad("库版本", "uvcpp_c_version_string() 返回空");
  } else if (std::strcmp(libver, UVCPP_VERSION_STRING) == 0) {
    ok("库版本", std::string(libver) + "（与头一致）");
  } else {
    bad("库版本", std::string("库是 ") + libver + "，头是 " + UVCPP_VERSION_STRING);
  }

  // 模块开关：这几个宏来自 configure 期生成的 uvcpp_config.h，写的是
  // "这份库里到底有什么"。SOAP/WSDL 是 Desk2ONVIF 的硬依赖，而且它在所有
  // 预编译包里都是关的 —— 这一项就是为了防止哪天误链回预编译包。
#if defined(UVCPP_WSDL_ENABLE) && UVCPP_WSDL_ENABLE
  ok("SOAP/WSDL 模块", "已编入");
#else
  bad("SOAP/WSDL 模块", "未编入 —— ONVIF 的 SOAP 信封层不可用，必须用源码构建版");
#endif

#if defined(UVCPP_WEBAPP_ENABLE) && UVCPP_WEBAPP_ENABLE
  note("webapp 模块", "已编入");
#else
  bad("webapp 模块", "未编入 —— HTTP/WebSocket 层不可用");
#endif

#if defined(UVCPP_NET_ENABLE) && UVCPP_NET_ENABLE
  note("net 模块", "已编入");
#else
  bad("net 模块", "未编入");
#endif

#if defined(UVCPP_CAPI_ENABLE) && UVCPP_CAPI_ENABLE
  note("C ABI 模块", "已编入");
#else
  bad("C ABI 模块", "未编入 —— 本自检用的就是它");
#endif

  // 这两项证明 DLL 真的加载起来了：符号解析发生在加载期，
  // 少一个 DLL 是进程根本起不来，而不是走到这里报错。
  note("活句柄数", std::to_string(uvcpp_c_live_handle_count()));
  char const* stale = uvcpp_c_strerror(UVCPP_C_E_STALE);
  note("错误码文案", stale ? stale : "(空)");
}

// Phase 4 的鉴权。这一段的价值全在于"拿别人的向量验自己" ——
// 如果只用 DigestAuth 自己生成挑战、自己校验，那么服务端和客户端**一起算错**
// 时它照样全绿，而症状要等到真 NVR 连不上才暴露。
void check_digest() {
  section("Digest 鉴权");

  // RFC 2617 §3.5 的官方测试向量。里面每一个中间值都写进了规范正文，
  // 所以对不上就是实现错了，不存在"标准这么写的但实际不是"的余地。
  struct Vector {
    char const* method;
    char const* uri;
    char const* user;
    char const* realm;
    char const* pass;
    char const* nonce;
    char const* qop;
    char const* nc;
    char const* cnonce;
    char const* want;
  };
  Vector const kRfc2617{"GET", "/dir/index.html", "Mufasa", "testrealm@host.com",
                        "Circle Of Life", "dcd98b7102dd2f0e8b11d0f600bfb0c093",
                        "auth", "00000001", "0a4f113b",
                        "6629fae49393a05397450978507c4ef1"};

  std::string const got =
      d2o::digest_response_hex(kRfc2617.method, kRfc2617.uri, kRfc2617.user,
                               kRfc2617.realm, kRfc2617.pass, kRfc2617.nonce,
                               kRfc2617.qop, kRfc2617.nc, kRfc2617.cnonce);
  if (got == kRfc2617.want) {
    ok("RFC 2617 测试向量", "qop=auth 摘要与规范一致");
  } else {
    bad("RFC 2617 测试向量",
        "期望 " + std::string(kRfc2617.want) + "，实得 " + (got.empty() ? "(空)" : got));
  }

  // RFC 2069 的老式算法（没有 qop）。ONVIF 客户端里偶尔碰到。
  std::string const legacy = d2o::digest_response_hex(
      "GET", "/dir/index.html", "Mufasa", "testrealm@host.com", "Circle Of Life",
      "dcd98b7102dd2f0e8b11d0f600bfb0c093", "", "", "");
  // 同一组输入，只是按 RFC 2069 的三段式：MD5(HA1:nonce:HA2)，其中
  // HA1=939e7578...、HA2=39aff3a2... 都是 RFC 正文里印着的值。
  if (legacy == "670fd8c2df070c60b045671b8b24ff02") {
    ok("RFC 2069 老式算法", "无 qop 的摘要正确");
  } else {
    bad("RFC 2069 老式算法", "实得 " + (legacy.empty() ? "(空)" : legacy));
  }

  // 走一遍完整的收发：服务端发挑战 -> 客户端算应答 -> 服务端校验。
  // 这验的是挑战生成、nonce 完整性校验、Authorization 解析这条**管道**，
  // 而上面两个向量只验算法本身。两者缺一不可。
  d2o::DigestAuth auth("Desk2ONVIF", "admin", "s3cret");
  std::string const challenge = auth.challenge(false);
  d2o::DigestResponse const ch = d2o::parse_authorization(challenge);

  if (ch.nonce.empty() || ch.realm.empty()) {
    bad("挑战生成", "从 WWW-Authenticate 里解析不出 nonce/realm：" + challenge);
  } else {
    std::string const response =
        d2o::digest_response_hex("POST", "/onvif/device_service", "admin", ch.realm,
                                 "s3cret", ch.nonce, "auth", "00000001", "abcdef");
    std::string const header = "Digest username=\"admin\", realm=\"" + ch.realm +
                               "\", nonce=\"" + ch.nonce +
                               "\", uri=\"/onvif/device_service\", qop=auth, "
                               "nc=00000001, cnonce=\"abcdef\", response=\"" +
                               response + "\"";

    bool stale = false;
    bool const good = auth.verify("POST", "/onvif/device_service",
                                  "http://192.168.1.5/onvif/device_service", header, stale);
    bool const bad_pw = auth.verify(
        "POST", "/onvif/device_service", "http://192.168.1.5/onvif/device_service",
        "Digest username=\"admin\", realm=\"" + ch.realm + "\", nonce=\"" + ch.nonce +
            "\", uri=\"/onvif/device_service\", qop=auth, nc=00000001, "
            "cnonce=\"abcdef\", response=\"00000000000000000000000000000000\"",
        stale);

    if (good && !bad_pw) {
      ok("挑战/应答往返", "正确口令通过，错误摘要被拒");
    } else if (!good) {
      bad("挑战/应答往返", "正确口令被拒 —— nonce 或摘要管道有问题");
    } else {
      bad("挑战/应答往返", "错误摘要被放行了 —— 这是鉴权失效，必须修");
    }
  }

  // 过期 nonce 必须带着 stale=true 走，否则客户端会再弹一次登录框。
  d2o::DigestAuth short_lived("Desk2ONVIF", "admin", "s3cret");
  short_lived.set_nonce_ttl(0);  // 立刻过期
  std::string const c2 = short_lived.challenge(false);
  d2o::DigestResponse const ch2 = d2o::parse_authorization(c2);
  std::string const resp2 =
      d2o::digest_response_hex("GET", "/x", "admin", ch2.realm, "s3cret", ch2.nonce,
                               "auth", "00000001", "cn");
  bool stale2 = false;
  bool const accepted =
      short_lived.verify("GET", "/x", "", "Digest username=\"admin\", realm=\"" +
                                              ch2.realm + "\", nonce=\"" + ch2.nonce +
                                              "\", uri=\"/x\", qop=auth, nc=00000001, "
                                              "cnonce=\"cn\", response=\"" + resp2 + "\"",
                         stale2);
  if (!accepted && stale2) {
    ok("nonce 过期", "被拒且置 stale=true（客户端不会重复弹框）");
  } else if (!accepted) {
    warn("nonce 过期", "被拒了，但没置 stale=true —— 客户端可能反复弹登录框");
  } else {
    bad("nonce 过期", "TTL=0 的 nonce 仍被接受");
  }
}

void check_build() {
  section("构建环境");

  note("编译器", __VERSION__);
  note("C++ 标准", std::to_string(__cplusplus / 100 % 100) + "（__cplusplus=" + std::to_string(__cplusplus) + "）");
  note("指针宽度", std::to_string(sizeof(void*) * 8) + " 位");
  note("可执行文件目录", exe_dir());
}

int cmd_selfcheck() {
  // 版本号放第一行：自检报告贴进 issue 时，它得和 exe 属性、包名是同一个数。
  // 这个数来自 `project(... VERSION)`（见 common/version.h），不是手抄的。
  std::printf("Desk2ONVIF %s 自检 —— 目的只有一个：证明依赖真的链上、真的能加载。\n",
              d2o::kVersion);

  check_build();
  check_ffmpeg();
  check_live555();
  check_uvcpp();
  check_digest();

  section("结论");
  if (g_failed == 0 && g_warned == 0) {
    std::printf("  全部通过。工具链、链接顺序、运行期 DLL 部署三件事同时成立。\n");
    return 0;
  }
  if (g_failed == 0) {
    std::printf("  通过，但有 %d 条警告（不影响本阶段继续）。\n", g_warned);
    return 0;
  }
  std::printf("  有 %d 项失败。\n", g_failed);
  return 1;
}

/// `--version` / `-v`：**只打一行**，别的什么都不做。
///
/// 为什么和 `--selfcheck` 分开（它们原先走的是同一个函数）：自检要建事件循环、
/// 要绑端口、要加载三套依赖，任何一步出岔子都会让它退非零 —— 而"问一下版本号"
/// 这件事在这些情况下**恰恰最需要能成功**（用户拿着一个起不来的安装包，第一句
/// 想确认的就是"我装的是哪个版本"）。一个会失败的版本查询不是版本查询。
///
/// 输出格式固定成 `Desk2ONVIF <版本>`：`scripts\package.ps1` 与两个验收脚本要
/// 拿它和 exe 的 VersionInfo、包名三方对账，所以这行**不能带别的东西**。
int cmd_version() {
  std::printf("Desk2ONVIF %s\n", d2o::kVersion);
  return 0;
}

// ---------------------------------------------------------------------------
// 命令行
// ---------------------------------------------------------------------------

struct ParsedArgs {
  // 值型选项：--key value 或 --key=value
  std::vector<std::pair<std::string, std::string>> values;
  // 开关型选项：只出现 --key，后面不跟值
  std::vector<std::string> flags;
  std::vector<std::string> rest;

  bool has(std::string const& k) const {
    for (auto const& f : flags) if (f == k) return true;
    for (auto const& v : values) if (v.first == k) return true;
    return false;
  }
  std::string value(std::string const& k, std::string const& fallback) const {
    for (auto const& v : values) if (v.first == k) return v.second;
    return fallback;
  }
};

/// 只认长选项。含 `=` 的拆开；不含 `=` 的，看下一个参数是不是值。
///
/// 这里显式列出哪些是开关而不是靠"下一个参数以 -- 开头"来判断：
/// 后者在 `--record --seconds 5` 这种写法下会把 `--seconds` 当成文件名。
///
/// **这张表漏一个开关，那个开关就会吞掉它后面的参数。** `--no-rtsp --http`
/// 在 `--no-rtsp` 不在表里时会被解析成"`--no-rtsp` 的值是 `--http`"，
/// 于是 `--http` 整个消失、Web 服务静默不开，而命令行看上去完全正常。
/// 加新开关时必须同时加到这里。
ParsedArgs parse_args(std::vector<std::string> const& argv) {
  static char const* const kFlagNames[] = {
      "--selfcheck", "--version", "-v", "--help", "-h",
      "--list-displays", "--list-audio", "--list-cameras",
      "--verbose", "--quiet", "--debug",
      "--rtsp", "--http",
      "--onvif", "--no-rtsp", "--no-http", "--no-onvif", "--no-wsd",
      "--sub", "--no-sub", "--save-config", "--no-http-auth",
      // 水印与自适应。**只有不带值的开关能进这张表** —— 它的作用是
      // "别把下一个参数当成我的值"。带值的选项（--osd-name / --max-height /
      // --adaptive）写进来会让它们永远取不到值，而命令行看上去完全正常。
      "--osd", "--no-osd", "--no-adaptive",
      // 音频。同样只有裸开关：`--audio`（system/mic/both）和 `--audio-device`
      // 是带值的。
      "--no-audio",
      // 进程形态。同样**只放不带值的两个**：`--service`（run/install/…）、
      // `--parent-pid`、`--handoff` 都是带值的，写进来会吞掉它们的值 ——
      // 而症状是"服务起来了、worker 也在跑，就是没有父进程看守"。
      "--worker", "--console", "--start",
      // 审计日志。**只有不带值的两个进这张表**：`--no-audit` / `--audit`
      // 都是裸开关，而 `--audit-keep-days` 带值 —— 后者放进来它会永远取不到
      // 值（症状：保留天数被静默忽略，而命令行看上去完全正常）。
      // 两个方向都给：`--no-audit` 是"这次别记"，`--audit` 是"配置里关着，
      // 这一次打开" —— 只有一个方向的话，"配了 false 之后想临时打开"
      // 就变成必须去改配置文件，而那时你**正是不想让它白纸黑字留着**。
      "--no-audit", "--audit"};
  auto is_flag_name = [](std::string const& s) {
    for (char const* f : kFlagNames) if (s == f) return true;
    return false;
  };

  ParsedArgs out;
  for (std::size_t i = 0; i < argv.size(); ++i) {
    std::string const& a = argv[i];
    if (a.size() >= 2 && a[0] == '-' && a[1] == '-') {
      std::string::size_type const eq = a.find('=');
      if (eq != std::string::npos) {
        out.values.emplace_back(a.substr(0, eq), a.substr(eq + 1));
      } else if (is_flag_name(a)) {
        out.flags.push_back(a);
      } else if (i + 1 < argv.size()) {
        out.values.emplace_back(a, argv[++i]);
      } else {
        out.values.emplace_back(a, std::string());  // 缺值，后面读的时候报错
      }
    } else if (a == "-h" || a == "-v") {
      out.flags.push_back(a);
    } else {
      out.rest.push_back(a);
    }
  }
  return out;
}

/// 取一个整数选项，带范围检查。p 为 nullptr 时返回 fallback。
bool int_option(ParsedArgs const& args, std::string const& key, int fallback,
                int lo, int hi, int& out, std::string& err) {
  if (!args.has(key)) {
    out = fallback;
    return true;
  }
  std::string const s = args.value(key, std::string());
  if (s.empty()) {
    err = key + " 后面缺数值";
    return false;
  }
  try {
    std::size_t used = 0;
    int const v = std::stoi(s, &used);
    if (used != s.size()) throw std::invalid_argument("tail");
    if (v < lo || v > hi) {
      err = d2o::fmt("%s 的值 %d 超出范围 [%d, %d]", key.c_str(), v, lo, hi);
      return false;
    }
    out = v;
    return true;
  } catch (std::exception const&) {
    err = d2o::fmt("%s 的值 \"%s\" 不是整数", key.c_str(), s.c_str());
    return false;
  }
}

void print_usage() {
  // **注意这里是 printf 的格式串，`%` 是有含义的。** 中间那行 `%APPDATA%`
  // 不转义的话，`%A` 会被当成"十六进制浮点"转换符去读一个根本不存在的参数
  // —— 是未定义行为，编译期也会报 -Wformat。写成 %%APPDATA%% 才对。
  std::printf(
      "Desk2ONVIF —— 把 Windows 桌面模拟成一台 ONVIF Profile S 摄像机\n"
      "\n"
      "用法：desk2onvif <命令> [选项]\n"
      "\n"
      "命令：\n"
      "  （不带参数）           按控制台形态跑起来 —— 双击 exe 用的就是这个入口\n"
      "  --console              同上，显式写出来（给快捷方式、自启项用）\n"
      "  --selfcheck            比对三套依赖的版本与能力，验完即退\n"
      "  -v, --version          只打一行 `Desk2ONVIF <版本号>` 然后退出。**不加载\n"
      "                         任何依赖** —— 自检会失败的时候（缺 DLL、端口被占），\n"
      "                         这一条仍然要能回答\"装的是哪个版本\"\n"
      "  --list-displays        列出显示输出，并逐个探测能不能录\n"
      "  --list-cameras         列出本机摄像头（含它们要给 --camera / 配置里\n"
      "                         channels.items[].camera 的符号链接）\n"
      "  --list-audio           列出播放设备与录音设备（含它们要给 --audio-device 的 id）\n"
      "  --record <文件.mp4>     录制桌面到 MP4 文件（跑完就退）\n"
      "  --rtsp                 开 RTSP 推流（可以和 --record 一起给）\n"
      "  --http                 开 Web 服务（REST + WebSocket + Web UI）\n"
      "\n"
      "     上面三条（--record / --rtsp / --http）是**一次性**形态：跑到 --seconds\n"
      "     用完就退出。不给这三条里的任何一条，就是 --console 那种长驻形态。\n"
      "     长驻形态下，**配置文件不存在时** Web 与 RTSP 按出厂设置打开；存过一次\n"
      "     配置之后就听配置的（这样网页上的开关才是真的能关掉）。\n"
      "     长驻形态会把日志同时写一份到**配置文件旁边**的 desk2onvif.log。\n"
      "\n"
      "Windows 服务：\n"
      "  --service install      装成开机自启的服务，并立刻启动（需要管理员，\n"
      "                         不在管理员上下文里会自己弹一次 UAC）\n"
      "  --service uninstall    先停再删\n"
      "  --service status       看状态（只读，不需要管理员）\n"
      "  --service print        只把要做的每一步打出来，什么都不改\n"
      "  --start                给 install 用：装完立刻启动\n"
      "  --worker               被服务拉起的形态（内部用）。它和 --console 是同一个\n"
      "                         程序，区别只是多一个 --parent-pid\n"
      "  --parent-pid <pid>     盯住这个进程，它一没就**优雅收尾**退出。用的是进程\n"
      "                         句柄而不是命名事件：Local\\ 命名空间按会话隔离，\n"
      "                         服务在 Session 0、worker 在 Session 1，同名事件在\n"
      "                         两边根本不是一个对象\n"
      "  --handoff <毫秒>       启动时等这么久去抢单实例锁（控制台形态自重启用）\n"
      "     服务只负责在**交互用户的会话**里拉起 worker 并看门狗它 —— 服务自己在\n"
      "     Session 0，而 DXGI 桌面复制在那里抓不到用户桌面，所以采集必须由 worker 做。\n"
      "     服务自己的日志在 %%ProgramData%%\\Desk2ONVIF\\service.log。\n"
      "\n"
      "采集与编码：\n"
      "  --display <N>          要录哪个输出，N 是 --list-displays 里的序号（默认 0）\n"
      "  --camera <名|链接>     **把通道 1 的画面来源换成摄像头**（默认取第一台）。\n"
      "                         名字或符号链接都认，写法见 --list-cameras。给了它\n"
      "                         --display 就不起作用了。通道 2 起的摄像头要写进\n"
      "                         配置文件的 channels.items[]\n"
      "  --seconds <N>          跑多少秒；不给就一直跑到 Ctrl+C\n"
      "  --fps <N>              帧率（默认 25）\n"
      "  --bitrate <kbps>       码率（默认 2048）\n"
      "  --gop <秒>             关键帧间隔（默认 2）\n"
      "  --max-height <N>       主码流高度上限，只用上限不放大（默认 0 = 桌面原生）。\n"
      "                         采集永远按桌面原生尺寸走 —— 采集时就缩放会让\n"
      "                         鼠标指针的位置整体偏掉\n"
      "  --profile <名>         baseline | main | high（默认 main）\n"
      "  --preset <名>          x264 preset（默认 ultrafast，即「最快」）\n"
      "  --tune <名>            x264 tune（默认 zerolatency，即「零延迟」）。**不要\n"
      "                         改成 film/animation 这类**：它们会开 B 帧和前瞻，\n"
      "                         直播延迟会从百毫秒级涨到秒级\n"
      "  --threads <N>          编码线程数（默认取配置里的 capture.threads，全新\n"
      "                         配置是 3 = 「均衡」档）。帧级并行下**每个线程多\n"
      "                         一帧延迟**：调大提吞吐、调小降延迟。配置里存 0\n"
      "                         表示\"按延迟优先自动选（封顶 6）\"，命令行不给 0\n"
      "  --latency <档>         realtime | balanced | quality | custom（延时档位）。\n"
      "                         三档把 threads/gop_seconds 钉成 1/1、3/2、6/4，\n"
      "                         custom = 这两个字段谁都不动。**preset / tune /\n"
      "                         profile 三档一律不动** —— 放松 preset 会把\n"
      "                         H.264 profile 从 Constrained Baseline 换成 Main，\n"
      "                         而 NVR 兼容性、GetProfiles 报出去的参数、子码流\n"
      "                         解码开销没有一样在这个档位表上\n"
      "  --queue <N>            采集队列深度（默认 4）\n"
      "\n"
      "水印（OSD）与自适应：\n"
      "  --osd / --no-osd       开关水印（默认按配置，出厂配置是**开**）\n"
      "  --osd-name <文字>      把「名称」那一项的文字定死成这个，并打开它。\n"
      "                         **定死之后就不再跟着 ONVIF 的改名走了**；\n"
      "                         要跟随就用网页上的水印设置，把文字留空\n"
      "  --adaptive <模式>      off | throttle | auto（默认 throttle）：\n"
      "                         off = 不干预；throttle = 编码跟不上就自动丢帧，\n"
      "                         丢到帧率下限仍然不够才在网页上提示降画质；\n"
      "                         auto = 连提示都不用，直接降到下一档重开管线\n"
      "  --no-adaptive          等于 --adaptive off\n"
      "     默认两项水印：**时间在左上角、名称在右下角**，黑底白字、白底黑字\n"
      "     自适应配色。位置上、字号上、日期格式上要改的走网页，命令行不给。\n"
      "\n"
      "RTSP：\n"
      "  --rtsp-port <N>        端口（默认 554，NVR 一般也用这个）\n"
      "  --rtsp-path <名>       地址里的路径（默认 live）\n"
      "  --rtsp-queue <N>       推流队列深度，按 NALU 计（默认 3 秒的量）\n"
      "  --sub                  开子码流（默认就是开的，见下）\n"
      "  --no-sub               关掉子码流\n"
      "  --sub-path <名>        子码流路径（默认 live_sub）\n"
      "  --sub-height <N>       子码流高度（默认 360），宽度按主码流比例推\n"
      "  --sub-bitrate <kbps>   子码流码率（默认 512）\n"
      "     子码流是**第二路编码器**，专门给 NVR 的多画面网格用：NVR 默认拉的是\n"
      "     主码流，于是 16 路网格会同时拉 16 条全分辨率的流。开着它，网格走子码流、\n"
      "     点开单画面才切主码流。只有开了 --rtsp 它才有意义。\n"
      "\n"
      "音频（AAC）：\n"
      "  --audio system|mic|both  录一条音轨：system = 系统声音（环回采集），\n"
      "                         mic = 麦克风，both = 两个混在一起。**不给就是不开**\n"
      "  --no-audio              关掉音轨。配置里开着音频时，用这个临时录一份纯视频\n"
      "  --audio-device <id>     用哪个麦克风，id 从 --list-audio 抄（留空 = 系统默认）\n"
      "     音轨**同时**进 MP4 和 RTSP 主码流（NVR 拉的那条流里就有声），预览页静音。\n"
      "     码率和两路增益在网页上配。默认**不开**：打开会改变每一台设备的 SDP，\n"
      "     而 NVR 的兼容性只能靠实测 —— **加上音频之后 NVR 取流异常，先关掉它试试**。\n"
      "\n"
      "Web 服务：\n"
      "  --http-host <地址>     监听地址（默认 0.0.0.0，即同局域网都能访问）\n"
      "  --http-port <N>        端口（默认 8080）\n"
      "  --web-root <目录>      静态资源根目录（默认 web/dist，不存在就只提供 API）\n"
      "  --http-user <名>       Web 控制台登录用户名（默认 admin）\n"
      "  --http-password <口令>  Web 控制台登录口令（默认 admin）。**这一对和\n"
      "                         ONVIF 那一对是分开的**：NVR 存着的是机器凭据，\n"
      "                         这里的是人手敲的，改一个不会牵动另一个\n"
      "  --no-http-auth         关掉 Web 控制台登录（默认是开的）。**只在\n"
      "                         --http-host 127.0.0.1 时才是合理的选择**\n"
      "\n"
      "ONVIF：\n"
      "  --onvif                打开 ONVIF 的 SOAP 端点（默认就是开的，见下）\n"
      "  --onvif-user <名>      Digest 用户名（默认 admin）\n"
      "  --onvif-password <口令>  Digest 口令（默认 admin）。**NVR 添加通道时会问你\n"
      "                         要的\"摄像机用户名/密码\"就是这一对**\n"
      "  --no-onvif             关掉 ONVIF 的 SOAP 端点（/onvif/*）\n"
      "  --no-wsd               关掉 WS-Discovery（UDP 3702），NVR 就只能手工填地址\n"
      "                         （WS-Discovery 跟着 ONVIF 走，关掉 ONVIF 它也关）\n"
      "\n"
      "操作日志（审计）：\n"
      "  --no-audit             这一次不记操作日志（配置里那个开关照旧，不被改写）\n"
      "  --audit                这一次记（配置里关着时用这个临时打开）\n"
      "  --audit-keep-days <N>  留存天数（默认 180 = 半年，0 = 永不剪枝）\n"
      "     记的是**改动类 + 登录**：网页上改配置/重启/恢复出厂/对时、NVR 发来的\n"
      "     Set*、以及登录成功与失败；**不记查看类**（开页面、看快照）。\n"
      "     一条一行，落在**配置文件旁边**的 logs\\audit-YYYY-MM-DD.log，按天分文件。\n"
      "     口令、Authorization 头、nonce、token **永不写入** —— 改口令只记「改过」。\n"
      "     恢复出厂会清空它（连同 desk2onvif.log）。\n"
      "\n"
      "配置文件：\n"
      "  --config <文件>        配置文件路径（默认 %%APPDATA%%\\Desk2ONVIF\\config.json）\n"
      "                         文件里的值作为默认值，**命令行参数一律覆盖它**\n"
      "  --save-config          把这次实际生效的配置写进配置文件，然后照常运行\n"
      "\n"
      "防火墙：\n"
      "  --firewall status      看当前配置对应的入站规则在不在（只读，不需要管理员）\n"
      "  --firewall add         放行它们（需要管理员）\n"
      "  --firewall remove      删掉它们（需要管理员）\n"
      "  --firewall print       只把 netsh 命令打出来，自己拿去执行\n"
      "                         **NVR 搜不到/连不上这台机器时，先查这个** ——\n"
      "                         本机 ffplay 拉流走回环、不受防火墙影响，所以\n"
      "                         \"本地一切正常、别的机器连不上\"是它的典型症状\n"
      "\n"
      "通用：\n"
      "  --verbose / --debug    打开调试日志\n"
      "  --quiet                只打错误\n"
      "  -h, --help             这段文字\n"
      "  -v, --version          版本号（同上，放在这里是因为它也叫通用选项）\n");
}

int cmd_list_displays() {
  std::vector<d2o::OutputInfo> const outputs = d2o::enumerate_outputs();
  if (outputs.empty()) {
    std::printf("没有找到任何显示输出。\n");
    return 1;
  }

  std::printf("发现 %zu 个显示输出。\n\n", outputs.size());
  int capturable = 0;
  for (std::size_t i = 0; i < outputs.size(); ++i) {
    d2o::OutputInfo const& o = outputs[i];
    d2o::CaptureProbe const p = d2o::probe_capturable(o);
    if (p.ok) ++capturable;
    std::printf("  #%zu  %s\n", i, d2o::describe(o).c_str());
    std::printf("       %s\n", p.ok ? "可以捕获" : p.message.c_str());
    std::printf("       适配器 %u 输出 %u，厂商 0x%04X 设备 0x%04X\n\n",
                o.adapter_index, o.output_index, o.vendor_id, o.device_id);
  }

  std::printf("其中 %d 个可以捕获。用 --record out.mp4 --display <序号> 指定。\n",
              capturable);
  if (capturable == 0) {
    std::printf("\n一个都录不了。常见原因：只有虚拟显示器（向日葵 / Parsec / "
                "usbmmidd 这类间接显示驱动）—— DXGI 桌面复制抓不到 IDD 输出。\n");
    return 1;
  }
  return 0;
}

/// `--list-cameras`：把本机摄像头列出来。
///
/// 存在的理由和 `--list-displays` 一字不差：**这是唯一能回答"这台机器上能选
/// 哪一个摄像头"的东西**，而摄像头这边还多一层 —— 配置里 `channels.items[].camera`
/// 要填的是那个**符号链接**（一整串 `\\?\usb#vid_...`），用户对着"FHD Camera"
/// 这个名字没有任何办法把它翻译过去。所以两个都印：名字给人看，链接给人抄。
int cmd_list_cameras() {
  std::string err;
  std::vector<d2o::CameraInfo> const cams = d2o::enumerate_cameras(&err);
  if (!err.empty()) {
    // **"枚举不了"和"一台都没有"要分开报**，理由同 `list_audio_endpoints`：
    // 混成一句话，用户会去插拔一个根本没坏的东西。
    std::printf("枚举摄像头失败：%s\n", err.c_str());
    return 1;
  }
  if (cams.empty()) {
    std::printf("没有找到任何摄像头。\n");
    return 1;
  }
  std::printf("发现 %zu 个摄像头。\n\n", cams.size());
  for (std::size_t i = 0; i < cams.size(); ++i) {
    d2o::CameraInfo const& c = cams[i];
    std::printf("  #%zu  %s%s\n", i, c.name.c_str(),
                c.is_default ? "（默认）" : "");
    std::printf("       %s\n\n", c.symbolic_link.c_str());
  }
  // 配置里写名字也认（`WebcamSource` 的匹配规则：符号链接精确 → 名字精确 →
  // 子串 → 不区分大小写）。写进配置的**推荐**符号链接，因为它不会因为接了
  // 第二台同名摄像头而指错。
  std::printf("配置里写 `channels.items[].camera`：填上面的符号链接最稳（不会"
              "因为接了第二台同名摄像头而指错），填名字也行。\n");
  std::printf("命令行试一下：--camera \"%s\"\n", cams[0].symbolic_link.c_str());
  return 0;
}

/// `--list-audio`：把系统上的播放设备与录音设备列出来。
///
/// 存在的理由和 `--list-displays` 一模一样：**这是唯一能回答"这台机器上能选
/// 哪个设备"的东西**。而音频这边更需要它 —— 设备的友好名（"扬声器 (Realtek(R)
/// Audio)"）和配置里要填的那个 id（一长串 `{0.0.0.00000000}.{...}`）**完全是
/// 两回事**，而配置里只能存 id。用户对着系统设置里的名字，没有任何办法把它
/// 翻译成 id。
///
/// 两个方向分开列（不是一张混起来的表）：「系统声音」只能从**渲染**端点环回，
/// 麦克风是**采集**端点。混在一起的话，用户会在麦克风那一栏选到一个扬声器，
/// 然后得到一个永远静音的音轨 —— 而 SDP 和 ffprobe 看上去都正常。
int cmd_list_audio() {
  struct Group {
    d2o::AudioFlow flow;
    char const* title;
    char const* usage;
  };
  Group const groups[] = {
      {d2o::AudioFlow::Render,
       "播放设备 —— 「系统声音」从这里环回采（含虚拟声卡）",
       "--audio system / audio.system=true，设备留空就是用默认的那个"},
      {d2o::AudioFlow::Capture,
       "录音设备 —— 麦克风",
       "--audio mic / audio.microphone=true，设备填下面的 id（留空用默认）"},
  };

  int failed = 0;
  std::size_t total = 0;
  for (Group const& g : groups) {
    std::string err;
    std::vector<d2o::AudioEndpoint> const eps = d2o::list_audio_endpoints(g.flow, err);
    std::printf("%s\n", g.title);
    if (!err.empty()) {
      // 枚举失败和"一个设备都没有"必须分开报：混在一起的话，
      // "没插麦克风"和"COM 没起来"在输出上长得一模一样。
      std::printf("  枚举失败：%s\n\n", err.c_str());
      ++failed;
      continue;
    }
    if (eps.empty()) {
      std::printf("  （这台机器上一个都没有）\n\n");
      continue;
    }
    total += eps.size();
    for (d2o::AudioEndpoint const& ep : eps) {
      std::printf("  %-6s %s\n", ep.is_default ? "[默认]" : "", ep.name.c_str());
      std::printf("         %s\n", ep.id.c_str());
    }
    std::printf("  用哪个：%s\n\n", g.usage);
  }

  if (failed > 0) return 1;
  if (total == 0) {
    std::printf("一个音频端点都没有。没有声卡的机器上这是正常的，"
                "录音只能录画面。\n");
    return 1;
  }
  return 0;
}

/// 防火墙规则辅助：`--firewall status | add | remove | print`。
///
/// **这个子命令只会在被显式调用时改防火墙。** 跑服务（`--rtsp`/`--http`）时
/// 一行规则都不会碰 —— 一个录屏工具不声不响地把自己的端口对局域网开放，
/// 是使用者从没同意过的事。
///
/// 端口从配置 + 命令行按**和 cmd_run 相同的规则**解析。这段解析是重复的，
/// 因为 cmd_run 把"生效值"算在它自己内部（顺序上和 WebServer 的构造绑在
/// 一起），抽出来不值当；但两处的口径必须一致 —— 防火墙放行的端口如果不是
/// 程序真正监听的端口，那这张规则表就只是一份好看的文件。
int cmd_firewall(ParsedArgs const& args) {
  std::string const mode = args.value("--firewall", std::string());
  if (mode != "status" && mode != "add" && mode != "remove" && mode != "print") {
    std::fprintf(stderr,
                 "--firewall 后面要跟 status / add / remove / print 之一，"
                 "给的是 \"%s\"。\n",
                 mode.c_str());
    return 2;
  }

  std::string const config_path = args.value("--config", d2o::default_config_path());
  d2o::AppConfig cfg;
  {
    std::string cerr_msg;
    bool existed = false;
    if (!d2o::load_config(cfg, config_path, cerr_msg, &existed)) {
      std::fprintf(stderr, "配置文件读不了：%s\n  %s\n", config_path.c_str(),
                   cerr_msg.c_str());
      return 2;
    }
  }

  // 只有"真的会监听"的端口才需要放行。给一个没开的服务开端口，是把攻击面
  // 白白扩大一块。
  bool const rtsp = (args.has("--rtsp") || cfg.rtsp.enabled) && !args.has("--no-rtsp");
  bool const http = (args.has("--http") || cfg.web.enabled) && !args.has("--no-http");
  bool const onvif = (cfg.onvif.enabled || args.has("--onvif")) && !args.has("--no-onvif");
  bool const wsd = http && onvif && !args.has("--no-wsd");

  std::string err;
  int rtsp_port = cfg.rtsp.port;
  if (!int_option(args, "--rtsp-port", cfg.rtsp.port, 1, 65535, rtsp_port, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }
  int http_port = cfg.web.port;
  if (!int_option(args, "--http-port", cfg.web.port, 1, 65535, http_port, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  std::vector<d2o::FirewallRule> rules;
  if (rtsp) {
    rules.push_back({d2o::firewall_rule_prefix() + "RTSP", "TCP", rtsp_port,
                     "NVR / VLC / ffplay 拉流"});
  }
  if (http) {
    rules.push_back({d2o::firewall_rule_prefix() + "HTTP", "TCP", http_port,
                     "Web 配置界面，以及 ONVIF 的 SOAP 端点（同一个端口）"});
  }
  if (wsd) {
    rules.push_back({d2o::firewall_rule_prefix() + "WSDiscovery", "UDP", 3702,
                     "WS-Discovery 组播，NVR\"自动发现\"靠它"});
  }

  if (rules.empty()) {
    std::printf("按当前配置（配置 %s + 命令行），一个服务都没开，没有端口要放行。\n",
                config_path.c_str());
    std::printf("先给 --rtsp 或 --http，再跑这个命令。\n");
    return 0;
  }

  if (mode == "print") {
    std::printf("下面这些命令需要**管理员**权限，逐条执行：\n\n");
    for (auto const& r : rules) {
      // 用和 `--firewall add` 完全同一个函数拼串：打印出来的就是它真正会跑的
      // 那一条。各拼一份的话，改了一处没改另一处，"照着提示抄却没用"——
      // 而提示本身看上去毫无破绽。
      std::printf("netsh %s\n", d2o::firewall_rule_add_args(r).c_str());
    }
    std::printf("\n删除：把 add 换成 delete，去掉 action= 之后的部分。\n");
    std::printf("（`--firewall add` 干的就是这件事，只是它自己会先删后建。）\n");
    return 0;
  }

  if (mode == "status") {
    std::printf("当前配置对应的入站规则（只查，不改）：\n\n");
    int present = 0;
    bool ok = true;
    for (auto const& r : rules) {
      bool exists = false;
      std::string qerr;
      if (!d2o::firewall_rule_query(r.name, exists, qerr)) {
        std::fprintf(stderr, "查询 %s 失败：%s\n", r.name.c_str(), qerr.c_str());
        ok = false;
        continue;
      }
      if (exists) ++present;
      std::printf("  [%s] %-28s %s %-5d  %s\n", exists ? "有" : "没有", r.name.c_str(),
                  r.protocol.c_str(), r.port, r.why.c_str());
    }
    if (!ok) return 1;
    std::printf("\n%d/%zu 条已就位。\n", present, rules.size());
    if (present < static_cast<int>(rules.size())) {
      std::printf("缺的那些加上：desk2onvif --firewall add%s\n",
                  rtsp ? " --rtsp" : " --http");
      if (!d2o::is_elevated()) {
        std::printf("（当前不是管理员，改不了防火墙。%s）\n",
                    d2o::elevation_hint().c_str());
      }
    }
    return 0;
  }

  // add / remove 都要管理员。
  if (!d2o::is_elevated()) {
    std::fprintf(stderr, "%s\n", d2o::elevation_hint().c_str());
    std::fprintf(stderr, "\n或者先看看现在是什么状态：desk2onvif --firewall status\n");
    return 1;
  }

  int failures = 0;
  for (auto const& r : rules) {
    std::string aerr;
    bool const good = (mode == "add") ? d2o::firewall_rule_add(r, aerr)
                                      : d2o::firewall_rule_remove(r.name, aerr);
    if (!good) {
      std::fprintf(stderr, "%s\n", aerr.c_str());
      ++failures;
    }
  }
  if (failures > 0) {
    std::fprintf(stderr, "%d 条规则没处理成功。\n", failures);
    return 1;
  }
  std::printf("%zu 条规则%s。\n", rules.size(),
              mode == "add" ? "已就位（同网段，专用/域网络）" : "已删除");
  if (mode == "add") {
    std::printf("注意：规则只对**同网段**放行。NVR 在别的子网/VLAN 里的话，"
                "要用 --firewall print 打出命令、把 remoteip=localsubnet 改掉。\n");
  }
  return 0;
}

// ---------------------------------------------------------------------------
// 进程形态：单实例锁 / 父进程看守 / 控制台中断
// ---------------------------------------------------------------------------
//
// 三种形态共用同一份采集代码，差别只有两件事：**有没有父进程可以盯**，以及
// **谁来重拉我**。
//
//   服务形态    [Session 0] --service 拉起并看门狗
//                     └─► [Session 1] --worker --parent-pid <pid>
//   控制台形态  [Session 1] --console
//   一次性形态  --record / --rtsp / --http（老行为：跑完就退）
//
// 服务为什么只做"拉起 + 看门狗"：**DXGI 桌面复制在 Session 0 里抓不到交互用户
// 的桌面**，所以采集必须在 Session 1 的进程里做。反过来，正因为只有 worker
// 进程碰采集，"仍然只有一个 HTTP 服务、配置的权威副本仍然只有一份"是结构性
// 成立的，不是靠约定维持的。

/// 单实例锁。
///
/// `Local\` 就够：两种形态的 worker 都落在交互用户那个会话里，而服务进程自己
/// 跑的是 `--service run`、根本不进这条路。换成 `Global\` 反而要自己写安全
/// 描述符才让非管理员用户打得开。
///
/// **拿不到锁不是错误。** 用户双击两下、或者服务重拉时旧进程还没退干净，都走
/// 到这里。对着"程序已经在运行"报错退出，是让人去任务管理器里找答案；正确的
/// 反应是把他领到已经在跑的那个网页上。
///
/// `wait_ms` 来自 `--handoff`：控制台形态自重启时，新进程要**等**旧进程把锁和
/// 端口一起放掉，而不是立刻宣告"已经在运行"然后退出 —— 那等于重启一次就把
/// 程序弄没了。
class InstanceLock {
 public:
  /// true = 锁是我们的，可以继续跑；false = 已经有实例在跑。
  bool try_acquire(int wait_ms) {
    // bInitialOwner 传 TRUE：对象**不存在**时我们当场持有它，不用再等一次；
    // 已存在时这个参数被忽略，我们也不持有 —— 那时才需要下面的等待。
    handle_ = ::CreateMutexW(nullptr, TRUE, L"Local\\Desk2ONVIF.worker");
    if (handle_ == nullptr) {
      // 建不出内核对象是系统级问题（句柄耗尽），不是"已经有实例"。
      // 说一声但**继续跑** —— 拒绝启动的后果（用户什么都用不了）比失去
      // 单实例保护严重得多。
      D2O_WARN("建单实例互斥体失败（错误 %lu），跳过单实例检查继续启动。",
               static_cast<unsigned long>(::GetLastError()));
      return true;
    }
    if (::GetLastError() != ERROR_ALREADY_EXISTS) return true;

    DWORD const w = ::WaitForSingleObject(handle_, static_cast<DWORD>(wait_ms));
    // WAIT_ABANDONED = 上一个持有者没释放就没了（被强杀、或者崩了）。锁归我们，
    // 但要说一声：正常退出会放锁，走到这里就意味着上次是被杀掉的。
    if (w == WAIT_ABANDONED) {
      D2O_WARN("上一个实例是被强杀的（互斥体处于遗弃状态），接管它。");
      return true;
    }
    return w == WAIT_OBJECT_0;
  }

  /// **不 ReleaseMutex**：进程退出时内核会替我们放掉，而这里只需要"我在跑"
  /// 这个事实。显式释放反而会在极端时序下（还有线程在用 session）放开一个
  /// 已经有新实例在等的锁。handle 也不关 —— 关掉会让对象消失。
  HANDLE handle() const { return handle_; }

 private:
  HANDLE handle_ = nullptr;
};

/// 盯住父进程：它一消失就请求停止。
///
/// **用父进程句柄而不是命名事件**（陷阱 69）：`Local\` 命名空间按会话隔离 ——
/// 服务在 Session 0、worker 在 Session 1，`Local\Desk2ONVIF.stop` 在两边根本
/// 不是一个对象（服务那侧置了信号，worker 这侧永远等不到）；而 `Global\` 要
/// 自己写安全描述符才让非管理员用户打得开。`OpenProcess(SYNCHRONIZE, ppid)`
/// 一行都不用碰 ACL，而且**顺带覆盖了"服务自己崩了"这条路** —— 服务一死句柄
/// 就信号，worker 自己收尾走人，不会留下一个没人管的孤儿进程占着端口。
///
/// 为什么要一条线程而不是在主线程上等：主线程要跑 `session.run()`。而
/// `request_stop()` 只是置一个原子位（见 capture_session.h），从任何线程调都安全。
///
/// 析构**必须先置退出事件再 join**：父进程还活着的时候那条等待是 INFINITE，
/// 直接 join 会挂死 —— 而这恰好是绝大多数情况（时间是到了、用户在网页上停了
/// 录制）。退出事件把它变成一条有出口的等待。
class ParentWatch {
 public:
  ParentWatch(d2o::CaptureSession& session, unsigned long pid) {
    if (pid == 0) return;   // 控制台形态：没有父进程可盯，这条守则是空转
    parent_ = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (parent_ == nullptr) {
      // 服务是 CreateProcessAsUserW 完**立刻**把 pid 给我们的，所以这几乎只
      // 可能是"父进程在这一瞬间已经没了"。**继续跑**而不是拒绝启动：一个没有
      // 看守的 worker 比一个起不来的 worker 好，而单实例锁挡住了重复。
      D2O_WARN("打不开父进程 %lu（错误 %lu），这一趟没有父进程看守。", pid,
               static_cast<unsigned long>(::GetLastError()));
      return;
    }
    quit_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    thread_ = std::thread([this, &session] {
      HANDLE waits[2] = {parent_, quit_};
      if (::WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) {
        D2O_INFO("父进程没了（服务停了或被关了），开始收尾 —— 这一段 MP4 会写完 trailer。");
        session.request_stop();
      }
    });
  }

  ~ParentWatch() {
    if (quit_ != nullptr) ::SetEvent(quit_);
    if (thread_.joinable()) thread_.join();
    if (parent_ != nullptr) ::CloseHandle(parent_);
    if (quit_ != nullptr) ::CloseHandle(quit_);
  }

  ParentWatch(ParentWatch const&) = delete;
  ParentWatch& operator=(ParentWatch const&) = delete;

 private:
  HANDLE parent_ = nullptr;
  HANDLE quit_ = nullptr;
  std::thread thread_;
};

/// 让控制台中断处理函数找到当前这条会话。
///
/// 用文件级全局是因为 `SetConsoleCtrlHandler` 的回调签名里**没有用户数据**
/// （`PHANDLER_ROUTINE` 只有那个 DWORD），没有别的办法把它递进去。
std::atomic<d2o::CaptureSession*> g_ctrl_session{nullptr};
std::atomic<bool> g_ctrl_seen{false};

/// Ctrl+C / 关窗口时**优雅收尾**，而不是被直接砍掉。
///
/// 没有它的话，控制台形态按一下 Ctrl+C 拿到的是一个没有 trailer 的 MP4：
/// 文件名在、大小不为零、看起来录了，但播放器打不开 —— 而这件事没有任何提示，
/// 用户会以为录像功能坏了。
///
/// 关窗口（CTRL_CLOSE_EVENT）那条路 Windows 只给几秒，可能来不及收完；这里
/// 尽力而为，来不及的话和以前一样。**再按一次就放行默认处理**（当场结束）——
/// 卡住的时候连按两下必须能出去，否则这个程序只能去任务管理器里杀。
BOOL WINAPI console_ctrl_handler(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT) {
    return FALSE;
  }
  d2o::CaptureSession* const s = g_ctrl_session.load();
  if (s == nullptr) return FALSE;
  if (g_ctrl_seen.exchange(true)) {
    std::fprintf(stderr, "\n再按一次就强制退出（这一段 MP4 不会有 trailer）。\n");
    return FALSE;
  }
  std::fprintf(stderr,
               "\n收到中断，正在收尾（编码器冲干净、MP4 写完 trailer）……\n"
               "再按一次就强制退出。\n");
  s->request_stop();
  return TRUE;
}

/// RAII：装上中断处理并登记会话，析构时摘掉。
///
/// **必须是 RAII**：下面有好几条 `return`，手写重置迟早漏一条，而漏掉的那条
/// 会让中断处理函数拿着一个已经析构的指针去调 `request_stop()`。
class CtrlHook {
 public:
  explicit CtrlHook(d2o::CaptureSession& s) {
    g_ctrl_session.store(&s);
    ::SetConsoleCtrlHandler(console_ctrl_handler, TRUE);
  }
  ~CtrlHook() {
    ::SetConsoleCtrlHandler(console_ctrl_handler, FALSE);
    g_ctrl_session.store(nullptr);
  }
  CtrlHook(CtrlHook const&) = delete;
  CtrlHook& operator=(CtrlHook const&) = delete;
};

/// 一次运行里最多重开几段。录制中途换分辨率是正常事件（插拔显示器、切用户、
/// 游戏全屏），每换一次就要把编码器/封装/SDP 整条链重建一遍，所以它是有代价
/// 的；而**反复**重建说明有东西在不停切模式（有些程序切进全屏会连切好几次）。
/// 到这个上限就停下来，而不是无限重开 —— 那会变成"一直在重开、什么都没录到"，
/// 比明确报错难查得多。
constexpr int kMaxSegments = 8;

/// 这个进程是**哪一种长驻形态**。只在"重启"那一步用到，而三种形态的去路
/// 完全不同：
///
///   * `Worker`  —— 服务拉起来的，退出码 7 = "立刻重拉我"（看门狗已经认这个码）
///   * `Console` / `Bare` —— 外面没人在守着，只能自己 `CreateProcessW` 拉一个自己
///
/// **为什么是参数而不是在 `cmd_run` 里回头去猜**：`cmd_run` 是被
/// `cmd_console` 调进来的，命令行里那个 `--console` / `--worker` 已经被
/// `cmd_console` 消费掉了；而 `--record` 这条路（`Bare`）**也可能**带着
/// `--console` 之外的任何组合。让调用方显式传进来，比在这里靠
/// `args.has("--worker")` 反推可靠 —— 反推漏掉一种组合的症状是"重启之后
/// 进程换了个形态"，比如一个 worker 自己拉了个自己、父进程却还在。
enum class RunForm { Bare, Console, Worker };

/// 自重启的命令行：把已有的 `--handoff <n>` 摘掉，末尾补一个新的。
///
/// **判据不是"这条命令行长什么样"，而是"不能重新拼一遍"。** 参数的每一段都用
/// **它在原始命令行里的原文**（连它自己的引号一起）原样搬过去，一个字符都不重新
/// 组装 —— 路径里的空格、中文、引号于是都不可能被拼错。按 `argv` 拼一条新命令行
/// 是这里最容易出错的做法：Windows 的引号规则不是"有空格就加引号"那么简单
/// （反斜杠与引号的交互有一条自己的规则），而错了的症状是"重启之后新进程的参数
/// 全乱"，且只在某些路径下发作。
///
/// 摘掉旧的 `--handoff` 是因为它只对**这次的接手**有意义：上一轮留下的
/// `--handoff 800` 如果照搬，新进程会在启动时白等 800 ms，而且两个值同时存在时
/// `int_option` 取到哪一个取决于扫描顺序 —— 那是"有时候慢、有时候不慢"这种
/// 最难查的缺陷。
///
/// `--handoff=800` 这种带等号的写法不在这里处理：`parse_args` 本来就不认它，
/// 当作未知参数报错退出，所以它到不了这儿。
std::wstring with_handoff(std::wstring const& src, int ms) {
  std::wstring out;
  std::size_t i = 0;
  std::size_t const n = src.size();
  while (i < n) {
    while (i < n && (src[i] == L' ' || src[i] == L'\t')) ++i;
    if (i >= n) break;
    std::size_t const begin = i;
    bool in_quote = false;
    while (i < n) {
      wchar_t const ch = src[i];
      if (ch == L'\\') {
        // 反斜杠转义下一个字符（Windows 的命令行规则），连它一起吞掉。
        i += (i + 1 < n) ? 2u : 1u;
        continue;
      }
      if (ch == L'"') {
        in_quote = !in_quote;
        ++i;
        continue;
      }
      if (!in_quote && (ch == L' ' || ch == L'\t')) break;
      ++i;
    }
    std::wstring const tok = src.substr(begin, i - begin);
    if (tok == L"--handoff") {
      // 连它后面那个值一起丢掉。
      while (i < n && (src[i] == L' ' || src[i] == L'\t')) ++i;
      std::size_t const vb = i;
      while (i < n && src[i] != L' ' && src[i] != L'\t') ++i;
      (void)vb;
      continue;
    }
    if (!out.empty()) out += L' ';
    out += tok;
  }
  out += L" --handoff ";
  out += std::to_wstring(ms);
  return out;
}

/// 我们的 stdout / stderr 里有没有**管道**？
///
/// 这一问决定新进程能不能继承它们 —— 理由见 `restart_process` 里那段长注释。
/// 只看 stdout 与 stderr：stdin 没人往管道里写，谁拿着它都不影响
/// "调用方能不能读到 EOF"。
bool std_stdio_is_pipe() {
  for (DWORD const which : {static_cast<DWORD>(STD_OUTPUT_HANDLE),
                            static_cast<DWORD>(STD_ERROR_HANDLE)}) {
    HANDLE const h = ::GetStdHandle(which);
    if (h == nullptr || h == INVALID_HANDLE_VALUE) continue;
    if (::GetFileType(h) == FILE_TYPE_PIPE) return true;
  }
  return false;
}

/// 按形态把"重启"落地。**返回值就是进程的退出码。**
///
/// 服务形态那条路一个字都不用改就能用：`service_host.cpp` 的看门狗已经是
/// "退出码 7 → 立刻重拉"，把这个码留在这里是**同一个定义的第二个使用者**
/// （`src/common/exit_code.h`），不是又写了一个字面量 7。
int restart_process(RunForm form, std::wstring const& raw_cmdline) {
  if (form == RunForm::Worker) {
    std::printf("\n重启：退出码 %d，由 Windows 服务立刻重新拉起。\n",
                static_cast<int>(d2o::kExitRestart));
    return d2o::kExitRestart;
  }

  // 控制台 / 裸形态：没人在外面守着，自己拉一个自己。
  //
  // **先拉新的、再退出。** 反过来的话两次进程之间有一段谁都没在听的空窗；
  // 而这样做，新进程会先卡在 `--handoff` 那 800 ms 上等我们把单实例锁和
  // 监听端口让出来，两件事自然错开。
  std::wstring const cmd = with_handoff(raw_cmdline, 800);
  if (cmd.empty()) {
    // 拿不到原始命令行（理论上不可能：Windows 一定给得出）。**不猜** ——
    // 从这里自己拼一条等于把路径里的空格和中文赌一把，赌输了新进程起不来
    // 或者参数全错。报清楚，让用户手工重启一次。
    std::fprintf(stderr,
                 "重启失败：拿不到本进程的命令行，无法自己拉起自己。"
                 "请手工重新启动一次。\n");
    return 1;
  }
  std::vector<wchar_t> mutable_cmd(cmd.begin(), cmd.end());
  mutable_cmd.push_back(L'\0');
  STARTUPINFOW si{};
  si.cb = sizeof si;
  // **把自己的标准句柄摘掉的那一半。**
  //
  // 实测（`out\pipe-probe.ps1`，2026-10-08）：`bInheritHandles=FALSE` **挡不住**
  // 孙进程拿到我们的 stdout 管道写端 —— 老进程退出后管道**没有关**，新进程还在
  // 往里写（捕获到的 stderr 尾部就是新进程自己的 `已编码 N 帧` 那几行），
  // 要等新进程被杀掉，写端才真的关。所以原先那句
  // "bInheritHandles=FALSE ⇒ 孙进程不继承管道"是**推出来的，不是量出来的**，
  // 而且量出来是反的。
  //
  // 后果不是"多一个句柄"：任何"等进程退出、再把输出读到 EOF"的调用方
  // （测试脚本、启动器、CI）**永远等不到 EOF**；它若干脆不读了，4 KB 的管道
  // 写满之后守护进程会**卡死在写日志上**。
  //
  // 做法：自己的 stdout/stderr 里**有管道**时，显式 `STARTF_USESTDHANDLES`
  // 且三个句柄全给 NULL，让新进程一个标准句柄都不继承。其余两种情况**一个字
  // 都不动**，因为它们的诉求相反且都成立：
  //   - **终端**（控制台句柄，`GetFileType` 是 `FILE_TYPE_CHAR`）：控制台句柄是
  //     控制台子系统给孩子的，本来就不靠继承；不动它，新进程的日志照样看得见。
  //   - **重定向到文件**（`FILE_TYPE_DISK`）：没人在等文件关掉，而且新进程继续
  //     写进同一个文件更好。
  //
  // 代价要说清（管道这一路）：新进程**没有** stdout/stderr，它的日志只进日志
  // 文件 —— console 形态有 `desk2onvif.log`，**裸形态本来就没有日志文件**。
  // 这是"让调用方能退出"必须付的价。
  bool const detach_stdio = std_stdio_is_pipe();
  if (detach_stdio) {
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;
    si.hStdOutput = nullptr;
    si.hStdError = nullptr;
  }
  PROCESS_INFORMATION pi{};
  // CREATE_UNICODE_ENVIRONMENT 配 lpEnvironment=nullptr：继承父进程的环境块
  // （不传这个标志会把我们的宽字符环境块当 ANSI 解，中文环境变量会乱）。
  // 工作目录传 nullptr = 继承本进程的 cwd —— 这一条**必须**继承：`--config`
  // 可能是相对路径，新进程换个目录跑就会去读另一个配置文件。
  if (!::CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) {
    DWORD const e = ::GetLastError();
    std::fprintf(stderr,
                 "重启失败：拉不起新进程（GetLastError=%lu）。请手工重新启动一次。\n",
                 static_cast<unsigned long>(e));
    return 1;
  }
  ::CloseHandle(pi.hThread);
  ::CloseHandle(pi.hProcess);
  std::printf("重启：已拉起新进程（PID %lu），本进程退出。\n",
              static_cast<unsigned long>(pi.dwProcessId));
  return d2o::kExitOk;
}

/// 恢复出厂：把磁盘和内存都退回"全新安装"的样子。顺序是固定的，每一步都有理由。
///
///   1. **先备份再删** —— 恢复出厂也是有人会后悔的，`.bak` 是唯一的后悔药。
///      备份**失败就到此为止**：宁可不恢复，也不能出现"配置没了、备份也没有"。
///   2. 删 `config.json` —— 删完之后下一次 `load_config` 看到的是"文件不存在"，
///      和全新安装**完全同一条路**（`cmd_console` 会因此把 Web 与 RTSP 都打开）。
///      换成"写一份默认值进去"的话，走的是另一条路，而两条路上任何一处差异
///      （比如"文件不存在时补 --http"那段）都会让恢复出厂的结果和全新安装不同。
///   3. 日志：**关 → 删 → 重新打开**。关是必须的 —— Windows 的 `fopen` 不带
///      `FILE_SHARE_DELETE`，句柄开着时删除直接失败，症状是"恢复出厂之后旧日志
///      还在"。删的是 `is_our_log_file(name)` 认得的那些，**只删那一个目录里的
///      普通文件，不递归** —— 那个目录里可能有别人的东西，"清空日志"清的是日志，
///      不是那个目录。
///   4. 设备状态复位（时间偏移归零、名称回到默认）。`reset_time()` 到今天为止
///      还没有调用者，这里是它的第一个 —— 所以验收里必须有一条**真的量它**
///      （先设一个偏移、恢复出厂、断言归零），而不是"看着像对"。
///
/// 不做的事，写在这里免得下次有人以为漏了：不碰 `%APPDATA%` 里用户自己那份
/// 配置，不碰防火墙规则，不碰 `Temp\`，**也不碰命令行参数** —— 重启用的是同一条
/// 命令行（`packaging\start.ps1` 传的是 `--http-port 8080 --rtsp-port 554`，
/// 正好等于默认值，所以看起来确实回到了 8080；但那是"命令行与默认值恰好一致"，
/// 不是"恢复出厂改了端口"）。
void factory_reset_files(std::string const& config_path, d2o::DeviceState& device_state,
                         d2o::AuditLog* audit, d2o::AuditWho const& who) {
  std::error_code ec;

  // --- 1 + 2. 配置文件 -----------------------------------------------------
  if (std::filesystem::exists(config_path, ec)) {
    std::string const bak = config_path + ".bak";
    std::error_code bec;
    std::filesystem::copy_file(config_path, bak,
                               std::filesystem::copy_options::overwrite_existing, bec);
    if (bec) {
      // 备份失败 -> **什么都不做**，然后照样重启（客户端已经被告知"正在恢复
      // 出厂"，而进程的会话/连接也已经按重启收尾了；停在这里会把服务形态
      // 弄成一个不会自己回来的死进程）。这里要喊得响，因为它意味着"点了恢复
      // 出厂，而机器上一切照旧"。
      D2O_ERROR("恢复出厂：备份 %s 失败（%s），**本次恢复出厂没有做任何事**。",
                bak.c_str(), bec.message().c_str());
      return;
    }
    std::error_code rec;
    std::filesystem::remove(config_path, rec);
    if (rec) {
      D2O_ERROR("恢复出厂：备份好了，但删不掉 %s（%s）。配置文件仍在，"
                "删掉 %s 即可手工完成。",
                config_path.c_str(), rec.message().c_str(), config_path.c_str());
      return;
    }
    std::printf("恢复出厂：配置已备份到 %s，%s 已删除。\n", bak.c_str(),
                config_path.c_str());
  } else {
    std::printf("恢复出厂：本来就没有配置文件（%s），跳过备份与删除。\n",
                config_path.c_str());
  }

  // --- 3. 日志 -------------------------------------------------------------
  //
  // 两份日志，两条清除路径，**都是"关 → 删 → 开回来"这个顺序**，理由同一条：
  // Windows 的 `fopen` 不带 `FILE_SHARE_DELETE`，句柄开着就删不掉，症状是
  // "恢复出厂之后旧日志还在"。
  std::string const log_path = d2o::Log::file_path();
  d2o::Log::close_file();

  int audit_files = -1;   // -1 = 没接审计（调用方给了空指针）
  if (audit != nullptr) {
    // **走 `AuditLog::clear()`，不在这里自己删。** 两条理由：
    //
    //   (a) "哪些文件算我们的日志"只有一个判据（`is_our_log_file`）。在这里
    //       写第二遍，两处迟早漂 —— 而漂的症状恰好是"恢复出厂之后旧日志还在"，
    //       也就是审计最不该出问题的地方。
    //   (b) 今天的那个文件**正被 `AuditLog` 开着**。上面那句 `Log::close_file()`
    //       关的是调试日志，与它无关 ⇒ 直接 `filesystem::remove` 必然失败
    //       （Permission denied），而且**静默失败**：原来那段代码把返回值丢掉，
    //       于是"清空失败"和"清空成功"打出的是同一句话。
    //
    // `clear()` 自己管这三步（关 → 删 → 开回来），目录本身不删。
    audit_files = audit->clear();
  }

  if (!log_path.empty()) {
    std::filesystem::path const lp(log_path);
    std::filesystem::remove(lp, ec);
    std::filesystem::remove(lp.string() + ".old", ec);
    // 开回来 —— 不然后面到进程退出之间的日志只进终端，而服务形态根本没有终端。
    d2o::Log::set_file(log_path);
    if (audit_files >= 0) {
      std::printf("恢复出厂：日志已清空（%s；操作日志 %d 个文件）。\n",
                  log_path.c_str(), audit_files);
    } else {
      std::printf("恢复出厂：日志已清空（%s）。\n", log_path.c_str());
    }
  }

  // --- 4. 设备状态 ---------------------------------------------------------
  device_state.reset_time();
  // 名称回到**出厂默认值**。这里从一份默认构造的配置里取，而不是写死一个字面量：
  // 默认名改在 `config.h` 里，这里自动跟上，不会漂。
  d2o::AppConfig const fresh;
  device_state.set_channel_name(fresh.onvif.device_name);
  std::printf("恢复出厂：设备时间偏移已归零，名称回到「%s」。\n",
              fresh.onvif.device_name.c_str());

  // --- 5. 在日志自己的第一行里留下"日志被清空了" ---------------------------
  //
  // 恢复出厂是本进程里**唯一**一个会把日志清掉的操作，所以它的痕迹必须由它
  // 自己留：不留的话，新日志的第一行会是"之后某个人做的某件事"，而"中间那
  // 一段去哪了"没有任何人回答得了 —— 那正是审计日志存在要回答的问题。
  //
  // **一行装下两件事**（谁恢复的 + 日志被清了），不是两行：这个动作把日志
  // 清空是它的一部分，写成两行的话第一行会被自己删掉，于是看起来像"有人
  // 恢复出厂、但没清日志"。
  //
  // 也正因为是这样，请求者必须**由 web 层一路带到这里**（见 `wo.factory_reset`
  // 的签名）—— 它是唯一一个"写日志的人"和"清日志的人"不是同一层的动作。
  if (audit != nullptr) {
    audit->note(who, "factory-reset",
                audit_files >= 0
                    ? d2o::fmt("配置已备份为 .bak 并删除；日志已清空（%d 个操作"
                               "日志文件）",
                               audit_files)
                    : std::string("配置已备份为 .bak 并删除；日志已清空"));
  }
}

// ---------------------------------------------------------------------------
// 多通道（第五轮 Phase AD）
//
// 通道 1 不在这套东西里 —— 它跑在 `cmd_run` 自己那条老路径上（`capture.*` /
// `rtsp.*`），一条线程、一个 `CaptureSession`、和从前逐字节相同的行为。
// 下面这些只服务通道 2 起。
// ---------------------------------------------------------------------------

/// `main.mp4` → `main.1.mp4`（第二段起）；没有扩展名就往后接。
///
/// 这段规则原来内联在分段循环里，抽出来是因为**每一路通道都要按同一条规则
/// 往后数** —— 见 `recorded_bytes()`。两处各写一遍的话，"通道 2 的第二段"
/// 会被算成一个不存在的文件名，而结果是磁盘占用少报一块。
std::string segment_filename(std::string const& base, int segment) {
  if (segment <= 0 || base.empty()) return base;
  std::size_t const dot = base.find_last_of('.');
  std::size_t const slash = base.find_last_of("/\\");
  bool const has_ext = dot != std::string::npos && dot != 0 &&
                       (slash == std::string::npos || dot > slash + 1);
  return has_ext
             ? base.substr(0, dot) + "." + std::to_string(segment) + base.substr(dot)
             : base + "." + std::to_string(segment);
}

/// 通道 N 的录制文件名：`out\mc.mp4` → `out\mc-ch2.mp4`。
///
/// **空进空出**，这一条是必须的：`--record` 没给的时候（只推流）每一路都得
/// 保持"不录"，而 `"-ch2"` 这种拼接会凭空造出一个文件来 —— 用户没让它录，
/// 磁盘上却多了个 `-ch2.mp4`。
std::string channel_record_filename(std::string const& base, unsigned index) {
  if (base.empty()) return base;
  std::string const tag = "-ch" + std::to_string(index);
  std::size_t const dot = base.find_last_of('.');
  std::size_t const slash = base.find_last_of("/\\");
  bool const has_ext = dot != std::string::npos && dot != 0 &&
                       (slash == std::string::npos || dot > slash + 1);
  return has_ext ? base.substr(0, dot) + tag + base.substr(dot) : base + tag;
}

/// 这一路**总共**占了多少磁盘：`out\mc.mp4` 加上它分出来的 `out\mc.1.mp4`…
///
/// 只报当前那个文件的 `file_size` 会漏掉前面几段，而"这一路占了多少磁盘"
/// 问的是总数 —— 中途中途换了分辨率就会分段，而一段满屏的 1080p 录像一个
/// 小时好几个 G，漏掉的那几段正是最占地方的那几段。
///
/// 上界取 `kMaxSegments`：分段次数在那边已经被卡死了，所以这个循环有界，
/// 不是"扫目录"。用 `error_code` 而不是异常 —— 文件还不存在是最常见的情况
/// （第一秒就有人来问状态），而抛异常在采集线程上等于把统计整个打断。
std::uint64_t recorded_bytes(std::string const& base) {
  if (base.empty()) return 0;
  std::uint64_t total = 0;
  auto const add = [&total](std::string const& p) {
    std::error_code ec;
    auto const sz = std::filesystem::file_size(p, ec);
    if (!ec) total += static_cast<std::uint64_t>(sz);
  };
  add(base);
  for (int i = 1; i <= kMaxSegments; ++i) add(segment_filename(base, i));
  return total;
}

/// 通道 2 起的一路。
///
/// ## 为什么对象在**堆上**（`unique_ptr` 而不是 `vector<ExtraChannel>`）
///
/// 每路通道自己那个 `WebServer` 的回调、以及这条采集线程，捕获的都是**这一路
/// 的信息**。`vector` 扩容会让元素搬家，于是那些引用指向被搬空的旧地址 ——
/// 这是陷阱 49（use-after-free，症状是"崩在别处"，任何 backtrace 都指不回来）。
/// `unique_ptr` 让元素的地址与 vector 的搬迁无关，这才是那一整族问题的**结构性**
/// 解法，不是风格问题。声明还要 `reserve` 满并放在任何 `web.start()` 之前。
struct ExtraChannel {
  /// 这一路的静态计划：号（2 起）、名字、来源、档位、RTSP 路径、`enabled`。
  ///
  /// 整份存下来而不是拆成几个字段，是因为下面那两处（ONVIF 身份、ONVIF
  /// profile 的 `rtsp_path`）要的正是计划里的原值 —— 拆开之后就得再拼一次，
  /// 而拼出来的和计划里的迟早会不一样（端口和路径对不上 NVR 就拉不到流）。
  d2o::ChannelPlan plan;
  /// 这一路的采集选项。**必须留着**：`run()` 收的是 `SessionOptions&`（非
  /// const），它会就地补 `rtsp.streams[i].url` 这些字段。临时对象的话，那条
  /// 线程手上会是一个悬垂引用。
  d2o::SessionOptions opt;
  /// 这一路的采集会话。**`shared_ptr` 而不是 `unique_ptr`**：`PreviewHub` 里那
  /// 张"按通道取快照"的表要握一份，而那张表活在 web 循环线程的手上 —— 用裸指针
  /// 的话，每换一次分辨率（整条链重建）就会留一个悬垂的入口，而症状是"点快照
  /// 偶发崩溃"，和"换了分辨率"看不出关系（陷阱 49）。`shared_ptr` 让"这一路被
  /// 重建了"和"有人正拿着它取帧"能同时成立。
  std::shared_ptr<d2o::CaptureSession> session;
  /// 这一路自己的 ONVIF 端点（`web.port + plan.index`）。
  ///
  /// **只开 SOAP 端点**：不开静态文件（没人会去 8081 上看网页）、不开
  /// WS-Discovery（UDP 3702 只有一个 socket，多开会变成 N 个进程抢同一个
  /// 端口，而失败的样子是"设备在 NVR 里时有时无"）。
  std::unique_ptr<d2o::WebServer> web;
  std::thread thread;
  /// 这一路录到哪个文件（基准名，分段是它加序号）。空 = 只推流不录。
  std::string record_path;

  // --- 采集线程写、状态那一份读 -----------------------------------------
  //
  // 逐项原子而不是整份加锁：状态是每秒刷新的近似值，拼出"fps 是这一拍、
  // 字节是上一拍"没有实际后果；而整份加锁意味着采集线程要等一把 web 循环
  // 线程持有的锁 —— 那是拿采集的准时性去换一个没人看得出来的精度。
  std::atomic<double> fps{0.0};
  std::atomic<double> bitrate_kbps{0.0};

  /// 这一路此刻的状态，一句人话（"显示器 #2 抓不到"比"失败"有用得多）。
  ///
  /// **主线程写、采集线程读**，所以有锁 —— 它是 `std::string`，跨线程裸读写
  /// 会读到一条正在被改的字符串，那是真会崩的。理由和上面两个数字不同：
  /// 它只在开场和收尾各变一次，锁的争用可以忽略。
  mutable std::mutex st_mu;
  std::string state = "未启动";

  void set_state(std::string s) {
    std::lock_guard<std::mutex> lk(st_mu);
    state = std::move(s);
  }
  std::string get_state() const {
    std::lock_guard<std::mutex> lk(st_mu);
    return state;
  }
};

/// 通道 1 的静态那几行 + 攒好的 `channels` JSON，共用一把锁。
///
/// 通道 1 没有 `ExtraChannel` 那样的对象（它跑在 `cmd_run` 自己那条路径上），
/// 所以它那几行信息得有个地方放。**只有主线程写**：建完这一趟的通道之后写
/// 一次、`run()` 返回之后再写一次状态；读它的是通道 1 每秒一次的统计钩子
/// （跑在采集线程上）。
struct ChannelBoard {
  struct Ch1Info {
    std::string name = "通道 1";
    std::string source = "display";
    std::string state = "未启动";
    /// 通道 1 录到哪个文件（基准名）。空 = 没在录。
    std::string record_file;
    /// 通道 1 主码流的 RTSP 路径，**带前导斜杠**（`/live`）。空 = 没开 RTSP。
    ///
    /// 带斜杠是刻意的：通道 2 起那些自动合成的路径（`/ch2`）都带，两处在界面上
    /// 拼出来的是同一句话。一边带一边不带的话，`/ws/stats` 里就会有两种写法。
    std::string rtsp_path;
  };

  mutable std::mutex mu;
  Ch1Info ch1;
  /// `/api/status` 里那个 `channels` 数组，已经序列化好。
  std::string json = "[]";

  void set_ch1(Ch1Info info) {
    std::lock_guard<std::mutex> lk(mu);
    ch1 = std::move(info);
  }
  void set_state(std::string state) {
    std::lock_guard<std::mutex> lk(mu);
    ch1.state = std::move(state);
  }
  void set_json(std::string j) {
    std::lock_guard<std::mutex> lk(mu);
    json = std::move(j);
  }
  std::string get_json() const {
    std::lock_guard<std::mutex> lk(mu);
    return json;
  }
  Ch1Info get_ch1() const {
    std::lock_guard<std::mutex> lk(mu);
    return ch1;
  }
};

/// 网页预览「现在看第几路」—— 全机一份的状态，外加切换时要用的那几份缓存。
///
/// ## 为什么"看哪一路"不能是每个连接各一份
///
/// 控制台只有一个（绑 `web.port` 的那个），画面有 N 路。而 `/api/snapshot.jpg`
/// 是一条**无状态的 GET**：界面上按的是"通道 2 的快门"、存下来的却是通道 1 ——
/// 只可能是这个状态没被两边共用。所以它落在进程里，谁问都读同一份。
///
/// ## 缓存是**必需**的，不是优化
///
/// 一次采集只发**一次**初始化分段（ftyp+moov），而 `WebServer` 只留它最近看到
/// 的那一份（`publish_preview` 里的 `preview_init`）。切到通道 2 时如果只把
/// "现在看 2"这个位翻过去，浏览器接下来收到的是通道 2 的分片配上通道 1 的 moov
/// —— 它**不报错**，只是一直黑着（track id、时间基、SPS 全对不上）。所以每路的
/// 初始化分段和 0 点各留一份，切换时重放。
///
/// 缓存**无条件填**，不看"现在是不是它"：通道 2 的会话多半在用户第一次切过去
/// 之前就跑起来了，那一刻它没在转发，但它的初始化分段必须留下来。
struct PreviewHub {
  /// 现在看第几路。**1 是默认**，也正是"没有这个功能时"的行为。
  std::atomic<int> active{1};

  mutable std::mutex mu;
  /// 每一路的初始化分段（ftyp+moov）。
  std::map<unsigned, std::shared_ptr<std::vector<std::uint8_t> const>> init;
  /// 每一路的预览 0 点（墙钟毫秒）—— 那把"真实延迟"尺子的原点。
  std::map<unsigned, std::int64_t> t0;
  /// 每一路的会话，**只为让 `/api/snapshot.jpg` 按当前那一路取帧**。
  ///
  /// 存 `shared_ptr` 而不是裸指针或引用：这些会话归 `ExtraChannel` 所有，而每换
  /// 一次分辨率它们就被**整体重建**（`teardown_channels` 先 join 再析构）。快照
  /// 回调跑在 web 循环线程上，随时可能落在那两件事中间 —— 在锁里拷一份
  /// `shared_ptr` 就把它撑住了：要么拿到重建前的、要么拿到重建后的，不会拿到一个
  /// 已经析构的（陷阱 49：症状是"崩在别处"，任何 backtrace 都指不回这里）。
  std::map<unsigned, std::shared_ptr<d2o::CaptureSession>> sessions;
  /// 现在**真的有**哪几路（1 恒有）。切换请求按它校验。
  std::vector<int> available{1};

  void note_init(unsigned idx, bool is_init,
                 std::shared_ptr<std::vector<std::uint8_t> const> const& bytes) {
    if (!is_init || !bytes || bytes->empty()) return;
    std::lock_guard<std::mutex> lk(mu);
    init[idx] = bytes;
  }
  void note_t0(unsigned idx, std::int64_t ms) {
    if (ms <= 0) return;
    std::lock_guard<std::mutex> lk(mu);
    t0[idx] = ms;
  }
  void set_available(std::vector<int> channels) {
    std::lock_guard<std::mutex> lk(mu);
    available = std::move(channels);
  }
  bool has(int idx) const {
    std::lock_guard<std::mutex> lk(mu);
    for (int const n : available) {
      if (n == idx) return true;
    }
    return false;
  }
  std::shared_ptr<std::vector<std::uint8_t> const> init_of(int idx) const {
    std::lock_guard<std::mutex> lk(mu);
    auto it = init.find(static_cast<unsigned>(idx));
    return it == init.end() ? nullptr : it->second;
  }
  std::int64_t t0_of(int idx) const {
    std::lock_guard<std::mutex> lk(mu);
    auto it = t0.find(static_cast<unsigned>(idx));
    return it == t0.end() ? 0 : it->second;
  }
  void put_session(unsigned idx, std::shared_ptr<d2o::CaptureSession> s) {
    std::lock_guard<std::mutex> lk(mu);
    sessions[idx] = std::move(s);
  }
  void drop_sessions() {
    std::lock_guard<std::mutex> lk(mu);
    sessions.clear();
  }
  std::shared_ptr<d2o::CaptureSession> session_of(int idx) const {
    std::lock_guard<std::mutex> lk(mu);
    auto it = sessions.find(static_cast<unsigned>(idx));
    return it == sessions.end() ? nullptr : it->second;
  }
};

/// 一秒出了多少字节 ≈ 多少 kbps。**空窗口给 0 而不是除零**：第一拍
/// （`elapsed_s == 0`）本来就是"还没有数"，而 0 在界面上是一句真话。
int bitrate_kbps_of(std::uint64_t bytes, double elapsed_s) {
  if (elapsed_s <= 0.0) return 0;
  double const kbps = static_cast<double>(bytes) * 8.0 / elapsed_s / 1000.0;
  return static_cast<int>(kbps + 0.5);
}

/// 拼出 `/api/status` 的 `channels` 数组。
///
/// **通道 1 的数字不从这里取**：它由调用方（通道 1 的统计钩子）把刚拿到的
/// `SessionStats` 递进来 —— 那一路的统计归会话所有，`ExtraChannel` 里没有它。
///
/// 这个函数跑在**采集线程**上（通道 1 的统计钩子），而它读的 `extras` 只有
/// 主线程会改。能这么做的理由是**那一刻主线程正阻塞在 `session.run()` 里**
/// —— 和 `set_stats_hook` 那个 lambda 一直以来的处境一模一样（见 `if (http)`
/// 那段的注释）。主线程改 `extras` 只发生在两次 `run()` 之间。
///
/// `base_port` 是 `web.port`（通道 1 的 ONVIF 端口）。每通道一个端口的规矩是
/// "通道 N 在 `base_port + N`"，而**这条规矩只在这里出现一次** —— 前端算一遍、
/// 后端算一遍的话，改了规则就会变成"界面上写的端口连不上"。`rtsp_path` 同理：
/// 它是可以按通道手配的（`channels.items[].rtsp_path`），前端拼不出来。
std::string build_channels_json(
    ChannelBoard const& board,
    std::vector<std::unique_ptr<ExtraChannel>> const& extras,
    d2o::SessionStats const& ch1_stats, int base_port) {
  nlohmann::json arr = nlohmann::json::array();

  ChannelBoard::Ch1Info const info = board.get_ch1();
  {
    nlohmann::json o;
    o["index"] = 1;
    o["name"] = info.name;
    o["source"] = info.source;
    o["state"] = info.state;
    o["fps"] = ch1_stats.effective_fps;
    o["bitrate_kbps"] = bitrate_kbps_of(ch1_stats.bytes, ch1_stats.elapsed_s);
    o["record_file"] = info.record_file;
    o["record_bytes"] = recorded_bytes(info.record_file);
    // 通道 1 在 `web.port` **本身**（不是 +1）—— "通道 N 在 port+N"只在 N ≥ 2
    // 时成立。写成 `base_port + 1` 是个很容易犯、而且看起来很像对的错。
    o["web_port"] = base_port;
    o["rtsp_path"] = info.rtsp_path;
    arr.push_back(std::move(o));
  }

  for (auto const& ch : extras) {
    nlohmann::json o;
    o["index"] = ch->plan.index;
    o["name"] = ch->plan.name;
    o["source"] = ch->plan.source;
    o["state"] = ch->get_state();
    o["fps"] = ch->fps.load(std::memory_order_relaxed);
    o["bitrate_kbps"] = ch->bitrate_kbps.load(std::memory_order_relaxed);
    o["record_file"] = ch->record_path;
    o["record_bytes"] = recorded_bytes(ch->record_path);
    o["web_port"] = base_port + static_cast<int>(ch->plan.index);
    // 路径取**计划里的**，不取 `ch->opt.rtsp.streams[0].path`：`opt` 会被采集
    // 线程就地补写 `url` 这些字段，而这里跑在通道 1 的采集线程上 —— 跨线程读
    // 另一个会话那个 `std::string` 是真会读到半个字符串的。计划是只读的。
    o["rtsp_path"] = ch->plan.rtsp_path;
    arr.push_back(std::move(o));
  }

  return arr.dump();
}

/// 拼出 `/api/sources` 的正文：本机**能当通道来源的东西**。
///
/// 配置页的「通道」那一段靠它把"还能加什么"摆出来 —— 用户看不到 `--list-displays`
/// 和 `--list-cameras` 的输出，而那两个命令是**命令行**上的东西，网页上够不着。
///
/// 内容是**启动时算好的快照**（调用方那份 `displays` 就是规划时用的同一份），
/// 理由写在 `cmd_run` 里那个注释和 `WebOptions::sources_json` 上：探"能不能抓"
/// 会真的建一个 `IDXGIOutputDuplication`，而一块屏同时只允许一个。
///
/// `cameras` 单独传而不是在这里现枚举，是为了让"枚举失败"这件事**就在调用方
/// 一眼可见**（它得处理那个 err），而不是藏在一个拼 JSON 的函数里。
std::string build_sources_json(
    std::vector<d2o::ChannelDisplayInfo> const& displays,
    std::vector<d2o::CameraInfo> const& cameras,
    std::string const& camera_err) {
  nlohmann::json out;
  out["displays"] = nlohmann::json::array();
  for (d2o::ChannelDisplayInfo const& d : displays) {
    nlohmann::json o;
    o["index"] = d.index;
    // `description` 已经带了分辨率和位置（`describe()` 拼的），照发 —— 前端
    // 不再自己拼一遍，否则以后 `describe()` 改了，两个地方会说出两个样子。
    o["name"] = d.description;
    o["width"] = d.width;
    o["height"] = d.height;
    o["x"] = d.x;
    o["y"] = d.y;
    // **`capturable:false` 的也要发出去**，不能在这里筛掉：用户在这台机器上
    // 看不到那块屏（向日葵那类间接显示驱动），而 `--list-displays` 里列着它
    // —— 清单里凭空少一项，他只会以为网页坏了。前端拿到 `false` 就把那一项
    // 显示成"抓不了"并且加不上（自动发现也不会建它，见 channel_plan.h 规则 3）。
    o["capturable"] = d.capturable;
    out["displays"].push_back(std::move(o));
  }

  out["cameras"] = nlohmann::json::array();
  for (std::size_t i = 0; i < cameras.size(); ++i) {
    nlohmann::json o;
    o["index"] = static_cast<unsigned>(i);   // 就是 --list-cameras 那个 #N
    o["name"] = cameras[i].name;
    // 第一台是默认的那一台（`--camera` 不带值时用的就是它）。前端据此把它
    // 排在前面并标一下，用户才知道"我不选的话会开哪个"。
    o["is_default"] = cameras[i].is_default;
    out["cameras"].push_back(std::move(o));
  }
  // 枚举失败时才带上这句话。**空串不发**：前端判 `sources.camera_error` 非空
  // 就显示一句提示，多一个恒为空的字段只是让前端多一个要判断的东西。
  if (!camera_err.empty()) out["camera_error"] = camera_err;
  return out.dump();
}

/// 通道 N 的 ONVIF 身份：主身份那份原样拿过来，只换掉**能区分通道的那几样**。
///
/// `uuid` 不在这里（`OnvifIdentity` 里没有这个字段）—— 它归 `WebOptions::device_uuid`，
/// 由 `make_uuid_urn(seed + "|chN")` 就地推出来，和序列号同源。两者都必须
/// **跨重启稳定**：NVR 按 uuid 记设备，一变就是"多出一台新摄像机、原来那台离线"。
///
/// 名字和型号里带上通道号是刻意的，不是装饰：NVR 的设备列表里那两台长得一模一样
/// 的时候，用户就是靠这一行字把列表里的条目和屏幕上的画面对上号的。
d2o::OnvifIdentity channel_identity(d2o::OnvifConfig const& base,
                                    d2o::ChannelPlan const& p,
                                    std::string const& seed) {
  d2o::OnvifIdentity id = d2o::identity_from_config(base);
  std::string const salt = d2o::fmt("%s|ch%u", seed.c_str(), p.index);
  id.device_name = p.name.empty() ? d2o::fmt("通道 %u", p.index) : p.name;
  id.model = d2o::fmt("%s (通道 %u)", base.model.c_str(), p.index);
  id.serial_number = d2o::make_serial(salt);
  return id;
}

/// 把一个名字百分号编码成 scope 值里能放的样子。
///
/// **不是为了好看，是为了不坏。** scope 的值是一个 URI 的一段，而通道名里带
/// `#` —— 自动发现建出来的就叫「显示器 #2」—— `#` 在 URI 里是 fragment 的分界符，
/// 原样写进去，NVR 解析出来的名字只剩 `显示器 `。中文则要按 UTF-8 逐字节编码
/// （百分号编码本来就是干这个的）。
std::string scope_escape(std::string const& s) {
  static char const* const kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(s.size());
  for (unsigned char const c : s) {
    bool const safe = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                      (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                      c == '.' || c == '~';
    if (safe) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

/// 一路通道（2 起）跑完它的全部段落。**在它自己那条线程上跑。**
///
/// 形状和 `cmd_run` 里那个主循环一样（换分辨率 → 整条链重建、文件名加序号），
/// 少掉的是配置重载那一半：通道 2 起的选项来自**启动时**算好的计划，而计划改了
/// 要重启进程（`channels.*` 在 `pending` 里）。这不是省事 —— 端口、设备 UUID、
/// WS-Discovery 的宣告表全绑在计划上，允许"重载就换端口"会让那三样对不上。
///
/// `total_seconds` 是**整段运行**的预算，不是每一路各一份：`--seconds 20` 在每一条
/// 通道上都是 20 秒，和主循环里"时间预算是整段运行的"是同一条规矩。
void run_channel_segments(ExtraChannel& ch, int total_seconds,
                          std::chrono::steady_clock::time_point const started) {
  std::string err;
  d2o::SessionOptions seg = ch.opt;
  for (int round = 0;; ++round) {
    if (round > kMaxSegments) {
      ch.set_state(d2o::fmt("反复重开超过 %d 次，停下", kMaxSegments));
      D2O_ERROR("通道 %u 反复重开超过 %d 次，停下 —— 再开下去就成了反复重建而"
                "录不到东西。", ch.plan.index, kMaxSegments);
      return;
    }
    if (round > 0) {
      seg.output_path = ch.record_path.empty()
                            ? std::string()
                            : segment_filename(ch.record_path, round);
      if (total_seconds > 0) {
        double const used = std::chrono::duration<double>(
                                std::chrono::steady_clock::now() - started)
                                .count();
        int const left = total_seconds - static_cast<int>(used);
        if (left < 1) {
          // 剩下的时间不够再录一段了。这一段已经完整收尾，就当正常结束。
          ch.set_state("已停止");
          return;
        }
        seg.seconds = left;
      }
      D2O_INFO("通道 %u：画面尺寸变了，另起一段%s。", ch.plan.index,
               seg.output_path.empty() ? "" : ("（" + seg.output_path + "）").c_str());
    }

    ch.set_state("运行中");
    d2o::CaptureSession::RunResult const r = ch.session->run(seg, err);
    if (r == d2o::CaptureSession::RunResult::Restart) continue;
    if (r == d2o::CaptureSession::RunResult::Failed) {
      ch.set_state("失败：" + err);
      D2O_ERROR("通道 %u 停了：%s", ch.plan.index, err.c_str());
    } else {
      ch.set_state("已停止");
    }
    return;
  }
}

int cmd_run(ParsedArgs const& args, RunForm form, std::wstring const& raw_cmdline) {
  d2o::SessionOptions opt;

  // 配置文件先读，读到的值当**默认值**用，命令行参数照常覆盖它。
  // 这个方向不能反：反过来（配置覆盖命令行）会让 `--fps 60` 在一个写了
  // fps=30 的配置文件面前静默失效，而用户手里唯一的线索是"参数没用"。
  std::string const config_path = args.value("--config", d2o::default_config_path());
  d2o::AppConfig cfg;
  {
    std::string cerr_msg;
    bool existed = false;
    if (!d2o::load_config(cfg, config_path, cerr_msg, &existed)) {
      // 读不了就**别硬撑**。带着一份默认配置继续跑，等于用默认值去覆盖用户
      // 的端口/口令，而用户以为配置文件是生效的 —— 那比直接报错难查得多。
      std::fprintf(stderr, "配置文件读不了：%s\n  %s\n", config_path.c_str(),
                   cerr_msg.c_str());
      return 2;
    }
    if (existed) std::printf("配置：%s\n", config_path.c_str());
    // 越界的值在这里就报出来，而不是等某个功能用到它才出怪事。
    std::vector<std::string> notes;
    if (d2o::validate_config(cfg, notes)) {
      for (std::string const& n : notes) {
        std::fprintf(stderr, "配置值越界，已夹到合法区间：%s\n", n.c_str());
      }
    }
  }

  // 延时档位（Phase W）。**必须夹在"读完配置"和"解析那几个 int 选项"之间**：
  // `--gop` / `--threads` 的默认值取自 `cfg`，而这个档位干的正是改 `cfg` 里
  // 那两项 —— 顺序反了的话 `--latency realtime` 会只改到标签、改不到值，
  // 而命令行看上去完全正常（陷阱 27 的同一族：**选项被无声忽略**）。
  //
  // 这里直接再走一遍 `validate_config`，而不是自己调用一次展开逻辑：
  // 展开只有那一处（在 validate_config 里），复制一份出来迟早会漂。
  // 第二次调用是幂等的 —— 上面那次已经把该夹的都夹了，这次只可能产出
  // 延时档位自己那两条 snap。
  if (args.has("--latency")) {
    std::string const v = args.value("--latency", std::string());
    if (!d2o::find_latency_tier(v) && v != "custom") {
      std::fprintf(stderr,
                   "--latency 的值 \"%s\" 不认识（可选：realtime / balanced / "
                   "quality / custom）\n",
                   v.c_str());
      return 2;
    }
    cfg.capture.latency = v;
    std::vector<std::string> lat_notes;
    if (d2o::validate_config(cfg, lat_notes)) {
      for (std::string const& n : lat_notes) {
        std::fprintf(stderr, "延时档位：%s\n", n.c_str());
      }
    }
  }

  std::string const path = args.value("--record", std::string());
  if (args.has("--record") && path.empty()) {
    std::printf("--record 后面要给输出文件名，例如 --record out.mp4\n");
    return 2;
  }
  opt.output_path = path;
  // 布尔开关的优先级**不是**"命令行覆盖配置文件"，而是"两处任一为真即为真"：
  // 对一个没有默认值的开关来说，"命令行里没写"只意味着"没表态"，不意味着
  // "关掉"。要让配置文件里的开关能被命令行关掉，得有 `--no-*`（下面那两条），
  // 而不是把"没写"当成"否" —— 后者会让配置文件永远无法生效。
  opt.rtsp.enabled = (args.has("--rtsp") || cfg.rtsp.enabled) && !args.has("--no-rtsp");
  bool const http = (args.has("--http") || cfg.web.enabled) && !args.has("--no-http");
  if (opt.output_path.empty() && !opt.rtsp.enabled && !http) {
    // 长驻形态（`--console` / `--worker`）走到这里说明**配置文件里两个开关都是关的**，
    // 因为配置文件不存在时 `cmd_console` 会先补上 `--http --rtsp`。这时那条
    // "要么给 --record…" 的提示是错的：用户根本没有在敲命令行，他是在双击一个
    // exe，或者服务在拉起 worker。报一句他能照做的话。
    if (args.has("--console") || args.has("--worker")) {
      std::fprintf(stderr,
                   "配置里 Web 与 RTSP 两个开关都是关的，长驻形态没有事情可做。\n"
                   "  想只开网页：把配置文件里的 web.enabled 改成 true，"
                   "或者删掉配置文件（按出厂设置起，两个都开）\n"
                   "  想只录一段：desk2onvif --record out.mp4\n");
      return 2;
    }
    std::printf("要么给 --record <文件>，要么给 --rtsp，要么给 --http"
                "（可以任意组合）。\n");
    return 2;
  }

  std::string err;
  int tmp = 0;
  if (!int_option(args, "--display", cfg.capture.display_index, 0, 64, tmp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  opt.source.display_index = static_cast<unsigned>(tmp);
  // `--camera <名|链接>`：把**通道 1** 的画面来源从桌面换成摄像头。
  //
  // 只动通道 1。通道 2 起的摄像头走 `channels.items[]`（那边的 `camera` 是
  // 权威，而且能被网页编辑），而命令行是"这一次跑起来看看"的入口 ——
  // `--list-cameras` 会把符号链接印出来供粘贴。和 `--display` 同一条规矩：
  // 命令行覆盖配置文件。
  //
  // **值不能为空**（`--record` 同一条纪律）：`--camera` 后面什么都没有时，
  // 悄悄退回"第一台"会让一条漏掉参数的启动命令看起来完全正常 —— 而它选中
  // 的可能是你根本不打算推出去的那一台。
  if (args.has("--camera")) {
    std::string const cam = args.value("--camera", std::string());
    if (cam.empty()) {
      std::printf("--camera 后面要给摄像头名字或符号链接，先用 --list-cameras "
                  "看一下有哪些。\n");
      return 2;
    }
    // `kind` 一旦设成 webcam 就**不再回到 display** —— `apply_config_to_options`
    // 刻意原样保留它，所以重载（换分辨率 / 点降档）之后这一路还是摄像头。
    opt.source.kind = "webcam";
    opt.source.camera = cam;
  }
  if (!int_option(args, "--fps", cfg.capture.fps, 1, 240, opt.fps, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--bitrate", cfg.capture.bitrate_kbps, 100, 100000, opt.bitrate_kbps, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--gop", cfg.capture.gop_seconds, 1, 60, opt.gop_seconds, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  // 线程数的默认值取自配置（**Phase W 修的**：以前这里硬编码 6，于是
  // `capture.threads` 在启动路径上根本没人读 —— 写入的是"生效值"、读出的却是
  // 常量，配置文件里那一项是一句空话，和陷阱 28 的 `tune` 一模一样）。
  // 延时档位的主旋钮就是这一项，档位展开出来却传不到编码器的话，整个档位
  // 只有标签在动。
  //
  // 区间的下界仍然是 1 而不是 0，这不矛盾：`int_option` **不检查默认值**
  // （`if (!args.has(key)) { out = fallback; return true; }`），所以配置里的
  // 0（"自动"、封顶 6）照样能通过；而命令行上显式写 `--threads 0` 会拿到
  // 一条"超出范围 [1, 128]"的用法错误。原来的顾虑（"取 0 会让下次启动把
  // 封顶 6 变成封顶 1"）说的是**配置里存着 0** 的情形，那条路没变。
  if (!int_option(args, "--threads", cfg.capture.threads, 1, 128, opt.threads, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--queue", 4, 1, 64, tmp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  opt.queue_capacity = static_cast<std::size_t>(tmp);
  if (!int_option(args, "--seconds", 0, 0, 24 * 3600, opt.seconds, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }

  // 父进程 pid（服务形态专用，见 ParentWatch）。0 = 没有父进程可盯，也就是
  // 控制台形态。上限取 int 的满量程：Windows 的 pid 是 4 的倍数、实际远小于
  // 这个数，但没必要在这里卡一个更小的上限让它变成一条"看起来像用法错误"的
  // 失败 —— 真要给一个不存在的 pid，OpenProcess 会自己说话。
  int parent_pid = 0;
  if (!int_option(args, "--parent-pid", 0, 0, 2147483647, parent_pid, err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  opt.profile = args.value("--profile", cfg.capture.profile);
  opt.preset = args.value("--preset", cfg.capture.preset);
  // tune 以前只存在于配置结构和 SessionOptions 里，**没有任何地方把它读进来**
  // —— 配置文件里写 tune 是一句空话（实际用的是 SessionOptions 的默认值），
  // 而"改了没反应"这种症状最难查。这里接上。
  opt.tune = args.value("--tune", cfg.capture.tune);
  // 帧缓冲池要比队列深一个以上，否则"池子空了"会频繁触发 ——
  // 队列里排着的帧都还占着池子里的缓冲。
  opt.pool_size = opt.queue_capacity + 2;

  // 主码流高度上限（0 = 桌面原生）。**只写上限**，采集永远按原生尺寸 ——
  // 采集时就缩放的话鼠标指针的坐标会整体偏掉。
  if (!int_option(args, "--max-height", cfg.capture.max_height, 0, 4320, opt.max_height,
                  err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 2;
  }

  // 水印。整段从配置来，命令行只给两个最常用的快捷开关 —— 四个角、字号、
  // 日期格式这些用命令行表达只会变成一串没人记得住的长选项，而它们本来就是
  // "在网页上点两下"的事。
  opt.osd = cfg.osd;
  if (args.has("--no-osd")) opt.osd.enabled = false;
  if (args.has("--osd")) opt.osd.enabled = true;
  {
    std::string const name = args.value("--osd-name", std::string());
    if (!name.empty()) {
      // `--osd-name "前门"` = 把名称项的文字定死成这个，并把它打开。
      // 定死之后它就不再跟着 ONVIF 的改名走了 —— 那是用户显式要的效果，
      // 所以不额外警告，但 `--help` 里要写清楚。
      bool found = false;
      for (auto& it : opt.osd.items) {
        if (it.type == "name") {
          it.enabled = true;
          it.text = name;
          found = true;
        }
      }
      if (!found) {
        d2o::OsdItemConfig it;
        it.type = "name";
        it.text = name;
        it.position = "lower_right";
        opt.osd.items.push_back(it);
      }
      opt.osd.enabled = true;
    }
  }

  // 自适应降载。
  opt.adaptive = cfg.adaptive;
  {
    std::string const mode = args.value("--adaptive", std::string());
    if (!mode.empty()) {
      if (mode != "off" && mode != "throttle" && mode != "auto") {
        std::fprintf(stderr,
                     "--adaptive 只能是 off / throttle / auto，收到的是「%s」。\n"
                     "  off = 不干预；throttle = 自动丢帧（默认）；"
                     "auto = 连提示都不用，自动降到下一档\n",
                     mode.c_str());
        return 2;
      }
      opt.adaptive.mode = mode;
    }
    if (args.has("--no-adaptive")) opt.adaptive.mode = "off";
  }

  // 音频。整段从配置起，命令行给三个开关 —— 增益、码率这些留在网页上。
  opt.audio = cfg.audio;
  {
    std::string const src = args.value("--audio", std::string());
    if (!src.empty()) {
      // **`--audio X` 是"音频源就是 X"，不是"再打开 X"。** 不把另一个源关掉的
      // 话，配置里勾了麦克风的人敲 `--audio system` 会得到 both —— 而他没要
      // both，命令行上也没有别的写法能表达"只要系统声音"。这和 `--onvif`
      // 能推翻配置里那个开关是同一条规则：命令行一律覆盖配置文件。
      if (src == "system") {
        opt.audio.system = true;
        opt.audio.microphone = false;
      } else if (src == "mic") {
        opt.audio.microphone = true;
        opt.audio.system = false;
      } else if (src == "both") {
        opt.audio.system = true;
        opt.audio.microphone = true;
      } else {
        std::fprintf(stderr,
                     "--audio 只能是 system / mic / both，收到的是「%s」。\n"
                     "  system = 系统声音（环回）；mic = 麦克风；both = 两个都混进去\n",
                     src.c_str());
        return 2;
      }
      opt.audio.enabled = true;
    }
    // **`--no-audio` 最后判，所以它赢** —— 和 `--no-sub` 的写法一致
    // （`… && !args.has("--no-sub")`）。两个一起给是自相矛盾，而"关掉"
    // 永远是更保守的那一个，也是唯一不会让人以为"音频开着"的那一个。
    if (args.has("--no-audio")) opt.audio.enabled = false;

    std::string const dev = args.value("--audio-device", std::string());
    // 空串是**有意义**的（= 用系统默认端点），所以不能拿它当"没给"的判据。
    // `args.has()` 才能区分"没给"和"给了空串"，而后者按前者处理是对的：
    // 用户的意图都是"回到默认"。
    if (args.has("--audio-device") && !dev.empty()) opt.audio.device = dev;
  }

  if (opt.rtsp.enabled) {
    if (!int_option(args, "--rtsp-port", cfg.rtsp.port, 1, 65535, tmp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    opt.rtsp.port = static_cast<std::uint16_t>(tmp);

    // 开头的斜杠在这里统一去掉，而不是报错让人改。命令行上写 `live` 更自然，
    // 配置文件和 NVR 界面里写 `/live` 更自然，而这是同一个东西 ——
    // 为一个人为的书写差异报错，只会让人以为是自己写错了。
    auto strip_slash = [](std::string s) {
      while (!s.empty() && s.front() == '/') s.erase(0, 1);
      return s;
    };

    // 队列按"够装几个 GOP"来定，而不是拍一个数：一个客户端卡一下，队列里
    // 最先被丢的就是最老的数据；深度不足一个 GOP 时，丢完就等于把这个 GOP
    // 整个废掉，客户端要花到下一个关键帧才恢复。
    if (!int_option(args, "--rtsp-queue", opt.fps * 3, 8, 10000, tmp, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    std::size_t const queue_capacity = static_cast<std::size_t>(tmp);

    // --- 子码流开关 -------------------------------------------------------
    // 和 RTSP/HTTP 那组布尔开关同样的语义：命令行或配置文件**任一为真即为真**，
    // 只有 `--no-sub` 能关掉它。"没写"只意味着没表态，不意味着"关掉"。
    opt.sub_enabled =
        (args.has("--sub") || cfg.rtsp.sub_enabled) && !args.has("--no-sub");
    if (!int_option(args, "--sub-height", cfg.rtsp.sub_height, 120, 2160, tmp, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    opt.sub_height = tmp;
    if (!int_option(args, "--sub-bitrate", cfg.rtsp.sub_bitrate_kbps, 32, 100000,
                    tmp, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    opt.sub_bitrate_kbps = tmp;

    // **顺序即语义**：streams[i] 对应编码器 i，push_annex_b(i, …) 按这个下标
    // 找路。主码流必须在前 —— 下标 0 在好几处被当成"那一路"（`/api/status`
    // 的 rtsp_url、给 NVR 手工填的地址、收尾报表里的推流[0]）。
    d2o::RtspStreamOptions main_stream;
    main_stream.path = strip_slash(args.value("--rtsp-path", cfg.rtsp.path));
    if (main_stream.path.empty()) {
      std::printf("--rtsp-path / rtsp.path 不能为空（写 live）。\n");
      return 2;
    }
    main_stream.fps = static_cast<unsigned>(opt.fps);
    // 只进 SDP 的 `b=AS:`，不影响编码 —— 实际码率由编码器决定。
    main_stream.bitrate_kbps = opt.bitrate_kbps;
    main_stream.queue_capacity = queue_capacity;
    opt.rtsp.streams.push_back(std::move(main_stream));

    if (opt.sub_enabled) {
      d2o::RtspStreamOptions sub;
      sub.path = strip_slash(args.value("--sub-path", cfg.rtsp.sub_path));
      if (sub.path.empty()) {
        std::printf("--sub-path / rtsp.sub_path 不能为空（写 live_sub）。\n");
        return 2;
      }
      if (sub.path == opt.rtsp.streams[0].path) {
        // 两路挂同一个路径：无论 Live555 最后让哪一个生效，现象都是一条能
        // 正常播的流，分不出它是主还是子 —— 这类错没有症状可循，只能在这里拦。
        std::printf("子码流路径不能和主码流相同（都是 %s），换一个（比如 %s_sub）。\n",
                    sub.path.c_str(), sub.path.c_str());
        return 2;
      }
      sub.fps = static_cast<unsigned>(opt.fps);
      sub.bitrate_kbps = opt.sub_bitrate_kbps;
      // 队列深度按 NALU 计，而两路每帧都是 1 个 VCL NALU —— "3 秒的量"在两路
      // 就是同一个数，照搬即可。按**字节**去等比缩小反而会让子码流更早丢包：
      // 客户端卡顿一次丢弃的帧数只取决于深度，与帧本身多大无关。
      sub.queue_capacity = queue_capacity;
      opt.rtsp.streams.push_back(std::move(sub));
    }
  }

  // --- 生效值回写进 cfg（采集与 RTSP 部分）-----------------------------
  // `GET /api/config` 和 `--save-config` 要报的是**实际生效**的配置，不是文件
  // 里写的那份 —— 命令行给了 `--fps 60` 而文件里是 30 时两者不同，而用户想看
  // 的显然是前者。上面每一处 option 都是"以 cfg 为默认值再写回 opt"，所以这里
  // 把 opt 一侧的结果收回来。
  //
  // 位置就在这里：此刻 opt 已经全部定稿，而 Web 服务还没起来，不存在"循环
  // 线程读到写了一半的 cfg"。
  //
  // 实现挪进了 `capture_session.cpp`，和它的**逆运算**
  // （`apply_config_to_options`，重载时用）放在一起 —— 这两张字段表必须一致，
  // 分开写迟早会漂移，而漂移的症状是"某个字段在重载后没变/自己变回去了"。
  d2o::sync_options_to_config(opt, cfg);

  // --- 通道规划（第五轮 Phase AD）---------------------------------------
  //
  // **只算这一次，不在分段循环里重算。** 通道的身份 —— ONVIF 端口、设备 UUID、
  // RTSP 路径 —— 全在这一份里，而它们必须在这一趟进程的生命里固定：通道 1 的
  // `WebServer` 在下面 `web.start()` 之后就不再搬家，而 WS-Discovery 那份"本机
  // 有哪几台设备"的表只在启动时宣告一次（后来改了的话，NVR 那边看到的名字和
  // 端口和实际对不上，而它不会自己重扫）。
  //
  // 改 `channels.*` 要重启，`pending` 那边本来也是这么报的：`sync_options_to_config`
  // 一个字都不碰 `channels`，所以任何改动都必然出现在"存盘的减运行侧的"里。
  //
  // 通道 1 **不在 `plans` 里** —— 它由 `capture.*` / `rtsp.*` 描述，走 `cmd_run`
  // 自己那条老路径（那条路上的行为逐字节和从前相同）。
  //
  // 机器标识在这里取一次、整趟共用：通道 2 起的 UUID 和序列号都由它派生，
  // 而每调一次 `machine_seed()` 就要读一次注册表。
  std::string const machine_seed_value = d2o::machine_seed();

  // 本机的显示输出（含"这块屏能不能抓"）。**留在函数级**，因为配置页要它：
  // `/api/sources` 把这份清单发给前端，用户才能看到"能加哪几路"。
  //
  // ⚠️ **它必须是启动时算好的这一份，不能每次请求现枚举。** 探"能不能抓"要
  // 真建一个 `IDXGIOutputDuplication`，而一块屏同时只允许一个（见
  // `channel_plan.h` 规则 2）—— 现枚举的话，用户一打开配置页就会让**正在采集
  // 的那一路**失败。所以这里算一次、之后只读。
  std::vector<d2o::ChannelDisplayInfo> displays;
  std::vector<d2o::ChannelPlan> const channel_plans = [&] {
    // 探测要真的建一次 D3D 设备 + 一次 DuplicateOutput，所以整个进程只做一次。
    std::vector<d2o::OutputInfo> const outputs = d2o::enumerate_outputs();
    displays.reserve(outputs.size());
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      d2o::ChannelDisplayInfo d;
      d.index = static_cast<unsigned>(i);   // 就是 --list-displays 那个 #N
      d.description = d2o::describe(outputs[i]);
      d.width = outputs[i].width();
      d.height = outputs[i].height();
      d.x = static_cast<int>(outputs[i].desktop_rect.left);
      d.y = static_cast<int>(outputs[i].desktop_rect.top);
      // 抓不了的屏**一条都不建**（见 channel_plan.h 规则 3）：向日葵那类间接
      // 显示驱动建出来就是一路永远黑屏的通道，而用户会去查摄像头或网络。
      d.capturable = d2o::probe_capturable(outputs[i]).ok;
      displays.push_back(std::move(d));
    }
    return d2o::plan_channels(displays, cfg.channels, opt.source.display_index,
                              opt.source.kind == "display");
  }();

  // 规划结果要**说出来**。哪几路被自动建了出来、名字/端口/路径各是什么，是
  // "NVR 里怎么加这几路"唯一能当场回答的东西 —— 而"自动发现"这件事在只有
  // 一块屏的机器上（比如这台）完全不发生，用户无从知道它在别的机器上会做什么。
  {
    unsigned enabled = 0;
    for (d2o::ChannelPlan const& p : channel_plans) {
      if (p.enabled) ++enabled;
      // `rtsp_path` **自带前导斜杠**（`validate_config` 补的、自动项合成时
      // 也是这么写的），所以这里不再加一个 —— 加了会印出 `RTSP //ch2`。
      //
      // "不建"有三种说法，这里分得开：**用户关的**（配置里那个开关）和
      // **建不了的**（同一块屏被占，`blocked_reason` 里写着为什么）。
      // 只印一句"不建"的话，第二种的用户会去配置里找一个明明是开着的开关。
      std::printf("通道 %u：%s%s%s，ONVIF 端口 %d，RTSP %s\n", p.index,
                  p.name.c_str(), p.auto_created ? "（自动发现）" : "",
                  p.enabled ? ""
                            : (p.blocked_reason.empty() ? "（配置里关着，不建）"
                                                        : "（建不了，见下）"),
                  cfg.web.port + static_cast<int>(p.index), p.rtsp_path.c_str());
    }
    if (channel_plans.empty()) {
      std::printf("通道 2 起：没有（只有一块可捕获的屏，配置里也没有 channels.items）。\n");
    } else {
      std::printf("通道 2 起：共 %zu 路，其中 %u 路启用。\n", channel_plans.size(),
                  enabled);
    }

    // **同一块屏不能开两路捕获** —— 原因整句照印。
    //
    // DXGI 的桌面复制在一块输出上同时只允许一个 `IDXGIOutputDuplication`：
    // 第二个 `DuplicateOutput` 直接失败，报的是 `E_INVALIDARG`
    // （实测如此，见 out\mc-e2e\run1.txt）—— 从"参数非法"这四个字里读不出
    // "两块屏写重了"。而且**谁先谁后是线程赛跑**（通道 2 的线程先起、通道 1
    // 的会话后跑），所以不拦的话结果有两种：要么这一路红，要么**通道 1 红**
    // 并带着整个进程退出（`run()` 失败 = 没有通道 1 = 没事可做）。
    //
    // 拦的地方在 `plan_channels` 的规则 2（那是**纯函数**，所以
    // `tests\channel_plan_check.cpp` 能逐条钉住它，不需要一台双屏机器）。
    // 这里只负责把它的结论说给用户听 —— 警告发在规划之后、起服务之前，
    // 用户在第一屏日志里就能看到，而不是等到某一路上报一个错误码。
    for (d2o::ChannelPlan const& p : channel_plans) {
      if (p.blocked_reason.empty()) continue;
      D2O_WARN("通道 %u 建不了：%s", p.index, p.blocked_reason.c_str());
    }
  }

  // Web 服务要在采集之前起来：起不来（端口被占）就该立刻退出，
  // 而不是录了十秒才发现在报错。
  d2o::WebServer web;
  // 网页预览"现在看第几路"的全机状态 + 切换要用的缓存。**声明在这里**（任何
  // 一处 `web.start()` 之前）：通道 1 的快照回调、通道 1 的预览钩子、以及通道 2
  // 起每一路的钩子都按引用捕获它，而它们分处三个不同的作用域。
  PreviewHub preview_hub;
  {
    // 现在真的有哪几路：1 恒有，其余是**计划里启用**的那些。关着的（用户关的、
    // 被挡住的）不算 —— 前端据它画那一排按钮，接口据它校验切换请求。
    std::vector<int> avail{1};
    for (d2o::ChannelPlan const& p : channel_plans) {
      if (p.enabled) avail.push_back(static_cast<int>(p.index));
    }
    preview_hub.set_available(std::move(avail));
  }
  // **会话对象也在这里就构造。** 它是默认构造，不碰 DXGI/GPU，没有任何副作用，
  // 真正的采集是在下面的 run() 里才开始的。提前声明只有一个原因：快照回调要
  // 按引用捕获它，而那个回调必须在 web.start() 之前装进 WebOptions。
  //
  // 声明顺序不能反过来：成员按声明逆序析构，web 先声明就意味着 session 先析构、
  // web 后析构。`publish()` 是采集线程经 stats_hook 调进来的，而 run() 返回时
  // 那条线程已经 join 完了，所以"web 后死"只是多一层保险，不是必需的。
  d2o::CaptureSession session;

  // --- 多通道（第五轮 Phase AD）：通道 2 起的运行时对象 -------------------
  //
  // **声明在这一层、在任何 `web.start()` 之前，而且 `reserve` 满。**
  //
  // 内容是**每一段重建一次**（见下面分段循环），但这个 vector 本身活到函数结束：
  // `wo.channels_json` 按引用捕获它，而那个回调由 web 循环线程在任意时刻调用
  // —— 包括 `session.run()` 正跑着的时候。把它挪进循环体就等于让回调拿着一个
  // 已经析构的 vector（陷阱 49：症状是"崩在别处"，任何 backtrace 都指不回这里）。
  //
  // 元素本身也必须是**堆上**的（`unique_ptr`），理由写在 `ExtraChannel` 上：
  // 每一路自己的 web 回调和采集线程捕获的是那一路的地址，而 vector 扩容会让
  // 元素搬家。
  std::vector<std::unique_ptr<ExtraChannel>> extra_channels;
  extra_channels.reserve(d2o::ChannelsConfig::kMaxChannels);
  // 通道 1 的那几行 + 攒好的 `channels` JSON。
  ChannelBoard ch_board;
  // **共享的** RTSP 服务器：一台，服务全部通道，按路径区分。整趟运行就这一个
  // 对象，每一段重新 `start()` 一次（编码器和 SDP 里的尺寸绑死在创建时）。
  std::unique_ptr<d2o::RtspServer> rtsp_server;

  // 拆掉这一趟的通道 2 起：先让各路停下来，**再 join**，最后才轮到 RTSP 服务器
  // （那个顺序是 `rtsp_server.h` 的契约，反过来的话某条线程会在服务器已经拆掉
  // 之后往它上面推 NALU）。
  //
  // 每次分段都调一次，所以它必须**可以重复调**：`extra_channels` 清空之后
  // 再调就是空转。
  auto const teardown_channels = [&]() {
    for (auto& ch : extra_channels) {
      if (ch->session) ch->session->request_stop();
    }
    for (auto& ch : extra_channels) {
      if (ch->thread.joinable()) ch->thread.join();
      // Web 停在线程 join **之后**：会话线程经统计钩子往它里面投递，
      // 先停 web 会让那条线程对着一个已经关掉的循环投递。
      if (ch->web) ch->web->stop();
    }
    extra_channels.clear();
    // **在 join 之后、element 析构之后**清那张快照表：它是这些会话的第二个
    // 持有者，先清就等于把"正在取帧的那一路"从 web 循环线程手里抽走 ——
    // 而 web 循环线程此刻还活着（`web` 是本函数的局部变量）。放在这里，
    // 上一句已经把线程 join 完了，没有人在取帧。
    preview_hub.drop_sessions();
    if (rtsp_server) {
      rtsp_server->stop();
      rtsp_server.reset();
    }
  };

  // --- 生命周期：Ctrl+C 与父进程 ----------------------------------------
  //
  // 两样都必须声明在 `session` **之后**（它们按引用捕获它），而且必须是 RAII
  // —— 这个函数有好几条 `return` 路径，手写重置迟早漏一条，而漏掉的那条会让
  // 中断处理函数或看守线程拿着一个已经析构的对象去调 `request_stop()`。
  //
  // 析构顺序也因此是对的：成员按声明逆序析构，所以 watch / hook 先于
  // `session` 销毁，join 那条线程时 session 还活着。
  CtrlHook const ctrl_hook(session);
  ParentWatch const parent_watch(session, parent_pid);

  // --- 被 Web 回调**按引用**捕获的几样，必须活到 web 析构为止 ------------
  //
  // 下面 `WebOptions` 里那几个 std::function 是捕获引用的，而它们保存在
  // `WebServer::Impl` 里、由 Web 循环线程在**任意时刻**调用 —— 包括
  // `session.run()` 正在跑的时候。
  //
  // 所以它们的声明必须在 `cmd_run` 这一层，**不能放进 `if (http) { ... }`**。
  // 那个块在 `web.start()` 之后紧接着就结束了，块内声明的对象当场析构；而
  // lambda 里留着的引用不会随之失效，它们指向的是已经析构的栈对象 ——
  // 之后每次调用读的是被复用的栈内存，写的是别人的栈帧。
  //
  // 踩过一次：`derived_serial` / `running_cfg`（当时还有一份 `cfg_pending`）
  // 原先声明在块内，症状是"POST 一次 /api/config 就崩"，而且崩在别的线程、
  // 别的位置（内存被写坏之后谁先碰到谁先死），完全指不到这里。**声明的位置
  // 就是这里的全部要点**，别为了"变量离用处近一点"把它们挪回块内。
  //
  // **同一个缺陷还有第二种形态，而且又踩了一次（2026-10-07）**：这次是
  // **闭包对象**本身被留在了块里 —— `wo.config_set` 捕获 `&apply_config`，
  // `apply_config` 捕获 `&compute_pending`。判据是"按引用捕获的目标活不活到
  // web 析构"，跟它是变量还是闭包没有关系。所以这两个 lambda 也声明在这一层
  // （见 1124 行那段），而不是在用到它们的地方。
  std::string derived_serial;
  d2o::AppConfig running_cfg;
  // `running_cfg` 是**两个线程**都会写的东西，所以它和 `cfg` 不一样：
  //   * 保存配置时由 Web 循环线程更新（`apply_config` 把能热生效的那几项同步
  //     进来）；
  //   * 重载的那一段跑完之后由主线程更新（见分段循环里那句
  //     `sync_options_to_config`）—— 那时"运行侧"就是它刚跑的那一份。
  // `cfg` 只有循环线程读写、不需要锁；这个两边都动，就必须有。锁里面做的事
  // 都只是拷几个字符串或者序列化一次配置，争用可以忽略（一次保存、一次重载、
  // 一次 GET）。**不要在这里面调任何会回头拿别的锁的东西** —— 目前两个持锁
  // 点互不嵌套，就是为了不留死锁的余地。
  std::mutex run_mu;

  // `POST /api/reload` 要按"用户刚存的那份配置"重算采集选项，而这件事发生在
  // **主线程**的分段循环里 —— `cfg` 本身却是循环线程在 `apply_config` 里用
  // `cfg = next;` 整体赋值的。和上面那个 `suggest_quality` 是同一类问题、同一个
  // 理由（见那段注释），只是这里递过去的是整份配置而不是一个字符串。
  //
  // **为什么不在 reload 的回调里顺手算**：那个回调跑在循环线程上，读 `cfg`
  // 完全安全；但它算出来的选项要交给主线程用，递过去还是得有一份拷贝加一把锁。
  // 既然锁一定要有，就让**每次保存**都刷新快照：于是"分辨率一变就重开"那条路
  // （用户没点重载，但配置可能早就改过了）拿到的也是最新的那份，而不是只有点过
  // 重载之后才是新的。
  //
  // 初值 = 开机生效值（`sync_options_to_config` 折叠完命令行、Web 起来之前），
  // 恰好就是第一段该用的那一份。
  std::mutex cfg_mu;
  d2o::AppConfig reload_cfg = cfg;

  // 「此刻有没有一条采集管线在跑」。只有 `POST /api/reload` 的回应要它 ——
  // 用来区分"正在重开"和"本来就没在录"，两者的 note 完全不同（见
  // `WebOptions::reload`）。
  //
  // 允许稍微过时：说"没在录"而其实刚起了几十毫秒，用户的配置照样会在下一段
  // 生效，因为分段循环每一趟都重算选项。所以用原子而不是锁。
  std::atomic<bool> session_running{false};

  // 「有人要求重启这个进程」/「有人要求恢复出厂」。
  //
  // **两个都必须声明在 cmd_run 这一层**，不能放进下面 `if (http) { ... }`
  // 块里：Web 那两个回调按引用捕获它们，而回调由 web 循环线程在**任意时刻**
  // 调用 —— 包括 `session.run()` 正跑着的时候；而那个块在 `web.start()` 之后
  // 紧接着就结束了。块内声明的对象当场析构，lambda 里留着的引用却不会随之失效，
  // 指向的是随后被复用的栈内存（陷阱 49：症状是"崩在别处"，任何 backtrace 都
  // 指不到真正的原因）。
  //
  // **两个位分开而不是一个枚举**：恢复出厂**包含**一次重启，但它还要在收尾时
  // 多做几件事。用一个枚举也能写，但那样"恢复出厂之后要不要重启"就变成一个要
  // 读代码才知道的问题；两个位各自为真时，`factory_reset_requested` 那一支顺带
  // 把重启也做了，读起来是直的。
  std::atomic<bool> restart_requested{false};
  std::atomic<bool> factory_reset_requested{false};

  // 「谁点了恢复出厂」。**必须单独存一份、而且必须现在存。**
  //
  // 恢复出厂是本进程里唯一一个"写审计的人"和"清审计的人"不是同一层的动作：
  // 写它的那层（web）只知道请求上下文（来源 IP + 用户名），而清它的那层
  // （主线程收尾）那时手上只剩一个原子位。所以那个"谁"要在这里被接住 ——
  // 晚一步它就已经被自己删掉了，而恢复出厂之后日志的第一行只能报出一个
  // 不知道是谁的动作，恰好是审计最该回答的那个问题。
  //
  // 用锁不用原子：它是一对 `std::string`，跨线程写（web 循环线程）读（主线程
  // 收尾）没有任何顺序保证可言。争用可以忽略（一辈子写一次）。
  std::mutex who_mu;
  d2o::AuditWho factory_reset_who;

  // `perf_suggested_quality` 的来源。**必须是一把锁加一个字符串，不能直接读
  // `cfg`。**
  //
  // 读它的地方是下面那个 stats hook —— 跑在**采集线程**上；而 `cfg` 是由
  // 循环线程在 `apply_config` 里用 `cfg = next;` **整体赋值**的（那是一次
  // `AppConfig` 的拷贝构造，不是原子写）。直接读就是数据竞争：轻则读到半新
  // 半旧的档位（建议一个用户没选的档），重则是 std::string 的内部指针在拷贝
  // 途中被读走 —— 那是真正的崩溃，而且只在"正好每秒那一次统计撞上一次保存"
  // 时才出现。
  //
  // 所以：**算在写的那一侧，读的那一侧只拿锁拷一个字符串**。`apply_config`
  // 每次保存跑一次、hook 每秒跑一次，争用可以忽略。
  std::mutex suggest_mu;
  std::string suggest_quality;
  // 开机就算一次：配置早就定稿了（命令行覆盖在上面的回写段里已经收回 `cfg`），
  // 而且**在这之前还没保存过任何东西** —— 不在这里播种的话，一个开箱就跑不动
  // 的机器要等到用户去网页上存一次配置才会看到那句建议。
  suggest_quality = d2o::suggest_lower_quality(cfg);

  // ---- 操作日志（审计，Phase V）-------------------------------------------
  //
  // **声明在 `cmd_run` 这一层（陷阱 49）**：Web 与 ONVIF 两侧的回调都按引用
  // 捕获它，而它们的地址存在 `WebServer::Impl::opt` 里，由 Web 循环线程在
  // `session.run()` 正跑着的时候调用 —— 块内声明的对象活不到那一刻。
  //
  // 落在 `<配置目录>\logs\`，与 `desk2onvif.log` 同一个父目录：用户问「日志
  // 存哪了」，答案要是一个他自己打得开的路径，而不是程序内部才知道的地方。
  //
  // 命令行那两项**折进 `cfg`**（与 `--no-onvif` 折进 `cfg.onvif.enabled` 是
  // 同一条规矩）：`GET /api/config` 报的必须是**生效值**，否则配置里写着
  // `audit: true` 而一行日志都不写，界面上看着一切正常。代价是 `--no-audit`
  // 会被后续任何一次保存带回磁盘 —— 这是"配置即真相"必然的代价，写进 README。
  d2o::AuditLog audit;
  {
    if (args.has("--no-audit")) cfg.log.audit = false;
    if (args.has("--audit")) cfg.log.audit = true;
    std::string ierr;
    if (!int_option(args, "--audit-keep-days", cfg.log.audit_keep_days, 0, 3650,
                    cfg.log.audit_keep_days, ierr)) {
      std::fprintf(stderr, "%s\n", ierr.c_str());
    }

    std::filesystem::path const cfg_dir =
        std::filesystem::path(config_path).parent_path();
    std::filesystem::path const audit_dir =
        (cfg_dir.empty() ? std::filesystem::path(".") : cfg_dir) / "logs";
    std::string aerr;
    if (!audit.open(audit_dir.string(), cfg.log.audit_keep_days, cfg.log.audit,
                    aerr)) {
      // 审计写不出来**不该让程序起不来**（与 `Log::set_file` 打不开文件
      // 是同一个取舍）。但要说出来 —— 一个以为自己在被审计的系统，比一个
      // 知道自己没被审计的系统危险得多。
      D2O_WARN("操作日志不可用：%s（其余功能不受影响）", aerr.c_str());
    } else if (cfg.log.audit) {
      D2O_INFO("操作日志：%s（保留 %d 天%s）", audit_dir.string().c_str(),
               cfg.log.audit_keep_days,
               cfg.log.audit_keep_days <= 0 ? "，不剪枝" : "");
    }
  }

  // 设备时钟与摄像头名称。**采集线程和 ONVIF 共享同一份**：水印那个「名称」
  // 项每帧现读它，而 NVR 改名/对时是写它 —— 各存一份的话，改完名水印不会变，
  // 而两边看起来都对（见 device_state.h）。
  //
  // 名字在**这里**播下种子，而不是等 WebServer 起来再推：ONVIF 关着的时候
  // 水印的名称项也该有字。运行中的改名走 `OnvifService::set_identity`。
  d2o::DeviceState device_state;
  device_state.set_channel_name(cfg.onvif.device_name);
  // 采集侧（水印）拿到的是**只读**指针：名字和时钟都只从 ONVIF / Web 那边写，
  // 采集线程每帧读。给它一个非常量指针等于邀请采集线程去改这两样东西。
  opt.device = &device_state;

  // ---- 设备时间的启动种子（Phase U）---------------------------------------
  //
  // `config.time.offset_s` 的语义是**"重启后要恢复的那个偏移"**（见
  // ntp_syncer.h 的文件头）。手动校准（网页 / NVR 下发）重启后不会自己再来
  // 一次，所以它必须落盘、也必须在启动时灌回去；NTP 那条路起来会自己再对
  // 一次，所以它不进配置 —— 两边这样分工才不会出现"配置里写的值"和"设备
  // 此刻在用的值"长期对不上。
  //
  // **两条分支都走一次 `DeviceState` 的写入口**，哪怕偏移是 0：0 也是一个
  // 明确的设备时间（= 主机时间），而 `DeviceState` 的初值本来就是这个。
  // 这里写 `reset_time()` 是**形式上的**归零、不是修正什么 —— 它的实在意义
  // 在于让"启动这一刻设备时间被确定过"这件事有一条可读的路径（`synced_`
  // 与 `synced_at_` 由它落定），而不是留一个"从没被设过"的模糊状态给后面
  // 那些 `GetSystemDateAndTime` 去猜。
  if (cfg.time.offset_s != 0) {
    device_state.set_device_time(std::chrono::system_clock::now() +
                                 std::chrono::seconds{cfg.time.offset_s});
    D2O_INFO("设备时间已按配置恢复：相对主机偏移 %+d 秒。",
             cfg.time.offset_s);
  } else {
    device_state.reset_time();
  }

  // ---- 周期对时的执行者（Phase U）-----------------------------------------
  //
  // **声明在 `cmd_run` 这一层（陷阱 49）**：四个 Web 回调按引用捕获它，而它们
  // 的地址存在 `WebServer::Impl::opt` 里、比下面那个 `if (http) { ... }` 块活
  // 得久。理由和 `device_state` / `live_osd` 是同一条。
  //
  // 指针参数是 `&device_state`（非只读）—— 与 `opt.device` 那条"只读"的纪律
  // **不冲突**：那条约束的是**采集线程**不许写设备状态，而这条线程的职责恰恰
  // 就是写它（和 ONVIF 的 `SetSystemDateAndTime`、网页的手动校准同一个落点）。
  // 三者都写 `DeviceState`、都通过它自己的原子量，没有共享的裸数据。
  d2o::NtpSyncer ntp_syncer;
  ntp_syncer.set_config(cfg.time);
  // 审计：**每一次尝试都落一行，成败都要有**。
  //
  // 这一行是必需的、不是补充：`SetSystemDateAndTime DateTimeType=NTP` 那条路在
  // ONVIF 侧写的那一行结尾写着"对时结果见随后的 ntp.sync 一行"（`onvif_service.cpp`
  // 的 `note_audit` 调用点）—— 少了它，那半句话就把读日志的人指向一个不存在的地方。
  //
  // **来源与身份都是 `system`**：这次对时是设备自己发起的（周期到了，或者被
  // 上面那两处请求踢了一下），不是某个 IP 干的。真正的来源在那两行里 ——
  // 「谁要求对时」和「对时结果是什么」是两件事，各写一行、各说一半。
  //
  // 回调跑在**对时线程**上（`NtpSyncer::on_sync` 的注释），而 `audit.note` 自带
  // 互斥量，所以这里是安全的。
  ntp_syncer.on_sync = [&audit](d2o::NtpSyncer::Status const& st) {
    std::string d = "服务器 " + st.server + "：";
    if (st.last_state == "ok") {
      // 符号位**自己拼**：`std::to_string(-120)` 已经带负号了，而正数不带 ——
      // 只写数字的话 "+120" 和 "120" 是两串不同的字节，读日志的人要自己去猜
      // 正负号是不是被截掉了。
      d += "成功，偏移 ";
      if (st.last_offset_s >= 0) d += "+";
      d += std::to_string(st.last_offset_s) + " 秒（stratum " +
           std::to_string(st.last_stratum) + "）";
      // 「算出来了」和「用上了」分开写：手动模式下按"立刻对时"是一个正常的用法
      // （用户想试试填的服务器通不通），那时算得出来、但设备时间不动。合成一句
      // "对时成功"的话，读日志的人会以为钟被挪了。
      d += st.last_applied ? "，已应用到设备时间"
                           : "，当前是手动模式，设备时间未改动";
    } else {
      d += "失败（" + st.last_state + "）";
      if (!st.last_error.empty()) d += "：" + st.last_error;
      d += "；累计失败 " + std::to_string(st.fail_count) + " 次";
    }
    audit.note(d2o::AuditWho{"system", "system"}, "ntp.sync", d);
  };
  ntp_syncer.start(&device_state);

  // 水印配置的"当期那一份"（见 common/live_osd.h）。**声明在这里是硬要求**
  // （陷阱 49 的同一个形状）：`apply_config` 与 `on_osd_patch` 两个闭包按引用
  // 捕获它，而它们的地址存在 `WebServer::Impl::opt` 里，由 Web 循环线程在
  // `session.run()` 正跑着的时候调用 —— 块内声明的对象活不到那一刻。
  //
  // 初值取 `opt.osd` 而不是 `cfg.osd`：两者此刻**值相同**（`sync_options_to_config`
  // 已经把命令行那几项折进 cfg 了），但写 `opt.osd` 才是"采集侧真正在用的那份"
  // 这一句话的字面意思 —— 而这份初值就是"启动时那一次写入"，不需要额外再 set 一遍。
  d2o::LiveOsd live_osd(opt.osd);
  opt.live_osd = &live_osd;

  // --- 下面这两个闭包也必须在 `cmd_run` 这一层，和上面那段同一个理由 --------
  //
  // 上面那条规矩（见 1041 行附近）说的是"**变量**"，而这里翻车的是"**闭包对象
  // 本身**"：`WebOptions` 里三个回调（`config_set` / `onvif.on_patch` /
  // `onvif.on_osd_patch`）捕获的是 `&apply_config`，而 `apply_config` 又捕获了
  // `&compute_pending`。它们原来声明在下面那个 `if (http) { ... }` 里，块一结束
  // 就析构 —— 可 `WebServer::Impl` 存着指向它们的引用，Web 循环线程之后每次保存
  // 都在读一块已经死掉的栈内存。
  //
  // **症状（2026-10-07 实测，查了一整轮）**：点过 `POST /api/reload` 之后第一次
  // `POST /api/config` **必静默 SIGSEGV** —— 没有 WER 事件、`out.log` 0 字节、
  // stderr 日志停在半句话上。崩在 `pthread_mutex_lock` 的第一条指令
  // `mov (%rcx),%r10`，也就是"传进来的 mutex 指针是野的"；而那个野指针正是从
  // 死掉的闭包里读出来的 `&run_mu`。崩的位置每次运行都不一样（栈槽被谁复用就崩
  // 在哪），所以任何 backtrace 都指不回这里。
  //
  // **为什么偏偏要重载之后才发作**：块结束之后没人立刻碰那块栈槽，重载之前那几次
  // 保存读到的还是没被覆盖的旧值。而分段循环体**尾部**那段（`next_file`、`substr`
  // 拼接、printf 的临时量，见下面 `run()` 返回 `Restart` 之后的那几行）是**唯一**
  // 会复用那块栈槽的代码 —— 第一段录制还没跑完时它一次都没执行过。
  //
  // 判据不是"变量还是闭包"，而是**"按引用捕获的目标是否活到 web 析构"**。
  // 把这两段挪回 `if (http)` 里（哪怕只是"离用处近一点"）会让这个 bug 原样复活；
  // 声明在这一层之后，闭包里的捕获**再也不可能指到块内的东西**（编译期就挡）。
  //
  // `pending` = 「存盘那份」与「运行侧那份」的差。**每次现算，不缓存。**
  //
  // 缓存过的版本有两个写入者（保存时一处、一次重载跑完之后另一处），漏掉
  // 任何一处都会让界面显示"需要重启才生效" —— 而那一项其实已经生效了。
  // 用户于是会去重启一次程序，重启完发现什么都没变，从此不再相信那个列表；
  // 而那个列表正是这一整套 `applied` / `pending` 机制存在的唯一理由。
  //
  // 现算的代价是两次 `config_to_json` + 一次 diff，而它只发生在用户打开
  // 配置页的时候。用它换掉"两个写入者必须同时记得更新一份缓存"，这笔账
  // 在任何一天都划算。
  auto compute_pending = [&cfg, &running_cfg, &run_mu]() {
    std::vector<std::string> p;
    std::lock_guard<std::mutex> lk(run_mu);
    d2o::diff_config(cfg, running_cfg, p);
    return p;
  };

  /// 落盘 → 推热字段 → 重算 `pending`。**保存配置和 ONVIF 的 `Set*` 共用这一条。**
  ///
  /// 两条路各写一遍的话，"改了却没进 pending"这种错会只发生在其中一条上，
  /// 而它正是这一整套机制要防的那一种（界面上显示"已生效"）。
  /// **`next` 按值收**：调用方自己以"当前生效值"为底去改，改完交进来。
  auto apply_config = [&cfg, &running_cfg, &run_mu, &compute_pending,
                       &config_path, &web, &derived_serial, &suggest_mu,
                       &suggest_quality, &cfg_mu,
                       &reload_cfg, &live_osd, &device_state,
                       &ntp_syncer](d2o::AppConfig next,
                                    d2o::ConfigApplyResult& out,
                                    std::string& cerr) {
    std::vector<std::string> notes;
    d2o::validate_config(next, notes);

    // --- 硬件码只读：`hardware_id` / `serial_number` / `device_uuid` --------
    //
    // **锁在这里，不在 `config_from_json` 里。** 后者还得能读文件 —— 在那边
    // 剥掉这三个字段会顺手破坏"配置往返一致"和用它的既有断言，而且手改
    // `config.json` 是**故意留着的逃生门**（用户明确选的取舍：网页上不许改，
    // 文件里可以改）。锁在"网页提交进来之后、落盘之前"这一步，正好两条都保住。
    //
    // `next` 是调用方以"当前生效值"为底改出来的，所以拿 `cfg`（= 上一次落盘
    // 的那份）当权威。这段必须在 `save_config` **之前** —— 放后面的话磁盘上
    // 已经写进那个被拒的值了，而内存里是旧值，重启一次就"变回去"。
    //
    // `OnvifDevicePatch` 不带这三个字段（实测），所以 NVR 的 `Set*` 那条路一行
    // 都不用改：它根本不经过这里。
    if (next.onvif.hardware_id != cfg.onvif.hardware_id ||
        next.onvif.serial_number != cfg.onvif.serial_number ||
        next.onvif.device_uuid != cfg.onvif.device_uuid) {
      // **要说出来。** 静默忽略的后果不是"少了一个功能"，是下一个人以为这个
      // 校验没做、于是再去做一遍，或者以为保存坏了。
      // 句子只点名"哪几项被拒"，**不回显值** —— 值本身不是秘密，但把"用户
      // 试图写进去什么"原样吐回去没有意义，而这里最好和审计那条纪律一致。
      std::string which;
      auto const add = [&which](char const* name) {
        if (!which.empty()) which += " / ";
        which += name;
      };
      if (next.onvif.hardware_id   != cfg.onvif.hardware_id)   add("硬件 ID");
      if (next.onvif.serial_number != cfg.onvif.serial_number) add("序列号");
      if (next.onvif.device_uuid   != cfg.onvif.device_uuid)   add("设备 UUID");
      out.notes.push_back(
          "设备标识（" + which + "）由本机硬件固定，未采纳本次修改；"
          "确实要改请直接编辑 config.json。");
      next.onvif.hardware_id   = cfg.onvif.hardware_id;
      next.onvif.serial_number = cfg.onvif.serial_number;
      next.onvif.device_uuid   = cfg.onvif.device_uuid;
    }

    if (!d2o::save_config(next, config_path, cerr)) return false;

    // ---- 落盘成功之后才谈生效 ----------------------------------------
    // **顺序不能反。** 先热改再落盘的话，写盘失败时运行中的服务已经被改了，
    // 而磁盘上还是旧值 —— 重启一次就"变回去"，属于过几天才发作、且看起来
    // 毫无来由的怪事。
    //
    // 这一段跑在**循环线程**上（libuvcpp 的 web 处理器就在那条线程），
    // 所以 `set_identity` 可以直接同步调，详见它的注释。
    d2o::OnvifIdentity nid = d2o::identity_from_config(next.onvif);
    // 派生规则必须和启动时一致，否则"清空序列号"这一项热生效和重启会得到
    // 两个不同的结果。
    if (nid.serial_number.empty()) nid.serial_number = derived_serial;
    // 把派生值写回 `next` —— 启动时也做同一件事（那边是 cfg.onvif.serial_number
    // = wo.onvif.serial_number）。少了这一步，用户清空序列号之后
    // `onvif.serial_number` 会**永远挂在 pending 里**：配置里是空串、运行侧
    // 是派生值，于是每次比较都不相等 —— 而两边其实完全一致，重启一百次也
    // 不会有任何变化。落盘的那份仍然是空串（那正是用户的意思："自己派生"）。
    next.onvif.serial_number = nid.serial_number;
    bool const identity_live = web.apply_onvif_identity(nid);
    if (identity_live) {
      // 运行侧真的换成新身份了 —— 快照跟上，这几个字段于是不进 pending。
      // 用 identity_to_config 而不是手写九个赋值：它和 identity_from_config
      // 是一对，字段集只有一个来源。
      std::lock_guard<std::mutex> lk(run_mu);
      d2o::identity_to_config(nid, running_cfg.onvif);
    }

    // Web 控制台的登录开关与凭据也是**当场生效**的。理由和 ONVIF 身份那条
    // 一样：它不碰编码器、不碰 socket，`SessionStore` 只在循环线程上被读，
    // 而这段代码就在循环线程上 —— 改它就是换几个字符串加一个 bool。
    //
    // **改口令立刻生效，但不会把已经登录的人踢下线**：令牌签的是每进程一份
    // 的随机密钥而不是口令（见 web_session.h）。要立刻踢掉所有人只能重启。
    // 这一点写进文档，而不是让"改了口令还在线"看起来像个 bug。
    bool const web_live =
        web.apply_web_credentials(next.web.auth_enabled, next.web.user,
                                  next.web.password);
    if (web_live) {
      std::lock_guard<std::mutex> lk(run_mu);
      running_cfg.web.auth_enabled = next.web.auth_enabled;
      running_cfg.web.user = next.web.user;
      running_cfg.web.password = next.web.password;
    }

    // 水印：**当场生效**（Phase S）。三条路都要走这三件事，顺序也固定：
    //
    //   1. `live_osd.set()` —— 采集线程下一拍取到新的一份，画面下一帧就变
    //   2. `web.apply_osd_config()` —— `GetOSDs` 报的是存盘的配置，不推的话
    //      用户在网页上删掉一项、转头在 NVR 的 OSD 页面又看到它
    //   3. `running_cfg.osd = next.osd` —— **这条才是"已生效"这句话的依据**
    //
    // 第 3 条是本轮从"故意不动"翻过来的：`pending` 的定义是"存盘的减运行侧的"，
    // 所以只要水印真的热生效了，运行侧就必须跟上 —— 否则 `osd.*` 会永远挂在
    // pending 里，用户看到"改了要重启"、重启完发现什么都没变，从此不再相信
    // 那个列表（而那个列表是这一整套机制唯一的存在理由）。
    //
    // ⚠️ **改这三行的时候要和 `on_osd_patch` 一起改。** 那一条是 NVR 的路，
    // 落点相同、理由不同（它还有一个防抖，见那里的注释）。
    live_osd.set(next.osd);
    web.apply_osd_config(next.osd);
    {
      std::lock_guard<std::mutex> lk(run_mu);
      running_cfg.osd = next.osd;
    }

    // 时间设置：**当场生效**（Phase U）。理由与水印那一段**逐条同构**，
    // 不再重复论证（`pending` 的定义、运行侧必须跟上、三件事的固定顺序）。
    //
    // 落地顺序也是三件事：
    //   1. 把偏移推进 `DeviceState`（**只在真的变了的时候**，见下）
    //   2. `ntp_syncer.set_config()` —— 模式/服务器/间隔改了要让对时线程知道；
    //      从手动切到 NTP 时它还会**立刻**对一次（不等一个周期，见那里）
    //   3. `running_cfg.time = next.time` —— "已生效"这句话的依据
    //
    // 第 1 条那句"只在变了的时候"不是省一次原子写的优化，是**正确性**：
    // `set_device_time()` 会顺手把 `synced_at_` 覆写成"现在"。无条件调它的话，
    // 用户随便改个码率存一次盘，状态页上的"最后同步时刻"就往前跳一下 —— 而
    // 那个数正是用来判断"对时到底有没有在跑"的，一个会自己乱跳的读数比没有
    // 读数更坏（陷阱 88 的同一族：一帧自相矛盾的读数比缺一个读数难查得多）。
    //
    // 读 `cfg.time.offset_s` 当"改之前的值"是安全的：`cfg = next;` 在这段
    // **之后**，而这条线程就是唯一写 `cfg` 的那条。
    if (next.time.offset_s != cfg.time.offset_s) {
      device_state.set_device_time(std::chrono::system_clock::now() +
                                   std::chrono::seconds{next.time.offset_s});
    }
    ntp_syncer.set_config(next.time);
    {
      std::lock_guard<std::mutex> lk(run_mu);
      running_cfg.time = next.time;
    }

    std::vector<std::string> changed;
    d2o::diff_config(cfg, next, changed);  // 这次真的改动了哪些字段
    cfg = next;

    // 主线程那个分段循环要能按新配置重算选项，而它不能直接读 `cfg`
    // （见 `reload_cfg` 的注释）。**放在 `cfg = next;` 之后**：两者必须同步
    // 更新，中间隔着的任何一句都可能抛/返回，那样快照就会落后于 cfg，
    // 而症状是"点重载重开了一遍还是老参数"。
    {
      std::lock_guard<std::mutex> lk(cfg_mu);
      reload_cfg = cfg;
    }

    // pending 是**全量**重算（`compute_pending` 每次现算），不是往上次的
    // 列表里追加 —— 追加的话"把某一项改回原值"会让它一直挂在待重启里，
    // 而那一项其实早就不需要重启了。
    std::vector<std::string> pending = compute_pending();

    // 「降到下一档」那个建议跟着新配置重算。**重算而不是按"用户点了降档"
    // 特判**：用户也可以在网页上把档位**调高**，那时建议必须跟着回到更低
    // 的那一档，否则横幅上会出现"降到 流畅"而当前已经是 流畅（因为它是从
    // 更早的那次保存里算出来的）。这里算的是"当前配置的下一档"，是无状态的。
    {
      std::lock_guard<std::mutex> lk(suggest_mu);
      suggest_quality = d2o::suggest_lower_quality(cfg);
    }

    // applied = 这次变了 ∩ 不在 pending 里（也就是：真的当场生效了）。
    // 没变的字段不进 applied —— "已生效"报给用户看的是**这次保存做了什么**。
    std::vector<std::string> applied;
    applied.reserve(changed.size());
    for (std::string const& f : changed) {
      if (!std::binary_search(pending.begin(), pending.end(), f)) {
        applied.push_back(f);
      }
    }

    out.applied = std::move(applied);
    out.pending = std::move(pending);

    for (std::string const& n : notes) {
      D2O_WARN("保存的配置里有越界值，已夹到合法区间：%s", n.c_str());
    }
    return true;
  };

  if (http) {
    d2o::WebOptions wo;
    wo.enabled = true;
    wo.host = args.value("--http-host", cfg.web.host);
    if (!int_option(args, "--http-port", cfg.web.port, 1, 65535, tmp, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return 2;
    }
    wo.port = tmp;

    // Web 控制台的登录。**方向和 ONVIF 那两个开关一致**：命令行能推翻配置。
    // 口令用哨兵区分"没给"和"给了空串"——配置文件里的空口令是**合法**的
    // （意思是"不设口令"），所以不能写成"空就当没配"。
    wo.auth_enabled = cfg.web.auth_enabled && !args.has("--no-http-auth");
    wo.auth_user = args.value("--http-user", cfg.web.user);
    wo.auth_password = args.has("--http-password")
                           ? args.value("--http-password", "")
                           : cfg.web.password;
    if (wo.auth_enabled && wo.auth_password.empty()) {
      // 空口令**不是**"不鉴权"：登录框照常出现、校验照走，任何知道用户名的人
      // 按一下回车就进去了 —— 一个看起来有门的门。这比明说没鉴权更危险，
      // 因为它会让人以为自己是安全的。
      D2O_WARN("Web 控制台的口令是空的：登录框照常出现，但任何人按一下回车就能进。"
               "请用 --http-password 或配置文件的 web.password 设一个，"
               "或者显式用 --no-http-auth 关掉登录（那样前端会跳过登录页，"
               "不会摆一道假门）。");
    }

    // ONVIF 的身份、凭据与码流描述。HTTP 端口由 WebServer 在 bind 之后自己填，
    // 这里填的是"它还不知道"的那部分。
    // 开关的方向是"命令行能推翻配置"，不是"配置能推翻命令行"：
    //   --onvif              无论配置怎么写都开（用来临时试）
    //   --no-onvif           无论配置怎么写都关（用来临时停）
    //   都没有                看配置（默认 true，见 config.h）
    // 顺序上 --no-onvif 优先，这样 `--onvif --no-onvif` 这种自相矛盾的命令行
    // 落到"关"—— 安全的那一侧。
    wo.onvif_enabled = (cfg.onvif.enabled || args.has("--onvif")) && !args.has("--no-onvif");
    // WS-Discovery **跟着 ONVIF 走**。反过来的话，`--no-onvif` 之后设备还在
    // 组播 Hello/ProbeMatch 里报 XAddrs 指向 `/onvif/device_service`，
    // 而那个路径已经 404 了 —— NVR 界面上会出现一台"能发现、加不上"的摄像机，
    // 这是最难从日志看出原因的一种故障。
    wo.wsd_enabled = wo.onvif_enabled && !args.has("--no-wsd");
    // 用户只说了"关 ONVIF"却没说"关 WS-Discovery"时，把这一条连带的后果讲清楚
    // —— 它是那种"NVR 上看得见、点不进去"的故障，日志里没理由留白。
    if (!wo.onvif_enabled && !args.has("--no-wsd")) {
      D2O_INFO("ONVIF 关掉了，WS-Discovery 一并关掉：设备继续在组播里报 XAddrs 的话，"
               "NVR 会搜到一台加不上的摄像机（那个地址已经不存在）。");
    }
    // 设备状态接进 Web：`SetSystemDateAndTime` 的落点，也是水印「名称」的来源。
    // **和 ONVIF 开不开无关** —— 水印不管 NVR 在不在都该有名字和钟，所以这一行
    // 在 `if (wo.onvif_enabled)` 之外。名称的**初值**由上面那次
    // `set_channel_name` 播下，不靠 ONVIF 推送（ONVIF 关着时没人推）。
    wo.device_state = &device_state;
    wo.onvif.user = args.value("--onvif-user", cfg.onvif.user);
    // 口令的优先级要小心：配置文件里的空口令是**合法**的（表示"不设口令"），
    // 所以不能用 "空就当没配"。config_to_json 抹掉口令时写的是哨兵而不是空串，
    // 正是为了让"空口令"和"没配"在往返中不混淆（见 config.h）。
    wo.onvif.password =
        args.has("--onvif-password") ? args.value("--onvif-password", "") : cfg.onvif.password;
    // 空口令**不是**"不鉴权"：Digest 挑战照发、HA1 = md5(user:realm:)，于是任何
    // 知道用户名（默认 admin，就写在 --help 里）的人都能进。结果是"以为配了口令、
    // 其实谁都能进"。默认值已经是 admin，走到这里说明有人显式写成了空 —— 那就
    // 说清楚，别让它在日志里静默通过。
    if (wo.onvif_enabled && wo.onvif.password.empty()) {
      D2O_WARN("ONVIF 口令为空：任何知道用户名（%s）的人都能访问，"
               "NVR 添加通道时口令栏留空即可。用 --onvif-password 或配置文件的 "
               "onvif.password 设一个。",
               wo.onvif.user.c_str());
    }
    wo.onvif.realm = "Desk2ONVIF";

    // --- 设备身份：**第一次启动固化一次，此后永远用配置文件里那份** --------
    //
    // 序列号与 device_uuid 是同一条规则、同一个写入点，所以放在一起。种子只吃
    // hostname + MachineGuid（MAC 从第五轮起不参与，见 net_identity.cpp），
    // 换来的是"换网卡/插拔扩展坞都不变"。
    std::string const& seed = machine_seed_value;   // 上面已经取过一次，见那里
    bool const need_uuid   = cfg.onvif.device_uuid.empty();
    bool const need_serial = cfg.onvif.serial_number.empty();
    if (need_uuid)   cfg.onvif.device_uuid   = d2o::make_uuid_urn(seed);
    if (need_serial) cfg.onvif.serial_number = d2o::make_serial(seed);

    // **必须在这里显式存一次，不能指望下面那道"启动回写"落盘。**
    // 那道闸门比的是"回写前后两份序列化是否相同"，而这里改的正是 `cfg`
    // 本身 —— 等判据取样的时候改动早就在里面了，闸门看不见它，于是不会存。
    // 结果就是每次都重推一遍、磁盘上那份永远缺这两个字段。
    // （Phase AA 第 5 条把"启动即回写"补上了，但那只覆盖"命令行或安装包给的
    // 值盖过了文件里的值"那一类；"文件里本来就缺这一项"是另一次改动。）
    //
    // 写盘失败**不当致命错误**：这一次跑得下去，只是下次启动还要重推。
    if (need_uuid || need_serial) {
      std::string serr;
      if (d2o::save_config(cfg, config_path, serr)) {
        D2O_INFO("设备身份已固化：%s 写进 %s",
                 need_uuid && need_serial
                     ? "serial_number 与 device_uuid"
                     : (need_uuid ? "device_uuid" : "serial_number"),
                 config_path.c_str());
      } else {
        D2O_WARN("设备身份写不进 %s（%s）。下次启动会重新推导 —— 两次推出来的值"
                 "若不同，NVR 那边会多出一台设备。",
                 config_path.c_str(), serr.c_str());
      }
    }
    // 配置里那份是权威的（上面刚保证过它非空）。放 `wo.device_uuid` 而不是
    // 重新 make_uuid_urn(seed)：WS-Discovery 用的是这一个，而它必须和
    // GetDeviceInformation 报的那个、以及配置文件里写着的那个是同一个值。
    wo.device_uuid = cfg.onvif.device_uuid;
    wo.onvif.uuid  = wo.device_uuid;
    // 只算一次、只存一份（变量本身声明在上面的 `if (http)` 之外，理由见那里）：
    // 启动时填身份和运行中热改身份都要用同一个值。
    derived_serial = cfg.onvif.serial_number;

    // 身份那一段走**和运行中热改完全相同的两个函数**。两条路各写一遍字段
    // 列表的话，加一个身份字段只会更新一处，症状是"启动时对、热改之后不对"
    // （或反过来），而两边看起来都没错。
    d2o::OnvifIdentity ident = d2o::identity_from_config(cfg.onvif);
    ident.user = wo.onvif.user;  // 命令行可能覆盖过配置，以覆盖后的为准
    ident.password = wo.onvif.password;
    if (ident.serial_number.empty()) ident.serial_number = derived_serial;
    d2o::apply_identity(wo.onvif, ident);
    // RTSP 没开的时候也报 554：NVR 会把 GetStreamUri 的地址存下来，
    // 报 0 毫无意义，而报真实端口只会让"没开 RTSP"这件事更晚被发现。
    wo.onvif.rtsp_port = opt.rtsp.enabled ? opt.rtsp.port : 554;
    wo.onvif.http_port = tmp;   // 先填着，真实值由 start() 覆盖
    // 水印配置的**权威副本**。`GetOSDs` 直接答它，所以这份必须和存盘的一致
    // （含 `--osd` / `--no-osd` 这些命令行覆盖 —— 上一行那个 `cfg.osd` 在
    // 1037 行已经把 `opt.osd` 写过来了，所以这里读的是覆盖之后的值）。
    //
    // **画面上的水印不是这份。** 采集那边用的是 `SessionOptions::osd`，见
    // `capture_session.cpp` 里 `run()` 顶上的快照。两份分家的代价写在
    // `OnvifServiceOptions::osd` 的注释里。
    wo.onvif.osd = cfg.osd;

    // 每一路 RTSP 就是一个 ONVIF Profile，**顺序和 opt.rtsp.streams 一致**。
    // 不从 streams 直接生成而是逐个填，是因为 Profile 里还有几项和推流无关的
    // 描述（name、h264_profile）；但循环的下标必须对齐 —— `set_video_size(i,…)`
    // 就是靠这个下标找 profile 的，错位了会让 NVR 把子码流的分辨率报成主码流的。
    //
    // 分辨率暂时用默认值，采集打开后会由统计通道上报真实尺寸 —— Web 服务比
    // 采集先起，这里拿不到桌面尺寸。
    for (std::size_t i = 0; i < opt.rtsp.streams.size(); ++i) {
      d2o::OnvifStreamProfile p;
      // token **不要带空格**：有些 NVR 会把它原样拼进 URL 查询串而不做转义。
      p.token = (i == 0) ? "main" : "sub";
      p.name = (i == 0) ? "main" : "sub";
      p.fps = opt.fps;
      p.bitrate_kbps = (i == 0) ? opt.bitrate_kbps : opt.sub_bitrate_kbps;
      p.gop_seconds = opt.gop_seconds;
      p.rtsp_path = "/" + opt.rtsp.streams[i].path;
      // `preset=ultrafast` 会把档位压到 Constrained Baseline（x264 关掉了
      // CABAC/8x8dct），此时报 Main 是有 NVR 会拒绝的 —— 报低不报高。
      p.h264_profile = (opt.preset == "ultrafast") ? "Baseline" : "Main";
      // 子码流的初始高度按配置给一个**合理**的猜测，真实值由统计通道覆盖。
      // 不能照抄主码流的默认 1920x1080：那会让 GetProfiles 在采集起来之前的
      // 那几秒里报"两路一样大"，而 NVR 完全可能在启动后立刻就来拉一次。
      if (i > 0) {
        int const sh = std::max(2, opt.sub_height / 2 * 2);
        int const sw = std::max(2, (1920 * sh / 1080) / 2 * 2);
        p.width = sw;
        p.height = sh;
      }
      wo.onvif.profiles.push_back(std::move(p));
    }

    // --- 多通道（第五轮 Phase AD）：宣告表与状态数组 ---------------------
    //
    // 通道 2 起各是一个**独立的 ONVIF 设备**（各占一个端口，见「已确认的取舍」），
    // 所以它们要一起出现在 WS-Discovery 的宣告表里 —— 否则 NVR 自动扫描只能
    // 扫到通道 1，其余几路得手工填地址。
    //
    // **只挂在通道 1 这一个 WebServer 上**：UDP 3702 只有一个 socket，N 个
    // responder 各自回答会让 NVR 收到 N 份互相矛盾的应答。`Wsd` 那边一次
    // Probe 回**一个报文里 N 条 ProbeMatch**（见 wsd.cpp 的 `on_datagram`）——
    // 一次单播里齐活，丢包不会只丢其中一路。
    for (d2o::ChannelPlan const& p : channel_plans) {
      if (!p.enabled) continue;   // 关掉的通道不宣告，NVR 里也就不会多一台连不上的设备
      d2o::WsdEndpoint ep;
      ep.uuid = d2o::make_uuid_urn(d2o::fmt("%s|ch%u", seed.c_str(), p.index));
      ep.http_port = cfg.web.port + static_cast<int>(p.index);
      // 类型那一半和主设备逐字相同（NVR 靠它认出"这是一台摄像机"），
      // 名字换成这一路自己的 —— 名字里那个 `#`（"显示器 #2"）必须百分号编码，
      // scope 是个 URI，原样的 `#` 会被当成 fragment 的分界符，于是 NVR 那边
      // 解析出来的名字只剩前半截。
      ep.scopes = {
          "onvif://www.onvif.org/type/video_encoder",
          "onvif://www.onvif.org/type/Network_Video_Transmitter",
          "onvif://www.onvif.org/Profile/Streaming",
          "onvif://www.onvif.org/name/" +
              scope_escape(p.name.empty() ? d2o::fmt("通道 %u", p.index) : p.name),
          "onvif://www.onvif.org/hardware/Desk2ONVIF",
          "onvif://www.onvif.org/location/desktop",
      };
      wo.wsd_extra_endpoints.push_back(std::move(ep));
    }

    // `/api/status` 的 `channels` 数组。**只有通道 1 这个 WebServer 填它** ——
    // 状态是全机一份，而通道 2 起那些端点只为 ONVIF 存在（它们连静态文件都
    // 不挂，没人在浏览器里打开）。留空的话那个字段恒为 `[]`（形状仍在，
    // `web_contract.py` 的 [4] 段字段集合相等断言因此照样是绿的）。
    wo.channels_json = [&ch_board] { return ch_board.get_json(); };

    // GET /api/audio/devices —— 配置页那个麦克风下拉框的来源。
    //
    // 两个方向**分开列**，和 `--list-audio` 是同一张表、同一个理由：
    // 「系统声音」只能从渲染端点环回，麦克风是采集端点。混成一栏的话，
    // 用户会在麦克风那里选到一个扬声器，然后得到一条永远静音的音轨
    // —— 而 SDP 和 ffprobe 看上去都正常。
    //
    // **枚举失败要说出来**（`err` 非空时带上），不能只回一个空数组：
    // 那样"没插麦克风"和"COM 起不来"在下拉框里长得一模一样。
    wo.audio_devices = []() -> std::string {
      auto group = [](d2o::AudioFlow flow) {
        nlohmann::json arr = nlohmann::json::array();
        std::string err;
        for (d2o::AudioEndpoint const& ep : d2o::list_audio_endpoints(flow, err)) {
          nlohmann::json e;
          e["id"] = ep.id;
          e["name"] = ep.name;
          e["default"] = ep.is_default;
          arr.push_back(std::move(e));
        }
        return std::make_pair(arr, err);
      };
      auto cap = group(d2o::AudioFlow::Capture);
      auto ren = group(d2o::AudioFlow::Render);
      nlohmann::json j;
      // 空的 `id` 是**有意义的**（= 系统默认端点），所以前端那个下拉框里
      // 永远要有"默认"这一项 —— 它不来自这份列表，由前端自己加。这里只报
      // 真实存在的设备。
      j["capture"] = std::move(cap.first);
      j["render"] = std::move(ren.first);
      // 只在真出错时带上这个键：`error` 恒存在（多半是空串）的话，
      // 前端得靠"空串算不算有错"来判断，而那是两边各写一次的口头约定。
      if (!cap.second.empty()) j["capture_error"] = cap.second;
      if (!ren.second.empty()) j["render_error"] = ren.second;
      return j.dump();
    };

    std::string const root = args.value("--web-root", cfg.web.static_root);
    // 目录不存在就当作"没挂静态资源"，而不是报错退出 —— Phase 5 之前
    // 根本没有 web/dist，而 API 和 WS 那时就该能用。
    std::error_code ec;
    wo.static_root = std::filesystem::is_directory(root, ec) ? root : std::string();
    if (wo.static_root.empty() && root != "web/dist") {
      std::fprintf(stderr, "--web-root %s 不是一个目录。\n", root.c_str());
      return 2;
    }

    // --- 生效值回写进 cfg（Web / ONVIF 部分）--------------------------
    // **必须在 web.start() 之前写完**：起来之后 /api/config 的回调就可能被
    // 循环线程调用，那时主线程再写 cfg 就是数据竞争。
    //
    // 回写前先留一份快照，给紧跟着那次落盘当判据 —— 判据要问的是"这一回回写
    // 到底改没改动什么"，那就得有个改动前的样子可比。`AppConfig` 是值类型，
    // 拷一份；这条路径一次启动只走一遍，代价可以忽略。
    d2o::AppConfig const cfg_before_writeback = cfg;
    cfg.web.enabled = true;
    cfg.web.host = wo.host;
    cfg.web.port = wo.port;
    cfg.web.static_root = root;
    cfg.web.auth_enabled = wo.auth_enabled;
    cfg.web.user = wo.auth_user;
    cfg.web.password = wo.auth_password;
    cfg.onvif.enabled = wo.onvif_enabled;
    cfg.onvif.user = wo.onvif.user;
    cfg.onvif.password = wo.onvif.password;
    cfg.onvif.serial_number = wo.onvif.serial_number;

    // --- 回写要落盘：**否则"首次启动固化身份与凭据"从来没真正发生过** ----
    //
    // 到上面那几行为止，改的都只是内存里的 `cfg`。整棵树里 `save_config` 原本
    // 只有两处 —— `apply_config`（运行中改配置）与 `--save-config`（一次性命令），
    // **启动这条路上一个都没有**。于是序列号、以及命令行/安装包给的那一套有效
    // 凭据，每次启动都在内存里重推一遍，磁盘上那份永远是旧的、或者根本没有。
    //
    // 行为上分不出来（每次推出来的值都一样），但"配置文件里存的是哪一份"和
    // "程序实际用的是哪一份"因此是两个不同的答案 —— 而这两个答案不一致，正是
    // 排查"我明明改了它、它却没生效"时最先要看的地方。所以让这一次回写真的
    // 落一次盘：固化是一件发生过的事，不是每次开机重演一遍的推导。
    //
    // **闸门：只在回写真的改动了什么的时候才写。** 不加这道闸门的话，每次启动
    // 都会换掉磁盘上那个文件 —— `save_config` 走的是"写 .tmp → remove → rename"，
    // 目标文件**每次都是一个新 inode**（时间、大小、inode 三个全变），于是所有
    // "这个文件没被被动过"的判据（第四轮那条防抖断言取的就是
    // `(st_mtime_ns, st_size, st_ino)` 三元组）会一起失效。
    //
    // 判据取"回写前后两份序列化是否相同"，而不是"文件在不在"：两者一致就什么
    // 都不做，于是**第二次启动起彻底安静**，一次启动最多写一次。用
    // `redact_secrets=false` 是有意的 —— 默认那个会把口令换成哨兵，于是"这次
    // 只改了口令"会看起来毫无变化，而那恰好是这一整套修复要固化下来的东西之一。
    //
    // 一个必然的后果，写在这里免得后面被当成新缺陷：**裸形态下 `config.json`
    // 从"第一次改配置才出现"变成"第一次启动就出现"**（陷阱速查表里那条记的就是
    // 旧行为）。这是刻意的 —— 一份从没落过盘的配置，"恢复出厂"和"手动编辑文件"
    // 这两条逃生路都够不着它。第四轮那条防抖断言取的是三元组、文件不在时**当场
    // 报错**（陷阱 91），所以它在新行为下照样成立，而且不再依赖"文件恰好还在"。
    if (d2o::config_to_json(cfg_before_writeback, /*redact_secrets=*/false) !=
        d2o::config_to_json(cfg, /*redact_secrets=*/false)) {
      std::string serr;
      if (d2o::save_config(cfg, config_path, serr)) {
        D2O_INFO("配置已固化：%s（启动时的生效值和文件里原有的那份不同，写了一次）",
                 config_path.c_str());
      } else {
        // **不当作致命错误**：这一次跑得下去，只是下次启动还要重新推一遍。
        // 但要说出来 —— 静默失败会让"改了没生效"那一类问题原地保留，而且
        // 同一路上 `apply_config` 的存盘会以同样的理由失败，两条其实是一件事。
        D2O_WARN("生效值没能写进 %s：%s（本次运行不受影响；但配置改动同样存不下来）",
                 config_path.c_str(), serr.c_str());
      }
    }

    // 配置读写桥。捕获引用是安全的，但**前提是那几个变量都声明在这个块之外**
    // （见函数开头那段）—— `cfg` / `config_path` 本来就是，`running_cfg` /
    // `cfg_pending` / `derived_serial` 也必须在。
    //
    // 这几个 lambda 只在循环线程上跑，那段时间里主线程正阻塞在 session.run()
    // 里，所以不存在数据竞争；它们要的是"对象还活着"，不是任何锁。
    //
    // **运行中那一侧的配置快照。** 它代表"服务真的在用哪一份"。每次保存时
    // 把能热生效的那部分同步进来，剩下的差异就是 pending —— 于是"哪些字段是
    // 热的"这件事不需要一张手写的清单，而是由"同步了哪几个"直接决定。
    // 手写清单迟早在加字段时漏掉一项，而漏掉的表现正是这个功能要防的：
    // 某个字段改了却不在 pending 里，界面上于是显示"已生效"。
    //
    // 取值放在这里而不是声明处：它必须反映**下面这一整段回写之后**的 cfg，
    // 也就是服务真正用的那份。
    running_cfg = cfg;

    // `compute_pending` 定义在**上面**（`if (http)` 之外），理由见那里的注释：
    // 它的地址被 `apply_config` 按引用捕获，而那个闭包比这个块活得久。

    wo.config_get = [&cfg]() { return d2o::config_to_json(cfg); };
    wo.config_pending = compute_pending;

    // `POST /api/reload`：只置一个原子位，真正的重开发生在采集线程上。
    //
    // **返回值是"此刻有没有管线在跑"**，不是成功与否 —— 这个操作没有失败那条
    // 路（没在录的时候请求本来就是空操作，下一段自然用新配置），详见
    // `WebOptions::reload` 的注释。
    //
    // 无论 live 与否都照置那个位：分段循环每一趟都在顶部重算选项、而 `run()`
    // 一开始就把这位清掉，所以"没在跑的时候置了它"是彻底的 no-op，
    // 反而比在这里分个支更不容易写错。
    wo.reload = [&session, &session_running]() {
      bool const live = session_running.load(std::memory_order_relaxed);
      session.request_reconfigure();
      return live;
    };

    // `POST /api/restart` / `POST /api/factory-reset`：**回调里只置位**。
    //
    // 它们跑在 web 循环线程上（在 202 那个响应入队之后的同一拍里），而"停采集、
    // 收尾、重启/恢复出厂"是主线程那条分段循环的活。和 `reload` 同一条纪律：
    // 慢动作和会让进程消失的动作都不在 handler 里做。
    //
    // `request_stop()` 是为了让**正在录的这一段当场收尾**：不叫停的话，点了重启
    // 要一直等到 `--seconds` 用完才会响应，而长驻形态下它干脆永远不会自己停。
    //
    // **但光有它不够，分段循环还必须自己查那个位。** `run()` 的第一条语句就是
    // `stop_.store(false)`（capture_session.cpp），所以一个恰好落在"上一段跑完、
    // 下一段还没进 run()"这个窗口里的 `request_stop()` 会被下一趟**就地清掉**。
    // 两边都要查，见下面循环顶部那两句。
    wo.restart = [&session, &restart_requested]() {
      restart_requested.store(true, std::memory_order_relaxed);
      session.request_stop();
    };
    // **恢复出厂多两个参数**（来源 + 身份），`restart` 一个都不要。
    //
    // 差别来自"这件事会不会把自己写的那行日志删掉"：`restart` 那条审计行写在
    // web 层、之后没人动它，所以 web 层知道来源就够了；而恢复出厂在收尾时会
    // `audit->clear()` —— web 层写的那一行**会被自己删掉**，于是"谁干的"必须
    // 先递到主线程手上，由清完之后那一行（见 `factory_reset_files` 第 5 步）
    // 重新报出来。这两个参数就是那条通道，没有别的走法。
    //
    // 签名从这里开始与 `WebOptions::factory_reset` 对齐 —— 它是本轮唯一改动的
    // 回调签名（`config_set` / `on_patch` 一个都没动，见计划里那条纪律）。
    wo.factory_reset = [&session, &restart_requested, &factory_reset_requested,
                        &who_mu, &factory_reset_who](std::string const& source,
                                                     std::string const& identity) {
      {
        std::lock_guard<std::mutex> lk(who_mu);
        factory_reset_who.source = source;
        factory_reset_who.identity = identity;
      }
      factory_reset_requested.store(true, std::memory_order_relaxed);
      // 恢复出厂包含一次重启：置这一个位，收尾那一支会把两件事都做了。
      restart_requested.store(true, std::memory_order_relaxed);
      session.request_stop();
    };

    // ---- 时间设置（Phase U）-------------------------------------------------
    //
    // 四条路的分工：
    //
    //   `time_get`   —— 只读。把 `DeviceState`（设备**此刻**在用什么）与
    //                   `NtpSyncer::Status`（对时线程在干什么）合成一份 JSON。
    //                   配置那一半（模式 / 服务器 / 间隔）从 `cfg` 读。
    //   `time_set`   —— 手动校准。**走 `apply_config`**：落盘、推运行侧、
    //                   重算 pending 三段都不必在这里重写一遍（写两遍的下场
    //                   是其中一条漏了某一段，而症状是"网页上改了没进 pending"）。
    //   `time_reset` —— 归零。同样走 `apply_config`，但**多一步**，见下。
    //   `time_sync`  —— 立刻对一次。**只置位**，与 `restart` 同一条纪律：
    //                   UDP 查询最长两秒（还可能乘三个地址），在循环线程上等它
    //                   等于把 WS 推送和 SOAP 一起堵两秒。
    //
    // 四条都在 Web 登录之后（`/api/*` 本来就在），不需要自己再做鉴权。
    //
    // **手动那两条在 NTP 模式下拒绝执行**，并把原因说出来。这不是保守：NTP
    // 模式下的钟归对时线程管，它下一个周期就会把手动设的值覆盖掉 —— 用户看到
    // 的是"设成功了、一小时后自己变回去了"，而这类"改了没反应"正是本项目最
    // 不愿意留下的形状。拒绝并让他先切到手动模式，是唯一说得圆的做法。
    // （NVR 的 `SetSystemDateAndTime` 不受这条限制 —— 它那条路的语义就是
    // "录像机现在要求对齐"，由 NVR 自己承担后果，见 onvif_service.cpp。）

    wo.time_get = [&cfg, &device_state, &ntp_syncer]() {
      auto const to_s = [](std::chrono::system_clock::time_point t) {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(
                t.time_since_epoch())
                .count());
      };
      auto const to_ms = [](std::chrono::system_clock::time_point t) {
        return static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                t.time_since_epoch())
                .count());
      };
      d2o::NtpSyncer::Status const st = ntp_syncer.status();
      // **主机时钟只取一次**，下面那几个由它推出来。取两次（哪怕只隔几十微秒）
      // 会让"这两个数说的是不是同一个时刻"变成一个只能靠运气的问题，而前端
      // 那个会走的钟正是按"它们说的是同一个时刻"推的。
      auto const host = std::chrono::system_clock::now();
      auto const offset = device_state.time_offset().count();
      nlohmann::json j;
      j["host_now"] = to_s(host);
      // 亚秒。**这一位是给网页上那个"会走的设备时间"用的**，不是给人读的。
      //
      // 没有它的话：`host_now` 是**整秒**（截断），前端只能拿它当基准、再按本地
      // 经过的时间往上加，而截断掉的那部分会变成一个**恒定的相位误差** ——
      // 显示的秒有大约一半的时间停在前一秒上，看着就像"设备时间慢了 1 秒"。
      // 而这一段要回答的恰恰就是"设备时间对准了没有"（用户原话：要能看出来
      // "时间是和其他时间准确对准了"），一个 ±1 秒的显示把答案变成了猜。
      // 有了毫秒这一位，剩下的误差只有网络单程延迟（本机是 1 ms 量级）。
      //
      // 与 `host_now` **同一时刻**（上面那一次 `now()`），所以
      // `host_now_ms / 1000 == host_now` 恒成立 —— 一个测量、两种单位，
      // 不是"两个真相"。
      j["host_now_ms"] = to_ms(host);
      // 设备时间由**同一个采样点**加偏移推出来，而不是再读一次
      // `device_state.now()`。这样 `device_now - host_now == offset_s` 是结构上
      // 成立的（前端那个会走的钟正是按这个关系推设备时间的），也不会出现
      // "读了偏移 A 得到一个时间、又报了偏移 B"那种自相矛盾的一帧。
      j["device_now"] = to_s(host) + offset;
      // 偏移**从 `DeviceState` 读**，不是从 `cfg.time.offset_s`：NTP 模式下
      // 这两个数**本来就该不一样**（NTP 得来的偏移不进配置，理由见
      // ntp_syncer.h 的文件头）。界面上要显示的是"设备此刻差多少"，那就得问
      // 设备状态；配置里那个数另给一个字段，让"存的那个"和"在用的那个"在
      // 界面上分得开 —— 这正是本项目对"两个真相"的一贯处理。
      j["offset_s"] = offset;
      j["synced"] = device_state.time_synced();
      j["synced_at"] = to_s(device_state.synced_at());
      j["mode"] = cfg.time.mode;
      j["ntp_server"] = cfg.time.ntp_server;
      j["ntp_interval_s"] = cfg.time.ntp_interval_s;
      j["configured_offset_s"] = cfg.time.offset_s;
      j["last_sync_at"] = st.last_at_s;
      j["last_sync_state"] = st.last_state;
      j["last_sync_error"] = st.last_error;
      j["last_sync_offset_s"] = st.last_offset_s;
      j["last_sync_stratum"] = st.last_stratum;
      j["syncing"] = st.syncing;
      j["sync_ok_count"] = st.ok_count;
      j["sync_fail_count"] = st.fail_count;
      return j.dump();
    };

    wo.time_set = [&cfg, &apply_config, &device_state](std::string const& body,
                                                       std::string& cerr) {
      if (cfg.time.mode == "ntp") {
        cerr = "当前是 NTP 自动对时模式，手动校准会被下一次对时覆盖。"
               "先在时间设置里把模式改成「手动」，再校准。";
        return false;
      }
      nlohmann::json j;
      try {
        j = nlohmann::json::parse(body);
      } catch (std::exception const& e) {
        cerr = d2o::fmt("请求体不是合法 JSON（%s）。", e.what());
        return false;
      }
      if (!j.is_object()) {
        cerr = "请求体应当是一个 JSON 对象，形如 {\"offset_s\":-3600}。";
        return false;
      }

      // 两种给法，二选一。**两个都给时以 `offset_s` 为准**，并在下面把这件事
      // 说明白 —— 静默挑一个的话，前端哪天两个都发就会得到一个解释不了的结果。
      bool const has_offset = j.contains("offset_s") && j["offset_s"].is_number();
      bool const has_abs = j.contains("device_now") && j["device_now"].is_string();
      if (!has_offset && !has_abs) {
        cerr = "要给出 offset_s（相对主机时间的秒数，可负）或者 "
               "device_now（\"2026-10-08T15:30:00\"，按本机时区解析）。";
        return false;
      }

      std::int64_t offset = 0;
      if (has_offset) {
        offset = j["offset_s"].get<std::int64_t>();
      } else {
        std::time_t when = 0;
        std::string const s = j["device_now"].get<std::string>();
        if (!parse_local_datetime(s, when)) {
          cerr = d2o::fmt("看不懂这个时间：「%s」。要写成 2026-10-08T15:30:00 "
                          "这种形状（日期与时刻之间用 T 或空格）。",
                          s.c_str());
          return false;
        }
        // 绝对时刻 → 偏移。用**主机此刻**当基准：`device_now` 是用户要的
        // 时间点，而偏移的定义就是"它减主机时间"，两者之间隔着的这几微秒
        // 会被量化进整秒里，无妨（设备时间本来就是整秒）。
        offset = static_cast<std::int64_t>(when) -
                 static_cast<std::int64_t>(std::time(nullptr));
      }
      // ±365 天。与 `validate_config` 的夹取范围一致 —— 两处不一致的话，
      // 一个刚好在边界外的值会在这里被接受、在 `apply_config` 里被悄悄夹掉，
      // 而用户看到的是"设成功了"却差了一年。
      constexpr std::int64_t kMaxOffsetS = 365LL * 86400;
      if (offset < -kMaxOffsetS || offset > kMaxOffsetS) {
        cerr = d2o::fmt("偏移 %lld 秒超出 ±365 天的范围。",
                        static_cast<long long>(offset));
        return false;
      }

      d2o::AppConfig next = cfg;
      next.time.offset_s = static_cast<int>(offset);
      d2o::ConfigApplyResult res;
      if (!apply_config(std::move(next), res, cerr)) return false;

      // **再显式写一次 `DeviceState`。** `apply_config` 里那一步是"值变了才
      // 写"（它防的是改码率时把 synced_at 一起刷新），而这里是用户**明确要求
      // 把设备时间设成这样**，哪怕新值和配置里存的恰好相同也得落定 —— 否则
      // 会出现这一串：NTP 把设备挪到 +5（配置里仍是 0）→ 用户切到手动、
      // 填 0 保存 → 配置没变、`DeviceState` 没被碰 → 设备时间还是 +5，而
      // 界面上写着 0。这正是"改了没反应"最标准的一种。
      device_state.set_device_time(std::chrono::system_clock::now() +
                                   std::chrono::seconds{offset});
      D2O_INFO("设备时间已手动校准：相对主机偏移 %+lld 秒。",
               static_cast<long long>(offset));
      return true;
    };

    wo.time_reset = [&cfg, &apply_config, &device_state](std::string& cerr) {
      if (cfg.time.mode == "ntp") {
        cerr = "当前是 NTP 自动对时模式，归零会被下一次对时覆盖。"
               "先在时间设置里把模式改成「手动」。";
        return false;
      }
      d2o::AppConfig next = cfg;
      next.time.offset_s = 0;
      d2o::ConfigApplyResult res;
      if (!apply_config(std::move(next), res, cerr)) return false;
      // **显式复位，不能只靠上面那句赋值。** 偏移完全可以是 0 而 `synced_`
      // 仍然是 true（NVR 对过一次恰好对齐的时、或者手动设了一个 0 偏移），
      // 那时 `apply_config` 的"值变了才写"一步都不会走，于是状态页上会一直
      // 挂着"已被同步过（设备时间与主机相差 0 秒）"—— 一句语法正确、但用户
      // 刚刚明确要求撤销的话。`reset_time()` 的第二个调用者就在这里
      // （第一个是 Phase R 的恢复出厂）。
      device_state.reset_time();
      D2O_INFO("设备时间已恢复成主机时间（偏移归零）。");
      return true;
    };

    wo.time_sync = [&ntp_syncer]() { ntp_syncer.request_now(); };

    // `apply_config` 定义在**上面**（`if (http)` 之外），理由见那里的注释：
    // 它的地址被下面三个 Web 回调按引用捕获，而它们比这个块活得久。

    wo.config_set = [&cfg, &apply_config](std::string const& body,
                                          d2o::ConfigApplyResult& out,
                                          std::string& cerr) {
      // 以**当前生效值**为底，不是以默认值为底：口令的哨兵（"__unchanged__"）
      // 靠的就是"out 里已经存着真口令"这件事，从默认值起手会把口令清成空串
      // —— 而空口令在这个服务里是合法的，于是它会静默地"改成无口令"。
      d2o::AppConfig next = cfg;
      if (!d2o::config_from_json(body, next, cerr)) return false;
      return apply_config(std::move(next), out, cerr);
    };

    // ---- 操作日志（Phase V）：写入点与读取接口 -----------------------------

    // **同一个 lambda 装到两处。** `WebOptions::audit` 与
    // `OnvifServiceOptions::audit` 是一字不差的同一个签名，落点也是同一个
    // `AuditLog` —— 它们的区别只在「谁调」：网页那侧知道来源 IP 与登录用户，
    // ONVIF 那侧知道 NVR 的 IP 与 Digest 用户名。读日志的时候不该需要知道
    // 这一行是网页写的还是 NVR 写的（`who.source` 里那个前缀已经说明了）。
    //
    // 回调只做一件事：写一行 + fflush 一次。它跑在 **web 循环线程**上
    // （ONVIF 那侧就在 SOAP handler 里），而那条线程同时管着 WS 推送、
    // 静态文件与其它请求 —— 挡着它的时间要尽可能短，绝不能在这里做网络、
    // 磁盘扫描或者任何"顺便"的事。
    auto const audit_note = [&audit](d2o::AuditWho const& who,
                                     std::string const& action,
                                     std::string const& detail) {
      audit.note(who, action, detail);
    };
    wo.audit = audit_note;
    wo.onvif.audit = audit_note;

    // `GET /api/audit?days=N&limit=M&q=关键字`。
    //
    // 回**完整的 JSON 文本**而不是 `std::vector<AuditEntry>`：与
    // `audio_devices` 同一个理由 —— Web 层于是完全不需要认识 `AuditLog`
    // （连它的头文件都只是被签名带上的），而「按天分组、倒序、关键字过滤、
    // 留存策略那句话」这些**呈现**上的决定全留在这一处。多一次拷贝，
    // 换掉一个会两头漂的结构。
    wo.audit_read = [&audit](int days, int limit, std::string const& keyword) {
      // 上下界在这里夹一次、并且**报的是夹过之后的值**：`read()` 内部也夹，
      // 但它不回报。界面按回应里这两个数去画分页，报原值的话它会以为
      // 「我请求了 5000 天」而实际只读了 400 天 —— 少掉的那截看起来像
      // 「那天没有记录」。
      int const d = days < 1 ? 1 : (days > 400 ? 400 : days);
      int const n = limit < 1 ? 1 : (limit > 5000 ? 5000 : limit);

      std::string aerr;
      std::vector<d2o::AuditEntry> const rows = audit.read(d, n, keyword, aerr);
      d2o::AuditStats const st = audit.stats();

      nlohmann::json arr = nlohmann::json::array();
      for (d2o::AuditEntry const& e : rows) {
        nlohmann::json o;
        o["day"] = e.day;
        o["time"] = e.time;
        o["source"] = e.source;
        o["identity"] = e.identity;
        o["action"] = e.action;
        o["detail"] = e.detail;
        // 整行原文单独给一份：界面上「复制一行」要的是它，而不是把字段拼
        // 回去的那份 —— 拼回去的那份和磁盘上的行不是同一串字节，而审计
        // 的全部价值就在于「贴出去的这一行确实是日志里那一行」。
        o["raw"] = e.raw;
        arr.push_back(std::move(o));
      }

      nlohmann::json j;
      // `ok` 说的是**读到了没有**，不是「有没有记录」。一个刚恢复出厂、
      // 审计文件一个都不在的机器必须回 `ok:true` + 空数组 —— 否则界面上
      // 分不出「这台设备还没发生过任何事」和「日志读不出来」，而这两件事
      // 该给的提示完全不同（后者要让人去查目录权限）。
      j["ok"] = aerr.empty();
      if (!aerr.empty()) j["error"] = aerr;
      j["enabled"] = st.enabled;
      j["keep_days"] = st.keep_days;
      j["dir"] = st.dir;
      j["files"] = st.files;
      j["bytes"] = st.bytes;
      j["days"] = d;
      j["limit"] = n;
      j["count"] = static_cast<int>(arr.size());
      j["entries"] = std::move(arr);
      return j.dump();
    };

    // NVR 通过 `SetVideoEncoderConfiguration` / `SetVideoSourceConfiguration`
    // 改设备配置时的落点。**和网页上的保存走同一条 `apply_config`** ——
    // 否则"NVR 改的字段没进 pending"会是一条只在 NVR 上才发作的路。
    //
    // **返回 void 是接口定的**（见 `OnvifServiceOptions::on_patch`）：落盘
    // 失败也不回 SOAP Fault，否则 NVR 的通道添加流程会当场失败。失败只剩下
    // 日志这一个出口，所以这里必须打 ERROR。
    wo.onvif.on_patch = [&cfg, &apply_config](d2o::OnvifDevicePatch const& p) {
      d2o::AppConfig next = cfg;
      // 档位管着 fps / bitrate_kbps / max_height / sub_enabled 四项，
      // `validate_config` 会把它们按档位表**重写**一遍。所以只要动了其中
      // 任何一项就必须同时翻成 `custom`，否则这次改动会在落盘前被"展开"掉
      // —— 而现象是"NVR 改完，GetProfiles 还是老样子"，本地怎么试都对。
      bool tier_field = false;
      if (p.name) next.onvif.device_name = *p.name;
      if (p.resolution_height) {
        next.capture.max_height = *p.resolution_height;
        tier_field = true;
      }
      if (p.fps) {
        next.capture.fps = *p.fps;
        tier_field = true;
      }
      if (p.bitrate_kbps) {
        next.capture.bitrate_kbps = *p.bitrate_kbps;
        tier_field = true;
      }
      // **延时档位是同一个形状的第二组档位表。** `capture.latency` 那张表管着
      // `threads` 与 `gop_seconds` 两项，`validate_config` 同样会把它们
      // **无条件重写**回档位值（`config.cpp` 里那段 `snap`）。NVR 能设的只有
      // `gop_seconds`（GovLength）这一项，所以动了它就必须把 latency 也翻成
      // `custom` —— 否则 NVR 设的「每 3 秒一个 I 帧」会在**同一次**
      // `apply_config` 里被「均衡」档改回 2 秒，而 SOAP 那边照样回成功。
      // 症状与上面那一段一字不差：NVR 改完毫无变化。
      //
      // ⚠️ 这一条是**量出来的**：`onvif_soap.py` 的 `GovLength 36 帧 ÷ 12 fps
      // = 3 秒` 那条断言，在延时档位进来之前一直是绿的。档位表一上线，它红了
      // —— 而 `onvif_service.cpp` 那边的换算（36/12=3）一个字节都没错，
      // 把 3 改回 2 的是下游的 `validate_config`。见陷阱速查表那一行。
      bool latency_field = false;
      if (p.gop_seconds) {
        next.capture.gop_seconds = *p.gop_seconds;
        latency_field = true;
      }
      if (p.h264_profile) next.capture.profile = *p.h264_profile;
      // 子码流的这两项**不在档位表里**（档位只管 sub_enabled 这个开关），
      // 所以改它们不必翻成 custom —— 翻了会让界面上那个档位标签无缘无故
      // 变成"自定义"，而值其实一点没离开档位。
      if (p.sub_height) next.rtsp.sub_height = *p.sub_height;
      if (p.sub_bitrate_kbps) next.rtsp.sub_bitrate_kbps = *p.sub_bitrate_kbps;
      if (tier_field) next.capture.quality = "custom";
      if (latency_field) next.capture.latency = "custom";

      d2o::ConfigApplyResult res;
      std::string cerr;
      if (!apply_config(std::move(next), res, cerr)) {
        D2O_ERROR("NVR 改设备配置没能存下来（%s）。内存里的设备状态照旧，"
                  "但这一次改动重启后会丢。",
                  cerr.c_str());
        return;
      }
      // 只有**名称**是当场生效的：它走 `DeviceState`，采集线程每帧现读，
      // 水印下一帧就是新名字。几何字段如实进 pending 并打出来 —— 那是
      // "为什么 NVR 上看不出变化"的唯一线索（NVR 那边只看得到成功）。
      if (p.name) D2O_INFO("NVR 把摄像头名称改成了「%s」，已存盘并当场生效。",
                           p.name->c_str());
      std::vector<std::string> const& pend = res.pending;
      if (!pend.empty()) {
        std::string joined;
        for (std::string const& f : pend) {
          if (!joined.empty()) joined += ", ";
          joined += f;
        }
        D2O_WARN("NVR 改的这几项要重启才生效，已在待重启列表里：%s", joined.c_str());
      }
    };

    // NVR 通过 `SetOSD` / `CreateOSD` / `DeleteOSD` 改水印时的落点。
    // **和网页上的保存走同一条 `apply_config`** —— 理由和 `on_patch` 那段
    // 一样：两条路各写一遍的话，"NVR 改的字段没进 pending"会变成一条只在
    // NVR 上才发作的缺陷。
    //
    // 结构和 `on_patch` 的唯一区别：那边收的是一组**可选字段**（patch），这边
    // 收的是**整份** `OsdConfig`。水印是一棵纯数据的树，没有"部分应用"这
    // 回事；给 patch 只会让"把改动合并进去"这段逻辑有两个副本。
    //
    // 落盘失败同样不回 SOAP Fault（接口定的，见 `OnvifServiceOptions::on_patch`），
    // 所以这里必须打 ERROR —— 日志是唯一的出口。
    wo.onvif.on_osd_patch = [&cfg, &apply_config](d2o::OsdConfig const& c) {
      // **防抖**：NVR 会周期性地把同一份 OSD 重复下发（`SetOSD` 的语义就是
      // "设成这样"，它对"已经这样了"和"刚改成这样"不作区分）。不去重的话
      // 每一次都会写一次盘、打一行日志，而内容一个字都没变 —— 日志于是被
      // 淹没，"这次到底改了什么"再也看不出来。
      //
      // 判据用 `osd_to_json` 的文本比较，而不是逐字段手写：那样写的话字段集
      // 迟早和 `config_to_json` 的 osd 子树漂移（陷阱 52 的同一个形状），
      // 而漂移的症状是"某一项改了却被当成没变、于是被吞掉"。两边共用
      // `osd_json_node()`，不可能漏。
      //
      // 读 `cfg` 在**循环线程**上是安全的：`cfg` 只有这条线程写（`apply_config`
      // 里那一句整体赋值），而这个回调就在同一条线程上。
      if (d2o::osd_to_json(c) == d2o::osd_to_json(cfg.osd)) return;

      d2o::AppConfig next = cfg;
      next.osd = c;
      d2o::ConfigApplyResult res;
      std::string cerr;
      if (!apply_config(std::move(next), res, cerr)) {
        D2O_ERROR("NVR 改的水印没能存下来（%s）。SOAP 那边照旧报成功，"
                  "重启之后这次改动会丢。",
                  cerr.c_str());
        return;
      }
      // **不逐项打 pending。** `on_patch` 那边打是因为它改的字段里有的能当场
      // 生效、有的不能，哪个是哪个需要说清楚；而水印的每一项都是热的，逐项
      // 打出来只是一行恒真的噪音。
      //
      // 这句话的时态在本轮改了：从前写的是"画面上要重启才变"，而那时它是对的
      // （`apply_osd_config` 恒返回 false）。留着就变成误导 —— 下一个人会按它
      // 去解释一个不存在的延迟。
      D2O_INFO("NVR 改了水印（现在 %zu 项），已存盘并**当场生效**。",
               c.items.size());
    };

    // NVR 问"你的钟由谁维护"（`GetSystemDateAndTime` 的 `DateTimeType`），以及
    // NVR 说"按 NTP 对时"（`SetSystemDateAndTime` 的 `DateTimeType=NTP`）。
    //
    // **这一条必须是回调，不能把 `cfg.time.mode` 的值拷进去。** 模式在运行中会从
    // 网页上改，而 `OnvifServiceOptions` 只在启动时被灌一次 —— 拷值的话 NVR 那边
    // 永远看到启动那一刻的模式，而"网页上写着 NTP、NVR 上看到 Manual"这种不一致
    // 两边各自都有出处，谁都不会去怀疑。
    //
    // 读 `cfg` 在**循环线程**上是安全的：`cfg` 只有循环线程写（`apply_config` 里
    // 那一句整体赋值），而这个回调就在同一条线程上被调（SOAP 处理器）。
    wo.onvif.time_mode = [&cfg]() { return cfg.time.mode; };

    // NVR 要求按 NTP 对时时做的事。**这个回调在 SOAP 响应发出去之后才跑**
    // （见 `OnvifServiceOptions::on_ntp_request`），所以这里可以安心调
    // `request_now()` —— 它只置一个位 + 一次 `notify`，阻塞的 UDP 查询在
    // NtpSyncer 自己的线程上。
    //
    // 模式的判断放在**这里**而不是 ONVIF 那一层：ONVIF 只该知道"NVR 要我对时"，
    // 不该知道这台设备的时间是怎么维护的 —— 那套东西全在 `cfg.time` 与
    // `ntp_syncer` 里。
    wo.onvif.on_ntp_request = [&cfg, &ntp_syncer]() {
      if (cfg.time.mode != "ntp") {
        // 手动模式下**连查都不查**（而不是"查了但不采用"）：手动模式下的偏移是
        // 用户自己校准出来的，一次查询若顺手把它改掉，配置里那个 `offset_s`
        // 立刻就对不上，而下次重启又弹回去。既然结果注定不会被采用，花掉那两秒
        // 就只是白等。这一句也是"NVR 那边设了 NTP、设备却没动静"的唯一解释。
        D2O_INFO("NVR 要求按 NTP 对时，但本设备当前是「手动」时间模式：不发起对时。"
                 "要让它跟着 NTP 走，请在时间设置里把模式改成「NTP」。");
        return;
      }
      D2O_INFO("NVR 要求按 NTP 对时，正在向 %s 发起一次对时查询。",
               cfg.time.ntp_server.c_str());
      ntp_syncer.request_now();
    };

    // NVR 用 `SetSystemDateAndTime` 把设备时间设成某个具体时刻之后，**把这个
    // 偏移落盘**。
    //
    // "时钟已经走起来了"那件事 ONVIF 那一层自己做完了（`device->set_device_time`
    // → 水印下一帧就变，`GetSystemDateAndTime` 也立刻报新值），这里只回答**另
    // 一个问题：这个偏移要不要留下来**。两件事分成两个落点的理由写在
    // `OnvifServiceOptions::on_device_time_set` 的注释里。
    //
    // **为什么必须留下来**：网页上那条手动校准的路是落盘的（`cfg.time.offset_s`），
    // 理由写在 `TimeConfig` 的注释里 ——「手动校准出来的偏移重启后要还在，不能
    // 指望它自己再对一次」。NVR 设的这一份是**同一类事实**：它也是"钟被定在了
    // 这里"，也不会自己再对一次（除非那台 NVR 下次重连时重发）。不落盘的话重启
    // 之后设备时间弹回主机时间，而**一段连续录像于是在重启前后带着两个时间
    // 基准** —— 水印上的时间正是取证时要和录像机对齐的那一项。
    //
    // **按模式和网页那条路对齐：只有「手动」模式写盘。** NTP 模式下这个偏移下
    // 一个对时周期就会被覆盖（`POST /api/time` 在 NTP 模式下干脆是拒绝的，见
    // `time_set`），落盘只会让 `cfg.time.offset_s` 变成一个用户从没设过、也不会
    // 被采用的数 —— 而 `/api/time` 里的 `configured_offset_s` 报的正是它。
    wo.onvif.on_device_time_set = [&cfg, &apply_config](int offset_s) {
      if (cfg.time.mode != "manual") {
        D2O_INFO("NVR 把设备时间设了 %+d 秒，但当前是 NTP 模式：不写进配置"
                 "（下一个对时周期就会覆盖它）。设备时钟本身已经跟着变了。",
                 offset_s);
        return;
      }
      // **防抖**，和 `on_osd_patch` 那条同一个形状，但这里省下的不是日志而是
      // **一次写盘**：NVR 会周期性地重发同一时刻附近的时间，算出来的偏移多数
      // 时候是同一个整数。`apply_config` 里的 `save_config` 是**无条件**写的
      // （它在任何 diff 之前），所以这一句比较是唯一的闸门。
      if (cfg.time.offset_s == offset_s) return;

      d2o::AppConfig next = cfg;
      next.time.offset_s = offset_s;
      d2o::ConfigApplyResult res;
      std::string cerr;
      if (!apply_config(std::move(next), res, cerr)) {
        // 落盘失败：**不回 SOAP Fault**（接口定的，见 `on_patch` 那段），日志
        // 是唯一的出口。这时钟已经改了，所以这句话只说"下次重启会丢"。
        D2O_ERROR("NVR 设的设备时间偏移 %+d 秒没能写进配置（%s）——"
                  "设备时钟已经跟着变了，但重启之后这次改动会丢。",
                  offset_s, cerr.c_str());
        return;
      }
      D2O_INFO("NVR 设的设备时间偏移 %+d 秒已写进配置（重启后仍然有效）。",
               offset_s);
    };

    // 快照取帧。`session` 在上面就构造好了，这里只是按引用捕获它。
    //
    // **按"现在看哪一路"分派**，不是恒取通道 1：界面上那一排通道按钮切过去
    // 之后，按快门存下来的必须是屏幕上正在看的那一路 —— 恒取通道 1 的话，
    // 用户拿到的是一张"别的摄像头的照片"，而且不会有任何报错。
    wo.snapshot_source = [&session, &preview_hub]() {
      int const n = preview_hub.active.load(std::memory_order_relaxed);
      // 通道 1 的会话不在表里（它是上面那个局部变量），所以 `n == 1` 直接走它。
      if (n != 1) {
        if (auto s = preview_hub.session_of(n)) return s->snapshot();
      }
      return session.snapshot();
    };

    // 本机有什么可以当通道 —— 配置页的「通道」那一段要。
    //
    // **显示输出那份是启动时算好的**（`displays` 就在上面），这里只补摄像头：
    // 枚举摄像头走 Media Foundation，要 `MFStartup`，几十毫秒起步，而
    // `--no-http` 的那些跑法（录制、只推流）根本没人问这件事 —— 所以它放在
    // `if (http)` 里面。
    //
    // 枚举失败**不拦启动**：`/api/sources` 回一份带 `camera_error` 的空表，
    // 配置页显示"这台机器上枚举不出摄像头"并能照常加显示器通道。让整个进程
    // 因为"网页上看不到摄像头清单"而起不来是荒谬的。
    {
      std::string cam_err;
      std::vector<d2o::CameraInfo> const cams = d2o::enumerate_cameras(&cam_err);
      if (!cam_err.empty()) {
        D2O_WARN("枚举摄像头失败（%s）—— 配置页的通道那一段列不出摄像头，"
                 "显示器通道不受影响。",
                 cam_err.c_str());
      }
      std::string const sources = build_sources_json(displays, cams, cam_err);
      wo.sources_json = [sources] { return sources; };
    }

    // 预览切到哪一路。**校验在这里**（哪几路真的存在是运行期的事，
    // Web 层手里只有一份字符串），成功之后**重放**那一路的初始化分段和 0 点。
    //
    // 重放是必需的，不是优化：一次采集只发一次 ftyp+moov，而 WebServer 只留
    // 它最近看到的那一份。不重放的话，切过去之后浏览器拿到的是新那路的分片
    // 配旧那路的 moov —— 它不报错，只是一直黑着。详见 `PreviewHub` 的注释。
    //
    // 重放**顺带广播给还连着的客户端**（`publish_preview` 就是这个语义），
    // 而前端的切换顺序是"先关旧 WS、再 POST、再开新 WS"，所以那时候通常一个
    // 都不剩；剩下一两个也正在被自己关掉。不额外做"清场"：那要么给
    // `WebServer` 加一个只此一处用的方法，要么把一段即将被丢掉的连接
    // 当成需要照顾的对象。
    wo.preview_channel_get = [&preview_hub]() {
      return preview_hub.active.load(std::memory_order_relaxed);
    };
    wo.preview_channel_set = [&preview_hub, &web](int n) {
      if (!preview_hub.has(n)) return false;
      preview_hub.active.store(n, std::memory_order_relaxed);
      if (auto bytes = preview_hub.init_of(n)) {
        web.publish_preview(true, std::move(bytes));
      }
      web.publish_preview_t0(preview_hub.t0_of(n));
      D2O_INFO("网页预览切到通道 %d。", n);
      return true;
    };

    // --- 起 Web 服务：**被重启拉起来的那个进程要等端口** --------------------
    //
    // 自重启的空窗里，上一个进程刚退，而它留下的连接还在 TIME_WAIT 里（Windows
    // 默认 120 秒）。监听套接字自己关闭不进 TIME_WAIT，但同一个端口上刚落下的
    // 连接会占着它，此时 `bind` 直接失败；而 libuvcpp 的监听套接字**没有**
    // SO_REUSEADDR（库源码里找不到任何一处设它 —— Live555 那侧才有）。所以
    // 猜一个固定 sleep 是靠不住的：改成有界重试，等到就能起，等不到就把最后一次
    // 的真实错误报出来。
    //
    // **只在 `--handoff` 时重试。** 它是"上一个实例刚把端口让出来"的标志
    // （现在有两个生产者：`packaging\start.ps1` 那条路和下面 `restart_process()`），
    // 而重试对别的失败是纯粹的浪费：`--web-root` 指错目录、端口被一个无关程序
    // 长期占着 —— 这两种等五秒也不会变，却会让**每一次**启动都多等五秒。
    //
    // `web.start()` 失败之后重入是安全的：它在失败分支里做了 `d.app.reset()`，
    // 下一次进来会新建一个 uvcpp_web_app 并重新注册路由（`register_routes` 只往
    // 那个新 app 里注册，没有别的副作用）。
    bool started = web.start(wo, err);
    if (!started && args.has("--handoff")) {
      int const kRetries = 25;        // 25 × 200 ms = 5 秒
      for (int i = 0; i < kRetries && !started; ++i) {
        if (i == 0) {
          std::printf("Web 端口还被上一个实例占着，等它让出来（最多 5 秒）…\n");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        started = web.start(wo, err);
      }
    }
    if (!started) {
      std::fprintf(stderr, "\nWeb 服务起不来：%s\n", err.c_str());
      return 1;
    }
    std::printf("Web 界面：http://127.0.0.1:%d/\n", web.port());
    // 登录这一条要打出来，而且要打**默认口令是 admin** —— 用户第一次打开
    // 页面时唯一需要的信息就是"我拿什么进"。不说的话，他会去翻源码。
    if (wo.auth_enabled && !wo.auth_password.empty()) {
      std::printf("          登录：用户名 %s，口令 %s\n", wo.auth_user.c_str(),
                  wo.auth_password == "admin" ? "admin（**默认值，请尽快改掉**）"
                                              : "（配置文件里的 web.password）");
      if (wo.auth_password == "admin") {
        D2O_WARN("Web 控制台用的是默认口令 admin。改成自己的：配置文件里的 "
                 "web.password，或用 --http-password 传一次。");
      }
    } else if (wo.auth_enabled) {
      std::printf("          登录：**口令为空，等于没有**（见上面的警告）\n");
    } else {
      std::printf("          登录：已关闭（--no-http-auth）\n");
    }
  }

  // ---- 生命周期：起 / 停各一行（Phase V）---------------------------------
  //
  // 这两行回答的是同一个问题：**这台机器上的服务在什么时候被起过一次、停过
  // 一次**。它是排查"那天几点重启过"的唯一线索 —— 调试日志会滚掉（4 MB 一份
  // `.old`），而审计按月留存。
  //
  // **不看 `http`**：审计在裸形态下照样是开的（`cmd_run` 一层那个 `audit.open`），
  // 而"什么时候起过一次"恰恰是裸形态下最想知道的事 —— 那条路既没有网页、也
  // 没有日志文件，只剩这一条。
  //
  // 端口取**生效的那一份**（`web.port()` / `opt.rtsp.*`），不读命令行也不读
  // 配置文件：命令行覆盖在上面那段回写里已经折进去了，而 `web.port()` 还额外
  // 覆盖了"端口写 0、让系统挑一个"那种情形 —— 读配置会报一个没用的 0。
  std::string const listen_ports = [&]() {
    std::string s;
    if (http) s = "网页 " + std::to_string(web.port());
    if (opt.rtsp.enabled) {
      if (!s.empty()) s += " / ";
      s += "RTSP " + std::to_string(opt.rtsp.port);
    }
    // "一个都没开"是一个真实且常见的形态（`--record x.mp4 --seconds 10`
    // 走的就是它）。写成空串的话，读日志的人分不出"没开服务"和"这一行没写全"。
    if (s.empty()) s = "无（这一趟没开任何服务）";
    return s;
  }();

  // 正文的拼法抽成一份：它有**两条出口**（正常跑完，和"要重启了"那条提前
  // return）。两处各拼一遍的话迟早只改一处 —— 症状是"重启的那一次，日志里
  // 没有形态"，而那次恰恰是最需要知道形态的一次。
  auto const lifecycle_detail = [&listen_ports, form](std::string const& why) {
    std::string d = "版本 " + std::string(d2o::kVersion) + "，形态 ";
    d += form == RunForm::Worker    ? "worker（服务形态拉起）"
         : form == RunForm::Console ? "console（控制台）"
                                    : "bare（裸形态）";
    d += "，监听 " + listen_ports + "，" + why;
    return d;
  };

  // `--save-config` 在 `if (http)` **外面**：把配置写盘和开不开 Web 服务是
  // 两件事，`--record out.mp4 --save-config` 也该能把这次的参数存下来。
  if (args.has("--save-config")) {
    std::string serr;
    if (!d2o::save_config(cfg, config_path, serr)) {
      std::fprintf(stderr, "写配置文件失败：%s\n  %s\n", config_path.c_str(),
                   serr.c_str());
      return 2;
    }
    std::printf("配置已写入：%s\n", config_path.c_str());
  }

  // **写在 `--save-config` 之后**：那一段有一条 `return 2`（配置写不进去），
  // 而先写"起好了"再因为写配置失败退出，会让读日志的人看到一对
  // "startup 已就绪 / 然后什么都没有" —— 分不出那是崩了还是正常收了。
  audit.note(d2o::AuditWho{"system", "system"}, "startup",
             lifecycle_detail("已就绪"));

  // 统计每秒经这里投递到 web 循环。钩子在采集线程上跑，publish() 只做
  // "序列化 + 投递"，不会拖慢采集。
  if (http) {
    // 「性能不足，要不要降到下一档」这句建议的**档位名**只有这里知道（会话
    // 只认 fps/码率，`quality` 这个标签住在 `AppConfig` 里），所以在钩子里补。
    //
    // 三件事值得说明：
    //   1. **只在 `overloaded` 时补**。`throttled` 是"已经自动丢帧、还能撑住"，
    //      那时提示用户降档等于说"你最好再降一档" —— 而自适应正是为了让这一步
    //      不必发生。把建议和"要不要显示横幅"绑在同一个条件上，前端就只需要
    //      判 `perf_suggested_quality` 非空，不必再复述一遍状态机。
    //   2. **拷一份再填**。`s` 是会话自己那份统计，动它等于把建议写回会话 ——
    //      而那个对象下一秒就会被下一轮统计覆盖，看起来"生效了"其实只是巧合。
    //   3. 空串是合法结果（已经在最低档），前端据此把横幅降级成"只能关掉一些
    //      东西了"这类纯文字提示。
    session.set_stats_hook([&web, &suggest_mu, &suggest_quality, &ch_board,
                            &extra_channels, base_port = cfg.web.port](
                               d2o::SessionStats const& s) {
      // `channels` 这一份**顺路在这一拍里重拼一次**。
      //
      // 为什么不放在 `wo.channels_json` 那个回调里现拼：那个回调跑在 web 循环
      // 线程上，而它要遍历的 `extra_channels` 只有主线程会动 —— 现拼就是拿一把
      // 读一把没人保护的 vector。放在这里则天然安全：钩子跑在采集线程上，而
      // 主线程此刻正阻塞在 `session.run()` 里（改 `extra_channels` 只发生在
      // 两次 `run()` 之间）。每秒一次也正好是 `channels` 该有的刷新率。
      //
      // **顺序是先写 board 再拼**，不能反：`build_channels_json` 读的是
      // `board.get_ch1()`，先拼后写会让通道 1 那一行永远慢一拍。
      ch_board.set_json(
          build_channels_json(ch_board, extra_channels, s, base_port));

      if (s.perf_state != "overloaded") {
        web.publish(s);
        return;
      }
      d2o::SessionStats copy = s;
      {
        std::lock_guard<std::mutex> lk(suggest_mu);
        copy.perf_suggested_quality = suggest_quality;
      }
      web.publish(copy);
    });
    // 浏览器预览（/ws/preview）的 fMP4 字节。同样在编码线程上跑，
    // publish_preview 只做投递；WS 的 send 不是线程安全的，必须落到循环线程。
    //
    // **两件事，顺序不能反**：先把初始化分段存进 `preview_hub`（无条件），再看
    // "现在看的是不是我"决定要不要转发。只存不分发的那些路，是为了让用户之后
    // 切过去时手里有 moov 可重放 —— 那是切换能不能出画面的唯一前提。
    session.set_preview_hook(
        [&web, &preview_hub](
            bool is_init, std::shared_ptr<std::vector<std::uint8_t> const> bytes) {
          preview_hub.note_init(1, is_init, bytes);
          if (preview_hub.active.load(std::memory_order_relaxed) != 1) return;
          web.publish_preview(is_init, std::move(bytes));
        });
    // 预览那把"真实延迟"尺子的零点。只报一次，报的是**墙钟**（Unix 毫秒），
    // 由封装器定（见 fmp4_muxer.cpp 里 write_nal 那段）—— 前端拿它把
    // `<video>.currentTime` 换算成墙钟，才能和 `Date.now()` 相减。
    session.set_preview_t0_hook([&web, &preview_hub](std::int64_t t0_wall_ms) {
      preview_hub.note_t0(1, t0_wall_ms);
      if (preview_hub.active.load(std::memory_order_relaxed) != 1) return;
      web.publish_preview_t0(t0_wall_ms);
    });
  }

  // --- 采集：跑一段，中途换分辨率 / 收到重载请求就接着再跑一段 -------------
  //
  // **为什么是个循环**：录制中途显示器换了分辨率（插拔、切换用户、游戏全屏、
  // 改显示设置）之后，编码器的 SPS/PPS、MP4 的轨道头、SDP 里的
  // sprop-parameter-sets 全都绑死在旧尺寸上，就地改不了 —— 硬换只会得到一个
  // 解不出来的流。而 CaptureSession::run() 里的编码器、封装器、RTSP 服务
  // 全都是它自己的局部变量，所以"把整条链重建一遍"就是"再调一次 run()"。
  //
  // `POST /api/reload` 走的是同一条出口：用户改了配置要立刻看到效果，而"立刻"
  // 在这条链路上只能是"重开一遍"。两种起因在下面**不加区分**，因为要做的事
  // 一模一样 —— 重算选项、再跑一段。
  //
  // 文件按段落盘：第一段用用户给的名字，之后是 main.1.mp4、main.2.mp4 ——
  // 一个 MP4 轨道装不下两种分辨率，硬写进去就是个解不开的文件。名字由这里
  // 决定而不是由 CaptureSession 决定：文件怎么摆是命令行的事。
  d2o::SessionOptions seg = opt;
  d2o::CaptureSession::RunResult res = d2o::CaptureSession::RunResult::Finished;
  std::chrono::steady_clock::time_point const run_start =
      std::chrono::steady_clock::now();
  int segment = 0;
  for (;;) {
    // 重启位**必须在每一趟的头上查一遍**（第二次查在下面 `run()` 之后）。
    //
    // 两次都要的理由是 `run()` 的第一条语句就是 `stop_.store(false)`
    // （capture_session.cpp:662）—— 也就是说一个落在"上一段已经跑完、下一段
    // 还没进 `run()`"这个窗口里的 `request_stop()` 会被下一趟**就地清掉**，
    // 采集于是正常地又跑起来，而那个"重启"请求从此石沉大海。查这个位是唯一
    // 不受那个 `store(false)` 影响的判据。
    //
    // 这里（循环顶部）覆盖的是"请求落在上一次 run() 之后"那一半，`run()` 之后
    // 那一次覆盖的是"请求落在 run() 期间"那一半。剩下的窗口只有"回调恰好落在
    // run() 那一句 store(false) 和它真正开始采集之间"，宽度是几十纳秒级；
    // **这里不声称它绝对密闭** —— 真撞上就是用户再点一次，而不是一个永远
    // 不会响应的按钮。真要被咬到，升级路径是给 `SessionOptions` 加一个外部
    // 中止位（那是改采集侧接口，不值得为这个窗口现在就做）。
    if (restart_requested.load(std::memory_order_relaxed)) {
      res = d2o::CaptureSession::RunResult::Finished;
      break;
    }

    // **每一趟都从当前配置重算选项**，包括第一趟。
    //
    // 只在拿到 Restart 之后才算是不够的：网页那边一个 `POST /api/config`
    // 落在"web 起来了、但第一趟 run() 还没进循环"这个窗口里时，`seg` 里装的
    // 还是开机时的值 —— 而用户已经看到"已保存"了。每次重算之后那个窗口就
    // 不存在了，重启请求也因此永远不会被吞掉（见 `request_reconfigure`）。
    //
    // 重算只动"由配置决定"的那批字段：`seconds`（时间预算，下面会缩）、
    // `output_path`（分段文件名）、`device`（ONVIF 改名的落点）、
    // `pool_size` / `queue_capacity`（不是配置字段）都原样留着。
    //
    // **取快照再算，不直接读 `cfg`**：写 `cfg` 的是循环线程（`apply_config`
    // 里那一句整体赋值），这里读它就是数据竞争 —— 而 `AppConfig` 里全是
    // `std::string` 和 `vector`，拷到一半被读走的是内部指针，那是真会崩的。
    // 每段一次拷贝，而分段本来就是"换个分辨率才发生一次"的事。
    d2o::AppConfig snap;
    {
      std::lock_guard<std::mutex> lk(cfg_mu);
      snap = reload_cfg;
    }
    d2o::apply_config_to_options(snap, seg);

    // 运行侧基线**就在这里跟上**：从这一句起，"接下来这一趟要跑的"就是 `seg`。
    //
    // **不能放在 `run()` 返回之后**（最初就是那么写的，实测当场撒了谎）：
    // `seg` 是在循环顶部从 `snap` 重算的，所以 `run()` 返回 `Restart` 的那一刻，
    // `seg` 里装的还是**这一趟**的旧选项 —— 新配置要到下一圈才进 `seg`。于是
    // 重载跑完一轮，基线反被旧值钉住，`pending` 一项不少地留在界面上：用户点了
    // 降档、管线真的重开过、画面真的降了，看到的却还是"有 4 项已保存，但需要重启
    // 程序才会生效"。他于是去重启一次，重启完发现没有任何变化 —— 而"哪些改了要
    // 重启"这个列表正是这一整套机制唯一的存在理由，从那一刻起它就不再被相信了。
    //
    // 放在这里覆盖的正是那条路径：重载请求落在某一段中间时 `run()` 返回
    // `Restart`，下一圈拿 `reload_cfg` 重算 `seg`（新档位），基线跟着换成新的，
    // `pending` 自然清空 —— 而这**同时也是"改配置 + 立刻重开"两步连做所必需的**，
    // 那正是「降到下一档」按钮干的事。
    //
    // 放在 `run()` 之前会不会把"还没生效"说成"已生效"？中间只隔着下面两句原子
    // 操作，管线在微秒级内就开始跑了。要看到那句谎话，网页得恰好挤进这一段 ——
    // 与"基线被旧值钉住、于是**永久**显示一条不存在的待重启项"相比，这个窗口
    // 可以忽略；而 `Finished` 那几支（时间到了 / 开不起来）之后不会再有人问
    // "生效了没有"。
    //
    // 用 `sync_options_to_config` 而不是手写一份赋值：它就是开机那条路用的同一个
    // 函数，字段集不可能和 `apply_config_to_options` 漂移（漂移的症状是"某个字段
    // 重载之后永远挂在 pending 里"，看起来像重载没生效）。
    //
    // **不改 `cfg`**：pending 的定义就是"存盘的减运行侧的"，运行侧跟上之后差自然
    // 就没了；改 cfg 反而会让 `GET /api/config` 报出一份用户没存过的配置。
    {
      std::lock_guard<std::mutex> lk(run_mu);
      d2o::sync_options_to_config(seg, running_cfg);
      // `quality` 是个**标签**，两张选项表里都没有它（采集侧只认 fps / 码率这些
      // 具体值，见 `sync_options_to_config` 的说明），所以上面那句同步不到它。
      // 少了这一行，用户点完降档会看到"画质档位：需要重启" —— 而那个档位刚刚
      // 才被完整地应用在这一段上。
      running_cfg.capture.quality = snap.capture.quality;
    }

    // --- 通道 2 起：这一趟的对象、共享服务器与线程 ------------------------
    //
    // **每一趟全部重建。** 采集链（编码器、封装、SDP 里的尺寸）全都绑死在创建
    // 时的尺寸上，而通道 1 那条路已经是"换分辨率就整条重建"，通道 2 起没有理由
    // 不一样。代价是这几路的 ONVIF 端点会在重开的那一两秒里断一下（通道 1 的
    // 不会）—— 而那一刻本机所有通道的推流本来就在断，不差这一个。
    for (d2o::ChannelPlan const& p : channel_plans) {
      auto ch = std::make_unique<ExtraChannel>();
      ch->plan = p;
      // 名字的兜底：`validate_config` 会给显式项补上名字，但计划是纯函数，
      // 手搓的 plan（测试、以后别的调用方）可能留空 —— 空名字在状态那一栏里
      // 就是一行空白，看着像界面坏了。
      if (ch->plan.name.empty()) ch->plan.name = d2o::fmt("通道 %u", p.index);
      // 录制文件名：`out\mc.mp4` → `out\mc-ch2.mp4`。没给 `--record` 时是空串，
      // 这一路就只推流（见 `channel_record_filename`）。
      ch->record_path = channel_record_filename(opt.output_path, p.index);
      ch->opt = d2o::options_for_channel(p, seg, ch->record_path);
      ch->session = std::make_shared<d2o::CaptureSession>();
      // 登记给 `preview_hub`：`/api/snapshot.jpg` 要按"现在看哪一路"取帧，而
      // 通道 1 的 WebServer 够不着这一路的会话。**无条件登记**（连关着的那几路
      // 也登记）：少登记一路的后果是"切过去之后按快门没反应"，而那个症状看不出
      // 和"这一路在计划里是关着的"有任何关系。
      preview_hub.put_session(p.index, ch->session);
      if (!p.enabled) {
        // **对象照样建**：它要在状态那一栏里显示成"已关闭"。关掉的那一路凭空
        // 消失的话，"我关掉的那一路去哪了"就没有答案。
        //
        // 两种"不建"要分开写：`blocked_reason` 非空的是**建不了**（同一块屏
        // 被前面某一路占了），空的是**你自己关的**。把建不了的那种也写成
        // "已关闭"的话，用户会去配置里找一个**明明是开着的**开关。原因整句
        // 照搬 —— 它是写给用户看的，中间再加工一道只会丢信息。
        ch->set_state(p.blocked_reason.empty()
                          ? std::string("已关闭")
                          : "未启用：" + p.blocked_reason);
      } else {
        // 这一路的统计钩子。它**同时是"这一路有下游"的凭据**：没有它，
        // `run()` 在"不录也不推流"时会当场判定 Failed（见 capture_session.cpp
        // 里那条守卫），而"只录不推流"是合法配置。
        ExtraChannel* const c = ch.get();
        ch->session->set_stats_hook([c](d2o::SessionStats const& s) {
          c->fps.store(s.effective_fps, std::memory_order_relaxed);
          c->bitrate_kbps.store(
              static_cast<double>(bitrate_kbps_of(s.bytes, s.elapsed_s)),
              std::memory_order_relaxed);
          // 这一路真实的编码尺寸也报给它自己的 ONVIF 端点。
          //
          // **不报的话 `GetProfiles` 会永远停在启动时那个猜出来的尺寸上**：
          // 通道 2 起的 `WebServer` 只服务 SOAP，没有任何人订阅 `/ws/stats`，
          // 于是 `publish()` 那条"每秒用真值覆盖一次"的路对它们根本不存在。
          // 而 `main.cpp` 建 profile 时只能猜 —— 它按 1920x1080 推子码流的宽
          // （那时采集还没开，连桌面/摄像头的原生尺寸都不知道）。实测通道 2
          // 主码流 1620x1080、子码流 540x360，而 profile 报的是 640x360。
          // 两个数都不报错，只有 NVR 里的分辨率和画面对不上。
          //
          // `c->web` 在这条线程上**只读**：它在 `ch->thread` 起来之前就建好了
          // （所以要失败也是在那之前失败），而 `teardown_channels` 是
          // **先 join 再 stop/析构**。那两处合起来才让这里省掉一把锁。
          if (c->web) {
            c->web->publish_video_size(s.video_width, s.video_height,
                                       s.sub_width, s.sub_height);
          }
        });

        // 这一路的预览字节 —— 只有在**网页开着**的时候才装（和通道 1 同一条
        // 规矩，见上面那个 `if (http)`）。装了它，会话里就会多跑一个 fMP4 封装器
        // （`capture_session.cpp` 里 `if (preview_hook_)` 那段），**没有重新编码**
        // —— 它只是把同一批 NAL 另抄一份。这是"网页上一次看一路、随时能切"的
        // 全部代价。
        //
        // **先存后判**（和通道 1 一模一样）：初始化分段无条件存进 hub，转发才看
        // "现在看的是不是我"。少了这一步，用户切到通道 2 时手里没有它的 moov，
        // 画面一直黑着 —— 而且不报错。
        //
        // 转发的目标是**通道 1 那个 WebServer**（`web`）：控制台只有一个，
        // 通道 2 起的那几个 WebServer 只服务各自端口的 ONVIF，没人订阅它们的
        // `/ws/preview`。
        if (http) {
          unsigned const idx = ch->plan.index;
          ch->session->set_preview_hook(
              [&web, &preview_hub, idx](
                  bool is_init,
                  std::shared_ptr<std::vector<std::uint8_t> const> bytes) {
                preview_hub.note_init(idx, is_init, bytes);
                if (preview_hub.active.load(std::memory_order_relaxed) !=
                    static_cast<int>(idx)) {
                  return;
                }
                web.publish_preview(is_init, std::move(bytes));
              });
          ch->session->set_preview_t0_hook(
              [&web, &preview_hub, idx](std::int64_t t0_wall_ms) {
                preview_hub.note_t0(idx, t0_wall_ms);
                if (preview_hub.active.load(std::memory_order_relaxed) !=
                    static_cast<int>(idx)) {
                  return;
                }
                web.publish_preview_t0(t0_wall_ms);
              });
        }
        ch->set_state("未启动");
      }
      extra_channels.push_back(std::move(ch));
    }

    // --- 共享的 RTSP 服务器：一台，全部通道挂在上面，靠路径区分 ------------
    //
    // 顺序即下标：通道 1 的两路先进（下标 0 = 主码流，这条约定别处也在读），
    // 然后按通道号依次追加。每一路的 `rtsp_stream_base` 就是"它的主码流在服务器
    // 上是第几路"，而 `rtsp_stream_slots` 是"给它排了几个槽" —— 那个数必须由
    // 这里显式给出，**不能**让会话拿 `base + 1 < stream_count()` 去猜：那个写法
    // 在越界之外还有一种更坏的情形，`base + 1` 正好是下一个通道的主码流，于是
    // 通道 1 的子码流地址上播的是通道 2 的画面，两路各自都"正常"。
    if (seg.rtsp.enabled) {
      d2o::RtspOptions server_rtsp;
      server_rtsp.enabled = true;
      server_rtsp.port = seg.rtsp.port;
      server_rtsp.streams = seg.rtsp.streams;   // 通道 1

      // 音频这一路**必须在这里**交给服务器，理由和它从前挂在会话上时一样，
      // 只是方向反了：`ServerMediaSession`（音频 subsession 要挂上去的那个）
      // 是 `RtspServer::start()` 里一次性建好的，而那已经是**最后一站** ——
      // 服务器比任何采集会话都先起。
      //
      // ⚠️ 这一份漏掉的代价不是"音频少个开关"：`push_audio_au()` 在服务器上
      // 没有音频轨时**按契约静默忽略**，于是 RTSP 那条流里根本没有音频
      // subsession，而 MP4 那一路照录不误 —— 现象是"录像里有声音、NVR 上没
      // 声音"，两边单看都正常。2026-10-09 就是漏了这一行（第五轮把服务器
      // 从会话里搬到 `cmd_run` 时），`tests\audio_e2e.ps1` 的 [2] 段红的。
      //
      // 三件事的取值都由**另外两处**定，这里一条都不许另算：
      //   * 采样率 / 声道 —— 采集层把每一路统一成 48 kHz 立体声
      //     （`AudioMixer::Options` 的说明），`AudioConfig` 里根本没有这两项；
      //   * `config_hex` —— `AacEncoder::asc_hex_for()` 按上面那两个数算出来
      //     的预测值，会话起来之后拿编码器的真值核对（见 capture_session）；
      //   * `bitrate_kbps` —— 只进 SDP 的 `b=AS:`，与编码器那个是同一个数。
      server_rtsp.audio.enabled = seg.audio.enabled;
      if (server_rtsp.audio.enabled) {
        server_rtsp.audio.sample_rate = 48000;
        server_rtsp.audio.channels = 2;
        server_rtsp.audio.bitrate_kbps = seg.audio.bitrate_kbps;
        server_rtsp.audio.config_hex =
            d2o::AacEncoder::asc_hex_for(server_rtsp.audio.sample_rate,
                                         server_rtsp.audio.channels);
        // 算不出来（采样率不在表里）时**关掉**而不是带着空 config 往下走：
        // `start()` 对"开着却没 config"是直接判失败的，那会让整台机器一个
        // 通道都推不出去 —— 代价和收益完全不成比例。这里降级成纯视频，并把
        // 原因写进日志。
        if (server_rtsp.audio.config_hex.empty()) {
          D2O_WARN("音频开着，但 %u Hz %u 声道推不出 AudioSpecificConfig —— "
                   "这一趟按纯视频推流（录像里照样有音轨）。",
                   server_rtsp.audio.sample_rate, server_rtsp.audio.channels);
          server_rtsp.audio.enabled = false;
        }
      }

      seg.rtsp_stream_base = 0;
      seg.rtsp_stream_slots = static_cast<unsigned>(server_rtsp.streams.size());

      std::vector<std::string> owner(server_rtsp.streams.size(), "通道 1");
      auto const holder = [&](std::string const& path) -> std::string {
        for (std::size_t i = 0; i < server_rtsp.streams.size(); ++i) {
          if (server_rtsp.streams[i].path == path) return owner[i];
        }
        return std::string();
      };

      for (auto& ch : extra_channels) {
        if (!ch->plan.enabled) continue;
        std::vector<d2o::RtspStreamOptions> const& wants = ch->opt.rtsp.streams;
        std::string clash;
        for (d2o::RtspStreamOptions const& s : wants) {
          std::string const h = holder(s.path);
          if (!h.empty()) {
            clash = d2o::fmt("/%s（%s 在用）", s.path.c_str(), h.c_str());
            break;
          }
        }
        if (!clash.empty()) {
          // 两条流挂同一个地址时，Live555 只会让其中一条生效 —— 而它是一条
          // **能正常播的流**，分不出是哪一路。`validate_config` 已经在显式项
          // 之间查过一遍，但**自动发现的显示器通道没经过它**（路径是这里按
          // 通道号合成的），所以补这一次。这一路照录、不推流。
          D2O_WARN("通道 %u 的 RTSP 路径 %s，这一路不推流（照样录像）。"
                   "在配置里给它换一个 rtsp_path 就好。",
                   ch->plan.index, clash.c_str());
          ch->opt.rtsp_server = nullptr;
          ch->opt.rtsp_stream_slots = 0;
          ch->opt.sub_enabled = false;
          continue;
        }
        std::size_t const base = server_rtsp.streams.size();
        for (d2o::RtspStreamOptions const& s : wants) {
          owner.push_back(d2o::fmt("通道 %u", ch->plan.index));
          server_rtsp.streams.push_back(s);
        }
        ch->opt.rtsp_stream_base = base;
        ch->opt.rtsp_stream_slots = static_cast<unsigned>(wants.size());
      }

      rtsp_server = std::make_unique<d2o::RtspServer>();
      if (!rtsp_server->start(server_rtsp, err)) {
        // 和从前一样，是**致命**的：配置里开着 RTSP 而端口被占。继续跑的话
        // 每一路都推不出去，而录下来的文件完好无损 —— 这个故障会显得不严重。
        std::fprintf(stderr, "\nRTSP 服务起不来：%s\n", err.c_str());
        return 1;
      }
      // **指针只在 start() 成功之后发下去**：早发的话，起失败的那些通道手上
      // 会留着一个指向半初始化服务器的地址。
      seg.rtsp_server = rtsp_server.get();
      for (auto& ch : extra_channels) {
        if (ch->opt.rtsp_stream_slots > 0) ch->opt.rtsp_server = rtsp_server.get();
      }
    } else {
      // 上一段留下的值要清掉 —— 它是个裸指针，而上面那块 `rtsp_server` 已经
      // 被 teardown 放掉了。
      seg.rtsp_server = nullptr;
      seg.rtsp_stream_slots = 0;
    }

    // --- 通道 2 起：ONVIF 端点 + 采集线程 ---------------------------------
    //
    // ONVIF 挂在 WebServer 上，所以只有"通道 1 起了 Web"时才给通道 2 起端点 ——
    // `--no-http` / `--no-onvif` 时通道 1 自己也没有端点，通道 2 起没有理由例外。
    // **采集线程和这个开关无关**：只推流、只录像的多通道不许因为"网页关了"而停。
    bool const channels_onvif =
        (cfg.onvif.enabled || args.has("--onvif")) && !args.has("--no-onvif");
    for (auto& ch : extra_channels) {
      if (!ch->plan.enabled) continue;

      if (http && channels_onvif) {
        d2o::WebOptions wo2;
        wo2.enabled = true;
        wo2.host = cfg.web.host;
        // **每通道一个端口**（用户选的形态）：通道 N 在 `web.port + N`，
        // 通道 1 在 `web.port` 本身。
        wo2.port = cfg.web.port + static_cast<int>(ch->plan.index);
        // 静态文件不挂：这是一条**给 NVR 用的**端点，没有人在浏览器里打开它。
        wo2.static_root.clear();
        // **但登录照样开，凭据用 ONVIF 那一对。**
        //
        // 一开始这里是 `auth_enabled = false`（"没人会在浏览器里打开 8101"）。
        // 那是错的：`WebOptions::snapshot_source` 一装上，`/api/snapshot.jpg`
        // 那条**浏览器**路由就跟着注册了（它和 ONVIF 那条 `<snapshot_path>`
        // 是同一个 `if` 里的两个路由），而它认的是 Web 会话 —— 关掉登录就
        // 等于"局域网里任何人都能在 8101 上取到这一路的画面"。ONVIF 那条
        // 走 Digest，不受影响。
        //
        // 复用自己的 ONVIF 凭据而不是另设一套：这一路本来就只有一套凭据，
        // 编不出第二个"只有网管知道"的口令。装好之后启动日志的那句
        // "HTTP 绑在 0.0.0.0 且没有登录"也自动变成如实的那一句。
        wo2.auth_enabled = true;
        wo2.auth_user = cfg.onvif.user;
        wo2.auth_password = cfg.onvif.password;
        // **WS-Discovery 只能起一次**：UDP 3702 只有一个 socket，第二个
        // `bind` 必冲突。全表挂在通道 1 那个 app 上（见 `wsd_extra_endpoints`）。
        wo2.wsd_enabled = false;
        wo2.device_state = &device_state;
        wo2.device_uuid = d2o::make_uuid_urn(
            d2o::fmt("%s|ch%u", machine_seed_value.c_str(), ch->plan.index));
        wo2.onvif_enabled = true;
        wo2.onvif.user = cfg.onvif.user;
        wo2.onvif.password = cfg.onvif.password;
        wo2.onvif.realm = "Desk2ONVIF";
        wo2.onvif.uuid = wo2.device_uuid;
        // RTSP 没开的时候也报 554（和通道 1 同一条规矩）：NVR 会把
        // GetStreamUri 的地址存下来，报 0 毫无意义。
        wo2.onvif.rtsp_port = seg.rtsp.enabled ? seg.rtsp.port : 554;
        wo2.onvif.http_port = wo2.port;   // 真实值由 start() 覆盖
        wo2.onvif.osd = cfg.osd;
        // 身份：主身份那份原样拿过来，换掉能区分通道的那几样（见
        // `channel_identity`）。NVR 的设备列表里那两台长得一模一样的时候，
        // 用户就是靠型号和序列号把条目和屏幕上的画面对上号的。
        d2o::apply_identity(
            wo2.onvif,
            channel_identity(cfg.onvif, ch->plan, machine_seed_value));
        // 每一路 RTSP 就是这一个通道的一个 ONVIF Profile，顺序和
        // `ch->opt.rtsp.streams` 一致（`set_video_size(i, …)` 靠这个下标找
        // profile，错位了 NVR 会把子码流的分辨率报成主码流的）。
        for (std::size_t i = 0; i < ch->opt.rtsp.streams.size(); ++i) {
          d2o::OnvifStreamProfile sp;
          // token **不要带空格**：有些 NVR 会把它原样拼进 URL 查询串而不转义。
          sp.token = (i == 0) ? "main" : "sub";
          sp.name = sp.token;
          sp.fps = ch->opt.fps;
          sp.bitrate_kbps = (i == 0) ? ch->opt.bitrate_kbps : ch->opt.sub_bitrate_kbps;
          sp.gop_seconds = ch->opt.gop_seconds;
          sp.rtsp_path = "/" + ch->opt.rtsp.streams[i].path;
          // `preset=ultrafast` 把档位压到 Constrained Baseline（x264 关掉了
          // CABAC/8x8dct），此时报 Main 有 NVR 会拒绝 —— 报低不报高。
          sp.h264_profile = (ch->opt.preset == "ultrafast") ? "Baseline" : "Main";
          if (i > 0) {
            int const sh = std::max(2, ch->opt.sub_height / 2 * 2);
            int const sw = std::max(2, (1920 * sh / 1080) / 2 * 2);
            sp.width = sw;
            sp.height = sh;
          }
          wo2.onvif.profiles.push_back(std::move(sp));
        }
        // 快照按**通道**取，不是按通道 1 取 —— 设备信息页要的那张图和它对应的
        // 那一路必须一致，否则 NVR 里点开通道 2 看到的是主屏。
        wo2.snapshot_source = [c = ch.get()] { return c->session->snapshot(); };
        // 审计走同一个 `audit`，和通道 1 的 `audit_note` 是同一件事（那个
        // 变量声明在上面的 `if (http)` 里，这里够不着，所以照它的样子再写
        // 一遍）。**没有它，从通道 2 起的端口进来的每一次 Set\* 都不会留痕**
        // —— 而"谁改的"恰恰是审计存在的理由。
        auto const audit_note2 = [&audit](d2o::AuditWho const& who,
                                          std::string const& action,
                                          std::string const& detail) {
          audit.note(who, action, detail);
        };
        wo2.audit = audit_note2;
        wo2.onvif.audit = audit_note2;
        // ⚠️ **`on_patch` / `on_osd_patch` 故意留空**（= 只改内存里的设备状态，
        // 不落盘）。NVR 从通道 2 起的端口改名字/改 OSD，改的是这一路的内存副本，
        // 重启就没了 —— 而**存盘没有正确的落点**：配置里 `onvif.*` 只有一份，
        // 那是通道 1 的；通道 2 起的身份在 `channels.items[]` 里。把 NVR 的改动
        // 写进通道 1 的身份会让两路互相改。这一条要在 README 里说清楚。
        ch->web = std::make_unique<d2o::WebServer>();
        std::string werr;
        if (!ch->web->start(wo2, werr)) {
          // **warn + 继续**（和通道 1 不一样）：端口被占只意味着这一路没有
          // ONVIF，而它照样录、照样推流 —— 丢掉整个进程才是真的亏。
          D2O_WARN("通道 %u 的 ONVIF 端口 %d 起不来（%s），这一路没有 ONVIF 端点，"
                   "但推流和录像照常。",
                   ch->plan.index, wo2.port, werr.c_str());
          ch->web.reset();
        }
      }

      ExtraChannel* const c = ch.get();
      ch->thread = std::thread([c, total = opt.seconds, started = run_start] {
        run_channel_segments(*c, total, started);
      });
    }

    // 通道 1 那一行（喂给 `channels` 数组用）。`state` 先按"这一趟要跑了"写，
    // 真正跑起来之后由下面的统计钩子逐秒刷新数字，状态则等 `run()` 回来再落定。
    {
      ChannelBoard::Ch1Info info;
      info.name = "通道 1";
      info.source = opt.source.kind.empty() ? "display" : opt.source.kind;
      info.state = "未启动";
      info.record_file = opt.output_path;
      // 主码流的路径。取 `opt`（主线程独有、`run()` 改的是它的副本 `seg`），
      // 不取 `seg.rtsp.streams[0]` —— 那个的 `url` 会被采集线程就地补写。
      if (!opt.rtsp.streams.empty()) {
        info.rtsp_path = "/" + opt.rtsp.streams[0].path;
      }
      ch_board.set_ch1(std::move(info));
    }

    // 置位/清位夹住 `run()`：`POST /api/reload` 的回调读它来决定回哪一句
    // note（"正在重开"还是"本来就没在录"）。
    session_running.store(true, std::memory_order_relaxed);
    ch_board.set_state("采集中");
    res = session.run(seg, err);
    session_running.store(false, std::memory_order_relaxed);

    // 状态落定。**失败要把原因写进去**：通道 1 起不来时用户看到的第一处就是
    // 「状态」那一栏，只写"已停止"的话，他得翻日志才知道是权限、显示器没了
    // 还是编码器没打开。
    ch_board.set_state(res == d2o::CaptureSession::RunResult::Failed
                           ? (err.empty() ? std::string("失败")
                                          : std::string("失败：") + err)
                           : std::string("已停止"));
    // **在这一刻把 `channels` 重拼一次，而不是等下一拍的钩子。**
    //
    // 下一拍不会来了（通道 1 已经停了），而这一份是**最后一份**：它得是
    // "所有通道都停了"的样子，而不是"最后一拍还在跑"的样子。数字归零也是
    // 同一个理由 —— 挂着最后一拍的非零 fps 看着像还在跑。
    //
    // 顺序上放在 `teardown_channels()` **之前**：那之后 `extra_channels` 就空了，
    // 拼出来的数组会只剩通道 1 —— 而"关掉/停掉的那一路凭空消失"正是这一块
    // 一开始就说明要避免的事（见 `ExtraChannel` 那段）。
    for (auto& ch : extra_channels) {
      if (ch->plan.enabled) ch->set_state("已停止");
    }
    ch_board.set_json(build_channels_json(ch_board, extra_channels,
                                          d2o::SessionStats{}, cfg.web.port));

    // 通道 1 停了，其余几路也就该停了。**这一句必须在这里**：Ctrl+C 和父进程
    // 看守都只认得通道 1 那条会话（`g_ctrl_session`），所以"停下来"这件事是
    // 从通道 1 传出去的。
    teardown_channels();
    // 重启位在 `run()` **之后**再查一次：这一次覆盖"请求落在采集正跑着的时候"
    // （那时候 `request_stop()` 是被 `run()` 自己的等待循环看见的，正常收尾）。
    // 因为上面那句 `request_stop()`，这条路上 `res` 通常是 `Finished` —— 也就是
    // 一份完整、带 trailer 的录像，这正是要的效果。理由见循环顶部那一段。
    if (restart_requested.load(std::memory_order_relaxed)) break;
    if (res != d2o::CaptureSession::RunResult::Restart) break;

    ++segment;
    if (segment > kMaxSegments) {
      err = d2o::fmt("中途重开了 %d 次（换分辨率或点重载），停下来 —— 再开下去"
                     "就成了反复重建而录不到东西。用一个固定的显示模式，"
                     "或者先关掉会自动切分辨率的程序。",
                     kMaxSegments);
      res = d2o::CaptureSession::RunResult::Failed;
      break;
    }

    // 时间预算是**整段运行**的，不是每段一份：--seconds 20 在两段里也是 20 秒。
    if (opt.seconds > 0) {
      double const used = std::chrono::duration<double>(
                              std::chrono::steady_clock::now() - run_start)
                              .count();
      int const left = opt.seconds - static_cast<int>(used);
      if (left < 1) {
        // 剩下的时间不够再录一段了。这一段已经完整收尾，就当正常结束。
        res = d2o::CaptureSession::RunResult::Finished;
        break;
      }
      seg.seconds = left;
    }

    // 下一段的文件名：main.mp4 → main.1.mp4（没有扩展名就往后接）。
    d2o::SessionStats const& done = session.stats();
    std::string next_file;
    if (!opt.output_path.empty()) {
      std::string const& base = opt.output_path;
      std::size_t const dot = base.find_last_of('.');
      std::size_t const slash = base.find_last_of("/\\");
      bool const has_ext =
          dot != std::string::npos && dot != 0 &&
          (slash == std::string::npos || dot > slash + 1);
      next_file = has_ext
                      ? base.substr(0, dot) + "." + std::to_string(segment) +
                            base.substr(dot)
                      : base + "." + std::to_string(segment);
    }
    seg.output_path = next_file;

    std::printf("\n");
    if (!opt.output_path.empty()) {
      std::printf("这一段录到 %s —— %s\n", opt.output_path.c_str(),
                  done.summary().c_str());
    } else {
      std::printf("重开会话继续推流 —— %s\n", done.summary().c_str());
    }
    // 起因（换分辨率 / 点了重载 / NVR 改了参数）在这里**不区分**，因为要做的
    // 事一样。要区分的话就得让 CaptureSession 把起因当成返回值带出来，而
    // 调用方对三种起因的处理一字不差 —— 多一个字段只会多一处要维护的分支。
    std::printf("接着录：编码器、封装和 SDP 里的尺寸都是创建时写死的，改参数"
                "只能整条链重建，所以从这里另起一段%s。\n",
                next_file.empty() ? "" : ("（" + next_file + "）").c_str());
  }

  // 先停 Web 再做后面的打印：否则采集已经结束、web 还在推最后一帧旧数据，
  // 用户看到的是"进程要退了页面还在跳"。
  web.stop();

  // --- 恢复出厂：**放在重启之前，顺序不能换** -----------------------------
  //
  // 放在这里而不是放在 Web 的 handler 里，理由有两条：那些动作（备份、删配置、
  // 关日志句柄、复位 `DeviceState`）都必须在**主线程**上做；而这一刻的顺序恰好
  // 是唯一正确的顺序 —— web 已经停了、采集已经收尾、磁盘上没人在写配置。
  //
  // 它**不看** `res`：用户点的是恢复出厂，那就得恢复。哪怕这一趟采集是失败
  // 退出的（`Failed`），"点了恢复出厂而机器上一切照旧"也是最坏的结局。
  if (factory_reset_requested.load(std::memory_order_relaxed)) {
    // 「谁点的」要在这里取出来再传进去。**先取、再清**：`factory_reset_files`
    // 第三步会把整份审计清空，而这一份 `who` 正是清完之后那第一行的内容 ——
    // 顺序反过来的话，那行会变成"不知道是谁恢复了出厂"。
    d2o::AuditWho who;
    {
      std::lock_guard<std::mutex> lk(who_mu);
      who = factory_reset_who;
    }
    factory_reset_files(config_path, device_state, &audit, who);
  }

  if (res == d2o::CaptureSession::RunResult::Failed) {
    // 失败要**先说**，而且不能因为"用户点了重启"就把它咽掉：那会让"重启之后
    // 还是起不来"变成一个没有任何痕迹的现象。
    std::fprintf(stderr, "\n失败：%s\n", err.c_str());
    // 但**不在这里 return**：用户点的那个请求要兑现。对 worker 形态尤其重要
    // —— 退出码 7 是"立刻重拉"，而退出码 1 会让服务走退避表（3s→10s→30s）。
    // 用户点的那一下不该换来三十秒的黑屏。
    if (!restart_requested.load(std::memory_order_relaxed)) return 1;
  }

  // --- 重启：三种形态的去路不一样 -----------------------------------------
  if (restart_requested.load(std::memory_order_relaxed)) {
    if (factory_reset_requested.load(std::memory_order_relaxed)) {
      std::printf("恢复出厂设置已完成，接着重启服务…\n");
    }
    // 收尾这一行**要在 `restart_process()` 之前写**：那个函数在控制台形态下
    // 是 `CreateProcessW` + `return`，在 worker 形态下是 `return 7` —— 两条路
    // 都不再回到这里。写在后面等于"重启的那一次永远没有收尾行"，而这正是
    // 要排查的那一次。
    audit.note(d2o::AuditWho{"system", "system"}, "shutdown",
               lifecycle_detail(factory_reset_requested.load(std::memory_order_relaxed)
                                    ? "因恢复出厂后重启而退出"
                                    : "因重启服务而退出"));
    return restart_process(form, raw_cmdline);
  }

  d2o::SessionStats const& s = session.stats();
  std::printf("\n结束。\n");
  if (!opt.output_path.empty()) {
    // 分了段就**把最后一段也说清楚**：只报 `main.mp4` 的话，用户拿到的时长
    // 是他看到的一半，而"这次录了 9 秒"这句话会和"我明明录了 20 秒"打架。
    std::printf("  文件    : %s", opt.output_path.c_str());
    if (segment > 0 && seg.output_path != opt.output_path) {
      std::printf("（分了 %d 段；最后一段是 %s）", segment + 1,
                  seg.output_path.c_str());
    }
    std::printf("\n");
  }
  std::printf("  统计    : %s\n", s.summary().c_str());
  if (segment > 0) {
    // **不写"换了 N 次分辨率"。** 起因有两种（桌面分辨率变了 / 用户点了重载），
    // 而这里刻意不区分（见分段循环顶部的注释）—— 写成其中一种，另一种情况下
    // 这行就是错的，而"日志里写的原因和实际发生的事不符"比不写原因坏得多。
    std::printf("  分段    : 共 %d 段（中途整条链重建了 %d 次）\n", segment + 1,
                segment);
  }

  std::printf("\n验证一下：\n");
  if (!opt.output_path.empty()) {
    std::printf("  ffprobe -v error -show_entries stream=codec_name,width,height,"
                "r_frame_rate,avg_frame_rate,nb_frames -show_entries format=duration,"
                "bit_rate -of default=noprint_wrappers=1 \"%s\"\n", opt.output_path.c_str());
  }
  for (std::size_t i = 0; i < s.rtsp_streams.size(); ++i) {
    if (s.rtsp_streams[i].url.empty()) continue;
    std::printf("  ffprobe -v error -show_streams -show_format \"%s\"   # 第 %zu 路\n",
                s.rtsp_streams[i].url.c_str(), i);
  }
  if (!s.rtsp_streams.empty() && !s.rtsp_streams[0].url.empty()) {
    std::printf("  ffplay -rtsp_transport tcp \"%s\"\n", s.rtsp_streams[0].url.c_str());
  }
  // 收尾这一行**放在最后**：上面那些 `std::printf` 是给人看的，而这一行是
  // 给"三个月之后来查日志的人"看的 —— 两者谁先谁后不影响正确性，但放在最后
  // 意味着它写出来的那一刻，这一次运行真的结束了。
  //
  // 它**不进失败分支**（`res == Failed` 那条在上面 `return 1` 走了另一条路）：
  // 这里只覆盖"跑完了、正常退出"。失败退出没有收尾行这件事是**刻意的** ——
  // 失败那一条会打一行 `失败：…` 到 stderr，而审计里那一格留给"它为什么没
  // 起来"，不留给"它退出了"。写成两处都有的话，读的人要自己去分辨那行是
  // 收敛还是崩溃。
  audit.note(d2o::AuditWho{"system", "system"}, "shutdown",
             lifecycle_detail("正常退出"));
  return 0;
}

/// 「已经在运行了」时该说什么。
///
/// 这一整段的存在意义是**别让人去任务管理器里找答案**：双击第二下的人要的是
/// 那个网页，不是一句"已经有一个实例在运行"。
void print_running_hint(std::string const& config_path, bool open_browser) {
  d2o::AppConfig cfg;
  std::string cerr;
  bool existed = false;
  bool const ok = d2o::load_config(cfg, config_path, cerr, &existed);
  // 端口和开关**从配置文件读**，不去问正在跑的那个进程：问它要发一次 HTTP
  // 请求，而这里连它监听没监听都不知道（它可能正卡在启动里）。配置文件里的值
  // 就是它启动时用的值（命令行覆盖除外）；真不一样的话用户打开页面会发现。
  bool const web_on = ok && (cfg.web.enabled || !existed);
  if (!web_on) {
    std::printf("Desk2ONVIF 已经在运行了（这次没有开网页服务，所以没有地址可给）。\n"
                "要停掉它：任务栏里找它的窗口按 Ctrl+C，或者用网页/服务那一侧的操作。\n");
    return;
  }
  std::string host = ok ? cfg.web.host : std::string("0.0.0.0");
  if (host.empty() || host == "0.0.0.0" || host == "::") host = "127.0.0.1";
  std::string const url =
      "http://" + host + ":" + std::to_string(ok ? cfg.web.port : 8080) + "/";
  std::printf("Desk2ONVIF 已经在运行了，网页地址是 %s\n", url.c_str());
  if (open_browser) {
    // `ShellExecuteA` 而不是 W：这里给的是一个纯 ASCII 的 URL（host:port），
    // 没有要跨编码的路径。
    ::ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
  }
}

/// 长驻形态（`--console` / `--worker` / 不带任何参数）。三条路是**同一个东西**：
/// 采集 + Web + ONVIF + RTSP 常开的那个进程。
///
/// 「同一个东西」不是偷懒，是这一整轮架构的要点：装成服务时网页/ONVIF/RTSP/
/// 采集也全在 worker 里，服务进程只负责拉起它；不装服务时同一个程序直接就是
/// worker。于是**无论如何都只有一个 HTTP 服务、配置的权威副本只有一份**。
///
/// 两者唯一的差别：worker 有父进程可以盯（`--parent-pid`），console 没有。
///
/// **按值收 `ParsedArgs`**：下面要往里补开关（见那段注释）。
int cmd_console(ParsedArgs args, std::wstring const& raw_cmdline) {
  // --- 开箱即用：配置文件不存在时把 Web 与 RTSP 打开 ----------------------
  //
  // 为什么不无条件打开：那样用户在网页上把「启用 Web 控制台」关掉、存盘、
  // 重启之后它照样开着 —— 一个关不掉的开关比没有开关更糟，而且用户不会想到
  // 是这个原因。
  //
  // 为什么不无条件不打：出厂默认里 `web.enabled` / `rtsp.enabled` 都是 false
  // （它们存在是为了让 `--record out.mp4` 单独跑时不额外开端口），于是"双击
  // exe"会得到一个什么都不做、窗口一闪而过的程序。
  //
  // 「配置文件在不在」正好把这两件事分开：全新安装 / 绿色版解压 → 全开；
  // 用户存过一次配置 → 以他存的为准。判据是**文件在不在**，不是"字段是不是
  // 默认值"—— 后者分不出"没配过"和"显式关掉"。
  std::string const config_path = args.value("--config", d2o::default_config_path());
  {
    std::error_code ec;
    if (!std::filesystem::exists(config_path, ec)) {
      args.flags.push_back("--http");
      args.flags.push_back("--rtsp");
      std::printf("还没有配置文件（%s），按出厂设置起：Web 与 RTSP 都开。\n"
                  "想改就在网页上改一次、存一次，之后按你存的走。\n",
                  config_path.c_str());
    }
  }

  // --- 日志文件 -----------------------------------------------------------
  //
  // 长驻形态**必须有**这一份，而且两边理由不同：
  //   * 控制台形态：用户双击起来，用完把窗口一关 —— stderr 跟着消失。
  //   * 服务形态：worker 是 CreateProcessAsUserW 拉起来的，**根本没有控制台**，
  //     它的 stderr 落在虚空里。没有这一份，那种形态出问题时能拿到的只有
  //     "进程还在、端口开着"。
  //
  // 放在**配置文件旁边**（而不是 %ProgramData% 或 exe 旁边）：配置文件是用户
  // 自己指定的位置，日志跟着它走就不会出现"服务形态的 worker 想往 Program Files
  // 里写日志"这种事，测试传一个 `--config out\xxx.json` 时日志也跟着进 out\。
  {
    std::error_code ec;
    std::filesystem::path const p(config_path);
    std::filesystem::path const dir = p.parent_path();
    if (!dir.empty()) std::filesystem::create_directories(dir, ec);
    std::string const log_path =
        (dir.empty() ? std::filesystem::path("desk2onvif.log")
                     : dir / "desk2onvif.log")
            .string();
    d2o::Log::set_file(log_path);
  }

  // --- 单实例 -------------------------------------------------------------
  int handoff_ms = 0;
  {
    std::string err;
    if (!int_option(args, "--handoff", 0, 0, 60000, handoff_ms, err)) {
      std::fprintf(stderr, "%s\n", err.c_str());
      return d2o::kExitUsage;
    }
  }
  InstanceLock lock;
  if (!lock.try_acquire(handoff_ms)) {
    // **不是错误路径**：用户双击了两下，或者服务重拉时旧进程还没退干净。
    // 退 0 而不是退非零：服务那侧把非零（除 7 之外）当成崩溃，会退避重拉 ——
    // 而"已经有一个实例在跑"重拉多少次都是同样的结果。
    //
    // 浏览器只在**控制台形态**开：服务形态下真的走到这里，意味着旧 worker 还
    // 没退完，这时弹一个浏览器窗口是纯打扰。
    print_running_hint(config_path, /*open_browser=*/!args.has("--worker"));
    return d2o::kExitOk;
  }

  // **哪一种形态要在**这里定下来**，不能等到 cmd_run 里去看 `--worker`**：
  // `cmd_run` 也被 `--record` / `--rtsp` / `--http` 那条裸形态的路直接调用，
  // 而那条路的命令行里可以合法地出现任何开关。形态决定的是"自重启时是退 7
  // 让服务重拉，还是 CreateProcessW 拉一个自己"，猜错的后果是**服务形态下
  // 自己拉一个 worker 出来**——两个进程抢同一个单实例互斥体，或者更糟：
  // worker 没有父进程看守，服务一停它还在。
  RunForm const form =
      args.has("--worker") ? RunForm::Worker : RunForm::Console;
  return cmd_run(args, form, raw_cmdline);
}

/// `--service run | install | uninstall | status | print`。
///
/// `run` 是 SCM 拉起服务进程时用的内部入口（`install` 会把它写进服务的
/// 命令行里），其余四个是给人用的。
int cmd_service(ParsedArgs const& args) {
  std::string const sub = args.value("--service", std::string());
  d2o::ServiceCommand const cmd = d2o::parse_service_command(sub);
  if (cmd == d2o::ServiceCommand::Unknown) {
    std::fprintf(stderr,
                 "--service 后面要跟 run / install / uninstall / status / print 之一，"
                 "给的是 \"%s\"。\n"
                 "  install    装成开机自启的 Windows 服务（**需要管理员，会弹一次 UAC**）\n"
                 "  uninstall  停止并删除它\n"
                 "  status     看状态（只读，不需要管理员）\n"
                 "  print      只把要做的每一步打出来，什么都不改\n",
                 sub.c_str());
    return d2o::kExitUsage;
  }

  // **配置路径必须先转成绝对路径。** 服务的当前目录是 `C:\Windows\System32`
  // （SCM 起的进程都这样），相对路径在那里指向别的地方 —— 而症状是"服务起来了、
  // worker 也在跑，但它读的是一份全新的默认配置"：端口、口令、路径全是默认值，
  // 日志里一个字都没有。
  std::string config_path = args.value("--config", d2o::default_config_path());
  {
    std::error_code ec;
    std::filesystem::path const abs = std::filesystem::absolute(config_path, ec);
    if (!ec) config_path = abs.string();
  }
  bool const start_now = args.has("--start");

  switch (cmd) {
    case d2o::ServiceCommand::Run:       return d2o::service_run(config_path);
    case d2o::ServiceCommand::Install:   return d2o::service_install(config_path, start_now);
    case d2o::ServiceCommand::Uninstall: return d2o::service_uninstall();
    case d2o::ServiceCommand::Status:    return d2o::service_status();
    case d2o::ServiceCommand::Print:     return d2o::service_print(config_path, start_now);
    case d2o::ServiceCommand::Unknown:   break;
  }
  return d2o::kExitUsage;
}

}  // namespace

int main(int argc, char** argv) {
  // 第一件事，必须在任何往线程池投递的调用之前：libuv 默认只有 4 个线程池
  // 线程，而静态文件、快照编码、配置读写全排在那一条队上。传 0 = 按 CPU 核数。
  // 放晚了库会打一条 WARN 并且**设置不生效**（它返回 UV_EALREADY 而我们
  // 此时还没有 web 服务，只能靠这条注释和顺序保证）。
  d2o::WebServer::init_runtime();

  std::vector<std::string> args(argv + 1, argv + argc);
  ParsedArgs const parsed = parse_args(args);

  // 自重启要用**原始的命令行文本**，而且要原样带回去（路径里的空格、中文、
  // 引号一个都不能重新拼）。`argv` 是 CRT 转换过的 ANSI 版本，非 ASCII 的路径
  // 在那一刻就已经折损了，所以取宽字符那一条。
  //
  // 取一次放在这里（而不是在要用它的 `restart_process` 里现取）：那时进程已经
  // 准备退出了，而 `GetCommandLineW` 返回的是**本进程启动时**那一份，两者其实
  // 等价 —— 放在这里是为了让"这条命令行长什么样"在一开始就定下来，中间任何
  // 代码都不可能改到它。
  std::wstring const raw_cmdline = [] {
    LPWSTR const w = ::GetCommandLineW();
    return std::wstring(w != nullptr ? w : L"");
  }();

  // 日志先按参数配好，后面各命令的日志才有正确的级别。
  bool verbose = false;
  if (parsed.has("--quiet")) {
    d2o::Log::set_level(d2o::LogLevel::Error);
  } else if (parsed.has("--debug") || parsed.has("--verbose")) {
    verbose = true;
    d2o::Log::set_level(d2o::LogLevel::Debug);
  }

  // FFmpeg 和 x264 的日志走 av_log，级别是各自独立的。默认压到 ERROR：
  //
  //   * x264 会打一整段编码参数（几百字符的 options 行）、逐帧类型统计、
  //     末尾的帧统计表。这些对日常使用是纯噪音。
  //   * mp4 封装器会打 "Starting second pass: moving the moov atom..."，
  //     那是 faststart 的正常行为，不是问题。
  //
  // 出错的路径我们自己都用 av_strerror 转成人话报出来了，不依赖 av_log。
  // 需要看细节时用 --debug，那时候噪音就是信息。
  av_log_set_level(verbose ? AV_LOG_VERBOSE : AV_LOG_ERROR);

  if (parsed.has("--help") || parsed.has("-h")) {
    print_usage();
    return 0;
  }
  // 和 --help 并列放在最前面：`--version` 是一次**查询**，不该被别的参数盖过。
  // （原先它和 --selfcheck 走同一个分支、还排在 --record/--http 后面，于是
  //  `desk2onvif --version --http` 会真的把服务跑起来 —— 而用户只是想知道版本。）
  if (parsed.has("--version") || parsed.has("-v")) {
    return cmd_version();
  }
  if (parsed.has("--list-audio")) {
    return cmd_list_audio();
  }
  if (parsed.has("--list-displays")) {
    return cmd_list_displays();
  }
  if (parsed.has("--list-cameras")) {
    return cmd_list_cameras();
  }
  if (parsed.has("--firewall")) {
    return cmd_firewall(parsed);
  }
  if (parsed.has("--service")) {
    // 放在最前面：`--service run` 是 SCM 拉起来的那个进程，它的命令行里
    // 只有 `--service run --config <路径>`，不会被下面任何一条抢走。
    return cmd_service(parsed);
  }
  // **长驻形态优先于一次性形态。** 两者的区别不只是"跑多久"，还包括单实例锁和
  // 日志文件 —— 那两样属于"长驻的那个进程"，而 `--record` 单独出现时是一次性
  // 用法（`dxgi_recovery.ps1` 这类测试脚本就是这么用的），不该去抢锁。
  //
  // 所以：写了 --console/--worker 就按长驻形态走（此时再给 --record 就是"长驻着
  // 往这个文件录"，那正是后面 `/api/recording/*` 要用的语义）；没写就是老的
  // 一次性行为。反过来排的话，`--console --record x.mp4` 会静默退化成一次性
  // 形态 —— 没有锁、没有日志文件，而命令行看上去完全正常。
  if (parsed.has("--worker") || parsed.has("--console")) {
    return cmd_console(parsed, raw_cmdline);
  }
  if (parsed.has("--record") || parsed.has("--rtsp") || parsed.has("--http")) {
    // **裸形态**：这一路没有单实例锁、没有日志文件，是"跑完就退"的一次性用法。
    // 它的重启语义因此也只能是"自己拉一个自己"，不能退 7（外面没人守着）。
    return cmd_run(parsed, RunForm::Bare, raw_cmdline);
  }
  if (parsed.has("--selfcheck")) {
    return cmd_selfcheck();
  }

  if (!parsed.rest.empty()) {
    std::fprintf(stderr, "未知参数：%s（用 --help 看用法）\n", parsed.rest[0].c_str());
    return 2;
  }
  // **不带任何参数 = 按控制台形态跑起来。** 以前这里跑的是自检 —— 那是"还没有
  // 别的功能"时的占位，而安装包双击的就是这个入口：双击 exe 得到一个自检报告
  // 然后窗口消失，是最不像"能用"的一种表现。
  return cmd_console(parsed, raw_cmdline);
}
