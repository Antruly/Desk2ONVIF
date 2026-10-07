/**
 * @file src/main.cpp
 * @brief Desk2ONVIF 入口。
 *
 * 目前有三件事可做：
 *
 *   --selfcheck       把依赖真的链上、真的调一次，然后打印版本（Phase 0 的验证）
 *   --list-displays   列出所有显示输出，并**真的**探一遍哪些能录
 *   --record <文件>    录一段桌面到 MP4（Phase 1 的验证）
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
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#  include <windows.h>
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
#include "common/config.h"
#include "common/firewall.h"
#include "onvif/digest.h"
#include "onvif/net_identity.h"
#include "web/web_server.h"

#include <filesystem>
#include <memory>
#include <system_error>
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
  std::printf("Desk2ONVIF 自检 —— 目的只有一个：证明依赖真的链上、真的能加载。\n");

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
      "--list-displays", "--verbose", "--quiet", "--debug", "--rtsp", "--http",
      "--onvif", "--no-rtsp", "--no-http", "--no-onvif", "--no-wsd",
      "--sub", "--no-sub", "--save-config", "--no-http-auth"};
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
      "  --selfcheck            比对三套依赖的版本与能力，验完即退\n"
      "  --list-displays        列出显示输出，并逐个探测能不能录\n"
      "  --record <文件.mp4>     录制桌面到 MP4 文件\n"
      "  --rtsp                 开 RTSP 推流（可以和 --record 一起给）\n"
      "  --http                 开 Web 服务（REST + WebSocket + Web UI）\n"
      "\n"
      "采集与编码：\n"
      "  --display <N>          要录哪个输出，N 是 --list-displays 里的序号（默认 0）\n"
      "  --seconds <N>          跑多少秒；不给就一直跑到 Ctrl+C\n"
      "  --fps <N>              帧率（默认 30）\n"
      "  --bitrate <kbps>       码率（默认 4000）\n"
      "  --gop <秒>             关键帧间隔（默认 2）\n"
      "  --profile <名>         baseline | main | high（默认 main）\n"
      "  --preset <名>          x264 preset（默认 ultrafast）\n"
      "  --tune <名>            x264 tune（默认 zerolatency）。**不要改成\n"
      "                         film/animation 这类**：它们会开 B 帧和前瞻，\n"
      "                         直播延迟会从百毫秒级涨到秒级\n"
      "  --threads <N>          编码线程数（默认 6）。帧级并行下**每个线程多一帧\n"
      "                         延迟**，调大提吞吐、调小降延迟\n"
      "  --queue <N>            采集队列深度（默认 4）\n"
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
      "  -h, --help             这段文字\n");
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

/// 一次运行里最多重开几段。录制中途换分辨率是正常事件（插拔显示器、切用户、
/// 游戏全屏），每换一次就要把编码器/封装/SDP 整条链重建一遍，所以它是有代价
/// 的；而**反复**重建说明有东西在不停切模式（有些程序切进全屏会连切好几次）。
/// 到这个上限就停下来，而不是无限重开 —— 那会变成"一直在重开、什么都没录到"，
/// 比明确报错难查得多。
constexpr int kMaxSegments = 8;

int cmd_run(ParsedArgs const& args) {
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
    std::printf("要么给 --record <文件>，要么给 --rtsp，要么给 --http"
                "（可以任意组合）。\n");
    return 2;
  }

  std::string err;
  int tmp = 0;
  if (!int_option(args, "--display", cfg.capture.display_index, 0, 64, tmp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  opt.display_index = static_cast<unsigned>(tmp);
  if (!int_option(args, "--fps", cfg.capture.fps, 1, 240, opt.fps, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--bitrate", cfg.capture.bitrate_kbps, 100, 100000, opt.bitrate_kbps, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--gop", cfg.capture.gop_seconds, 1, 60, opt.gop_seconds, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  // 线程数的默认值刻意**不取配置**：合法的下限是 1，而上限是个宽范围，
  // 0（"自动"）不在 int_option 的区间里。配置里存的是生效值（见下面的回写），
  // 所以取 0 会让下次启动把"封顶 6"变成"封顶 1"。
  if (!int_option(args, "--threads", 6, 1, 128, opt.threads, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  if (!int_option(args, "--queue", 4, 1, 64, tmp, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
  opt.queue_capacity = static_cast<std::size_t>(tmp);
  if (!int_option(args, "--seconds", 0, 0, 24 * 3600, opt.seconds, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }

  opt.profile = args.value("--profile", cfg.capture.profile);
  opt.preset = args.value("--preset", cfg.capture.preset);
  // tune 以前只存在于配置结构和 SessionOptions 里，**没有任何地方把它读进来**
  // —— 配置文件里写 tune 是一句空话（实际用的是 SessionOptions 的默认值），
  // 而"改了没反应"这种症状最难查。这里接上。
  opt.tune = args.value("--tune", cfg.capture.tune);
  // 帧缓冲池要比队列深一个以上，否则"池子空了"会频繁触发 ——
  // 队列里排着的帧都还占着池子里的缓冲。
  opt.pool_size = opt.queue_capacity + 2;

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
  cfg.capture.display_index = opt.display_index;
  cfg.capture.fps = opt.fps;
  cfg.capture.bitrate_kbps = opt.bitrate_kbps;
  cfg.capture.gop_seconds = opt.gop_seconds;
  cfg.capture.threads = opt.threads;
  cfg.capture.preset = opt.preset;
  cfg.capture.tune = opt.tune;
  cfg.capture.profile = opt.profile;
  cfg.rtsp.enabled = opt.rtsp.enabled;
  // 端口和路径**只在 RTSP 真开着的时候回写**。关着的时候 opt.rtsp 里装的是
  // RtspOptions 的默认值，回写就等于把用户配好但暂时关掉的端口/路径抹成默认。
  if (opt.rtsp.enabled) {
    cfg.rtsp.port = opt.rtsp.port;
    // 存回带斜杠的形式，读的时候两种都认。
    cfg.rtsp.path = "/" + opt.rtsp.streams[0].path;
    cfg.rtsp.sub_enabled = opt.sub_enabled;
    cfg.rtsp.sub_height = opt.sub_height;
    cfg.rtsp.sub_bitrate_kbps = opt.sub_bitrate_kbps;
    // 子码流路径**只在它真开着的时候回写**，理由和端口/路径那条一样：关着时
    // opt.rtsp.streams 只有一路，从这里取子码流的路径会取到不存在的东西。
    if (opt.sub_enabled) cfg.rtsp.sub_path = "/" + opt.rtsp.streams[1].path;
  }

  // Web 服务要在采集之前起来：起不来（端口被占）就该立刻退出，
  // 而不是录了十秒才发现在报错。
  d2o::WebServer web;
  // **会话对象也在这里就构造。** 它是默认构造，不碰 DXGI/GPU，没有任何副作用，
  // 真正的采集是在下面的 run() 里才开始的。提前声明只有一个原因：快照回调要
  // 按引用捕获它，而那个回调必须在 web.start() 之前装进 WebOptions。
  //
  // 声明顺序不能反过来：成员按声明逆序析构，web 先声明就意味着 session 先析构、
  // web 后析构。`publish()` 是采集线程经 stats_hook 调进来的，而 run() 返回时
  // 那条线程已经 join 完了，所以"web 后死"只是多一层保险，不是必需的。
  d2o::CaptureSession session;

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
  // 踩过一次：`derived_serial` / `running_cfg` / `cfg_pending` 原先声明在块内，
  // 症状是"POST 一次 /api/config 就崩"，而且崩在别的线程、别的位置（内存被
  // 写坏之后谁先碰到谁先死），完全指不到这里。**声明的位置就是这里的全部
  // 要点**，别为了"变量离用处近一点"把它们挪回块内。
  std::string derived_serial;
  d2o::AppConfig running_cfg;
  std::vector<std::string> cfg_pending;

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
    // 设备 UUID 从机器标识派生，**跨重启稳定** —— NVR 拿 UUID 当主键，
    // 一变就会在列表里多出一台"新"摄像机。
    std::string const seed = d2o::machine_seed();
    if (wo.device_uuid.empty()) wo.device_uuid = d2o::make_uuid_urn(seed);
    wo.onvif.uuid = wo.device_uuid;
    // 序列号：配置里写死了就用它，否则从机器标识派生。**派生值跨重启稳定**，
    // 所以"配置文件里没有这一项"不会让 NVR 看到一台新设备。
    //
    // 只算一次、只存一份（变量本身声明在上面的 `if (http)` 之外，理由见那里）：
    // 启动时填身份和运行中热改身份都要用同一个派生值，两处各算一次就会有两个
    // "同一个"序列号（`make_serial` 纯由 seed 决定，所以结果相同 —— 但让它们
    // 只有一处来源，才不用去论证这句话）。
    derived_serial = d2o::make_serial(seed);

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

    wo.config_get = [&cfg]() { return d2o::config_to_json(cfg); };
    wo.config_pending = [&cfg_pending]() { return cfg_pending; };
    wo.config_set = [&cfg, &running_cfg, &cfg_pending, &config_path, &web,
                     &derived_serial](std::string const& body,
                                      d2o::ConfigApplyResult& out,
                                      std::string& cerr) {
      // 以**当前生效值**为底，不是以默认值为底：口令的哨兵（"__unchanged__"）
      // 靠的就是"out 里已经存着真口令"这件事，从默认值起手会把口令清成空串
      // —— 而空口令在这个服务里是合法的，于是它会静默地"改成无口令"。
      d2o::AppConfig next = cfg;
      if (!d2o::config_from_json(body, next, cerr)) return false;
      std::vector<std::string> notes;
      d2o::validate_config(next, notes);
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
        running_cfg.web.auth_enabled = next.web.auth_enabled;
        running_cfg.web.user = next.web.user;
        running_cfg.web.password = next.web.password;
      }

      std::vector<std::string> changed;
      d2o::diff_config(cfg, next, changed);  // 这次真的改动了哪些字段
      cfg = next;

      // pending 是**全量**重算，不是往上次的列表里追加。
      // 重算才能处理"把某一项改回原值"—— 追加的话它会一直挂在待重启里，
      // 而那一项其实早就不需要重启了。
      std::vector<std::string> pending;
      d2o::diff_config(cfg, running_cfg, pending);

      // applied = 这次变了 ∩ 不在 pending 里（也就是：真的当场生效了）。
      // 没变的字段不进 applied —— "已生效"报给用户看的是**这次保存做了什么**。
      std::vector<std::string> applied;
      applied.reserve(changed.size());
      for (std::string const& f : changed) {
        if (!std::binary_search(pending.begin(), pending.end(), f)) {
          applied.push_back(f);
        }
      }

      cfg_pending = pending;
      out.applied = std::move(applied);
      out.pending = std::move(pending);

      for (std::string const& n : notes) {
        D2O_WARN("保存的配置里有越界值，已夹到合法区间：%s", n.c_str());
      }
      return true;
    };
    // 快照取帧。`session` 在上面就构造好了，这里只是按引用捕获它。
    wo.snapshot_source = [&session]() { return session.snapshot(); };

    if (!web.start(wo, err)) {
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

  // 统计每秒经这里投递到 web 循环。钩子在采集线程上跑，publish() 只做
  // "序列化 + 投递"，不会拖慢采集。
  if (http) {
    session.set_stats_hook([&web](d2o::SessionStats const& s) { web.publish(s); });
    // 浏览器预览（/ws/preview）的 fMP4 字节。同样在编码线程上跑，
    // publish_preview 只做投递；WS 的 send 不是线程安全的，必须落到循环线程。
    session.set_preview_hook(
        [&web](bool is_init, std::shared_ptr<std::vector<std::uint8_t> const> bytes) {
          web.publish_preview(is_init, std::move(bytes));
        });
  }

  // --- 采集：跑一段，中途换分辨率就接着再跑一段 ---------------------------
  //
  // **为什么是个循环**：录制中途显示器换了分辨率（插拔、切换用户、游戏全屏、
  // 改显示设置）之后，编码器的 SPS/PPS、MP4 的轨道头、SDP 里的
  // sprop-parameter-sets 全都绑死在旧尺寸上，就地改不了 —— 硬换只会得到一个
  // 解不出来的流。而 CaptureSession::run() 里的编码器、封装器、RTSP 服务
  // 全都是它自己的局部变量，所以"把整条链重建一遍"就是"再调一次 run()"。
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
    res = session.run(seg, err);
    if (res != d2o::CaptureSession::RunResult::Restart) break;

    ++segment;
    if (segment > kMaxSegments) {
      err = d2o::fmt("中途换分辨率超过 %d 次，停下来 —— 再重开下去就成了"
                     "反复重建而录不到东西。用一个固定的显示模式，或者先关掉"
                     "会自动切分辨率的程序。",
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
      std::printf("采集尺寸变了，重开会话继续推流 —— %s\n",
                  done.summary().c_str());
    }
    std::printf("接着录：换分辨率之后编码器/封装/SDP 都得按新尺寸重建，"
                "所以从这里另起一段%s。\n",
                next_file.empty() ? "" : ("（" + next_file + "）").c_str());
  }

  // 先停 Web 再做后面的打印：否则采集已经结束、web 还在推最后一帧旧数据，
  // 用户看到的是"进程要退了页面还在跳"。
  web.stop();
  if (res == d2o::CaptureSession::RunResult::Failed) {
    std::fprintf(stderr, "\n失败：%s\n", err.c_str());
    return 1;
  }

  d2o::SessionStats const& s = session.stats();
  std::printf("\n结束。\n");
  if (!opt.output_path.empty()) {
    std::printf("  文件    : %s\n", opt.output_path.c_str());
  }
  std::printf("  统计    : %s\n", s.summary().c_str());
  if (segment > 0) {
    std::printf("  分段    : 共 %d 段（中途换了 %d 次分辨率）\n", segment + 1,
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
  return 0;
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
  if (parsed.has("--list-displays")) {
    return cmd_list_displays();
  }
  if (parsed.has("--firewall")) {
    return cmd_firewall(parsed);
  }
  if (parsed.has("--record") || parsed.has("--rtsp") || parsed.has("--http")) {
    return cmd_run(parsed);
  }
  if (parsed.has("--selfcheck") || parsed.has("--version") || parsed.has("-v")) {
    return cmd_selfcheck();
  }

  if (!parsed.rest.empty()) {
    std::fprintf(stderr, "未知参数：%s（用 --help 看用法）\n", parsed.rest[0].c_str());
    return 2;
  }
  // 还没别的功能时默认跑自检。以后这里会变成"启动服务"。
  return cmd_selfcheck();
}
