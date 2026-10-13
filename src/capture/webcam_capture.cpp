/**
 * @file src/capture/webcam_capture.cpp
 * @brief 见 webcam_capture.h 的说明。这里值得单独读的只有四处：
 *
 *   * `mf_platform()`：为什么"进程一次"和"懒初始化"都是必要的；
 *   * `mf_hr_text()`：为什么那个 switch **有** default（和 display_source.cpp 正好相反）；
 *   * `acquire()` 里行宽那段推导：这是整条路上最容易写错、错了又最难看出来的一处；
 *   * `reemit()` 为什么要留着那份没有水印的底图。
 */

#include "capture/webcam_capture.h"

#include "common/log.h"
#include "common/text.h"

#include <mfapi.h>
#include <mferror.h>

#include <chrono>
#include <cstring>
#include <mutex>
#include <utility>

namespace d2o {
namespace {

// ---------------------------------------------------------------------------
// 1. 进程级的 Media Foundation 平台
// ---------------------------------------------------------------------------

/// `MFEnumDeviceSources` 的签名 —— 就是 `mfidl.h` 里声明的那一份。
using PfnEnumDeviceSources = HRESULT(WINAPI*)(IMFAttributes*, IMFActivate***, UINT32*);

/// 第一路视频流。设备源**只有一路**，但这个常量在 MF 的 API 里处处要传。
DWORD const kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

struct MfPlatform {
  bool started = false;  ///< MFStartup 成功过 —— 成功过就必须配对一次 MFShutdown
  bool ok = false;       ///< 平台起来了，而且入口也解析到了
  std::string err;       ///< 没起来的原因（**人话**，不是 HRESULT）
  PfnEnumDeviceSources enum_sources = nullptr;

  /// 撤销平台。放在这个静态对象的**析构**里 = 进程退出时，**不是** `close()` 里。
  /// 理由见 `mf_platform()`。
  ~MfPlatform() {
    if (started) ::MFShutdown();
  }
};

void init_mf_platform(MfPlatform& p) {
  HRESULT const hr = ::MFStartup(MF_VERSION, MFSTARTUP_LITE);
  if (FAILED(hr)) {
    // 最常见的两个原因写进消息里，不然用户只会看到一个十六进制数：Windows N 版
    // 默认**不带**「媒体功能包」（MF 的一批 dll 根本不在），以及被精简过的系统。
    p.err = "Media Foundation 起不来（MFStartup " +
            hresult_hex(static_cast<long>(hr)) +
            "）—— 摄像头通道要它。Windows N 版默认不带「媒体功能包」，"
            "在「设置 → 应用 → 可选功能」里装上再试";
    return;
  }
  p.started = true;

  // 入口的解析。**必须 `LoadLibraryW` 而不是 `GetModuleHandleW`**：mf.dll 通常还
  // 没被谁加载过（被加载的是 mfplat.dll，而它**不导出**这两个函数 —— objdump 实测）。
  // **也不 `FreeLibrary`**：这个函数指针在进程余下的生命里都要有效，模块一卸它就
  // 悬空了。
  HMODULE mf = ::LoadLibraryW(L"mf.dll");
  if (!mf) {
    p.err = "加载 mf.dll 失败 —— 这台机器的 Media Foundation 不完整";
    return;
  }
  void* const proc = reinterpret_cast<void*>(::GetProcAddress(mf, "MFEnumDeviceSources"));
  if (!proc) {
    p.err = "mf.dll 里找不到 MFEnumDeviceSources —— 系统版本不对，或者 mfcore.dll 被删了";
    return;
  }
  // 先转 `void*` 再转函数指针：直接从一个函数指针类型转到另一个，`-Wextra` 下的
  // `-Wcast-function-type` 会报"参数列表不兼容"，而这里的不兼容是**必然**的 ——
  // `GetProcAddress` 返回的就是一个无参数的 FARPROC，我们拿到的地址和签名是同一份。
  p.enum_sources = reinterpret_cast<PfnEnumDeviceSources>(proc);
  p.ok = true;
}

/// 进程内的 MF 平台，**一次**，而且是**懒**的。
///
/// 为什么"一次"不能换成"每个 WebcamSource 各来一次"：`MFStartup` 在 MF 内部是
/// **引用计数**的（每一次都要配一次 `MFShutdown`），而"每个通道自己 Startup、
/// 自己 close 时 Shutdown"在多通道下会互相踩 —— 关掉通道 2 会把还在跑的通道 3
/// 的平台从底下抽走，症状是通道 3 突然开始报 0xc00d3e85（MF_E_SHUTDOWN），而它
/// 和"关通道 2"这个动作看起来毫无关系。
///
/// 为什么"懒"而不是"程序一起来就 Startup"：桌面那一路（DXGI）根本不需要 MF，
/// 只有真的要开摄像头才付这份开销。
///
/// 为什么 `MFShutdown` 在析构里而不是 `close()` 里：平台是**进程级**的东西，
/// 撤销的时机也应该只有一个。放进 close() 就正好踩上面那个坑。
/// 那个函数内静态本身。**和 `mf_platform()` 分成两个函数是必须的**：`call_once`
/// 要一个可调用对象，而 lambda 直接捕获这个块作用域的静态变量时，GCC 会报
/// "capture of variable with non-automatic storage duration"（`-Wall -Wextra` 下
/// 是警告，而本项目的规矩是警告就是缺陷）。拆开之后两边都不用捕获。
MfPlatform& mf_instance() {
  static MfPlatform p;
  return p;
}

MfPlatform& mf_platform() {
  static std::once_flag once;
  std::call_once(once, [] { init_mf_platform(mf_instance()); });
  return mf_instance();
}

// ---------------------------------------------------------------------------
// 2. 出错时说什么
// ---------------------------------------------------------------------------

/// HRESULT → 一句**能照着做**的话。
///
/// 为什么不直接把 0xc00d36b4 摆给用户看：那个数在微软的文档里查得到，而**在网页上
/// 看到一个十六进制数的人，除了截图发给别人之外没有任何办法**。这条路上会遇到的
/// 失败就那么几种，每一种都有明确的处置，所以这里把它们逐条写成人话。
///
/// ⚠️ 这个 switch 的 `default` 是**故意**留的，和 `display_source.cpp` 里那个不写
/// default 的 switch 不是一回事：那边 switch 的是**枚举**，漏掉一个取值编译器会报错；
/// 这边 switch 的是 HRESULT —— 一个 32 位的数，**穷举不了**，漏掉的那一个编译器
/// 永远不会提醒你。所以规矩反过来：宁可落到 default（"未知错误 0x……"，用户至少还
/// 能把那串数报出来），也不要为了"看起来完整"去编造一句话。
std::string mf_hr_text(HRESULT hr) {
  switch (static_cast<unsigned long>(hr)) {
    // --- 设备被别人占着 / 没了（这条路上最常见的一类）--------------------
    case 0x80070005UL:  // E_ACCESSDENIED
      return "摄像头被别的程序占着（相机 / 会议软件 / 浏览器标签页都算），"
             "把它们关掉再试";
    case 0xC00D3EA2UL:  // MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED
      return "摄像头被拔掉了，或者驱动刚重启过";
    case 0xC00D3EA3UL:  // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
      return "摄像头被别的程序抢走了（相机 / 会议软件）";

    // --- 协商 / 属性 ------------------------------------------------------
    case 0xC00D36B4UL:  // MF_E_INVALIDMEDIATYPE
      return "摄像头不支持这个输出格式";
    case 0xC00D36E6UL:  // MF_E_ATTRIBUTENOTFOUND
      return "摄像头没有报这个属性（我们问了一个它不提供的值）";
    case 0xC00D3704UL:  // MF_E_HW_MFT_FAILED_START_STREAMING
      return "摄像头起不了流（多半是 USB 带宽不够，或者设备已经没响应了）";

    // --- 我们自己的用法问题 ----------------------------------------------
    case 0xC00D3E85UL:  // MF_E_SHUTDOWN
      return "这个会话已经被关掉了 —— 一个通道只能有一个 SourceReader，"
             "在同一个设备源上重复建 reader 就是这个错";
    case 0xC00D36B0UL:  // MF_E_PLATFORM_NOT_INITIALIZED
      return "Media Foundation 平台没起来（MFStartup 没成功）";

    // --- 通用 -------------------------------------------------------------
    case 0x8007000EUL:  // E_OUTOFMEMORY
      return "内存不足";
    case 0x80004005UL:  // E_FAIL
      return "未指明的失败";
    default:
      break;
  }
  return "未知的 Media Foundation 错误 " + hresult_hex(static_cast<long>(hr));
}

/// 读帧失败时：这是"重建就能好"的，还是真故障？
///
/// 分错了的代价是不对称的：把可恢复的报成 Fatal，整个通道**停在那里不动**
/// （采集会话遇到 Fatal 就收工），而用户做的事只是插拔了一下摄像头；把不可恢复的
/// 报成 AccessLost，最坏是每分钟打一行"重建失败"的日志，然后一直重试。
/// 所以判据偏向 AccessLost。
SourceStatus classify_read_failure(HRESULT hr) {
  switch (static_cast<unsigned long>(hr)) {
    case 0xC00D3EA2UL:  // MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED —— 拔线
    case 0xC00D3EA3UL:  // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED —— 被抢
    case 0xC00D3E85UL:  // MF_E_SHUTDOWN —— 会话已经关了，重建一个新的就是修复
      return SourceStatus::AccessLost;
    default:
      return SourceStatus::Fatal;
  }
}

// ---------------------------------------------------------------------------
// 3. 枚举
// ---------------------------------------------------------------------------

/// `MFEnumDeviceSources` 交出来的那个数组**内存**的守卫。
///
/// 那次调用给的是两层所有权，**只做一层就是泄漏**：数组本身是 `CoTaskMemAlloc`
/// 出来的（要 `CoTaskMemFree`），每个元素是独立的一份引用（要 `Release`）。
/// 元素那一层在这里**故意**不做：它们会被接管进 `DeviceEntry`（见下），
/// 这里再 Release 一次就是**双重释放**。
struct ActivateArrayMem {
  explicit ActivateArrayMem(IMFActivate** p) : p_(p) {}
  ActivateArrayMem(ActivateArrayMem const&) = delete;
  ActivateArrayMem& operator=(ActivateArrayMem const&) = delete;
  ~ActivateArrayMem() {
    if (p_) ::CoTaskMemFree(p_);
  }

 private:
  IMFActivate** p_;
};

struct DeviceEntry {
  std::string name;  ///< 友好名（"FHD Camera"）
  std::string link;  ///< 符号链接（设备实例路径，系统里唯一）
  ComPtr<IMFActivate> activate;
};

/// 取设备上的一个字符串属性，转成 UTF-8；没有就返回空串。
///
/// `GetAllocatedString` 给的缓冲**必须** `CoTaskMemFree` —— 这正是它名字里
/// "Allocated" 的意思。忘了这一句就是每次枚举泄漏几个字节，症状要跑很久才显形。
///
/// 走 `to_utf8` 而不是 ANSI 转换：摄像头名可能是中文（"高清摄像头"），按代码页
/// 转会在 936 之外的机器上变成乱码 —— 而"名字是乱的"直接影响下面按名字挑设备。
std::string take_string(IMFActivate& dev, REFGUID key) {
  wchar_t* w = nullptr;
  UINT32 len = 0;
  if (FAILED(dev.GetAllocatedString(key, &w, &len)) || !w) return std::string();
  std::string const s = to_utf8(w);
  ::CoTaskMemFree(w);
  return s;
}

/// 枚举本机的视频采集设备。失败时填 err 并返回空数组。
std::vector<DeviceEntry> collect_devices(std::string& err) {
  std::vector<DeviceEntry> out;
  MfPlatform& platform = mf_platform();
  if (!platform.ok) {
    err = platform.err;
    return out;
  }
  // 这里单独再挡一次空指针：`ok` 与 `enum_sources` 是分开置的，将来有人加一条
  // 新的失败分支忘了同步它们时，宁可在这里返回一句人话，也不要跳进空指针。
  if (!platform.enum_sources) {
    err = "MFEnumDeviceSources 没有解析到（Media Foundation 的初始化没走完）";
    return out;
  }

  ComPtr<IMFAttributes> attrs;
  HRESULT hr = ::MFCreateAttributes(attrs.put(), 1);
  if (FAILED(hr)) {
    err = "建不出 Media Foundation 的属性表（MFCreateAttributes " +
          hresult_hex(static_cast<long>(hr)) + "）";
    return out;
  }
  // 不写这一条的话枚举出来的是**所有**设备源（音频、视频、甚至虚拟的），
  // 而我们要的只有摄像头。
  attrs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
                 MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);

  IMFActivate** raw = nullptr;
  UINT32 count = 0;
  hr = platform.enum_sources(attrs.get(), &raw, &count);
  ActivateArrayMem guard(raw);
  if (FAILED(hr)) {
    err = "枚举摄像头失败（MFEnumDeviceSources " + hresult_hex(static_cast<long>(hr)) +
          "）：" + mf_hr_text(hr);
    return out;
  }
  if (!raw || count == 0) {
    // **"一台都没有"不算失败**：err 留空，由调用方决定怎么说这句话（和
    // `list_audio_endpoints` 同一条规矩）。把"没有设备"和"查不了设备"混成一句，
    // 用户会去修那个根本没坏的东西。
    return out;
  }

  out.reserve(count);
  for (UINT32 i = 0; i < count; ++i) {
    DeviceEntry e;
    // `ComPtr` 没有"接管一个已经 AddRef 过的裸指针"的接口（只有 `put()` /
    // `put_opt()`），而枚举交出来的正是**裸的、已经加过引用**的指针。
    // `*e.activate.put()` 是这里唯一能表达"接管"的写法：`put()` 先 reset（此刻
    // 本来就是空），返回 `&p_`，写进去就等于那一份引用归我们管。
    *e.activate.put() = raw[i];
    e.name = take_string(*e.activate.get(), MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME);
    e.link = take_string(*e.activate.get(),
                         MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK);
    out.push_back(std::move(e));
  }
  return out;
}

// ---------------------------------------------------------------------------
// 4. 挑设备
// ---------------------------------------------------------------------------

/// ASCII 范围内的小写化。**只动 ASCII 是刻意的**：UTF-8 里所有非 ASCII 字节都
/// >= 0x80，原样比过去永远不会破坏编码；而按语言环境去折（土耳其语的 I 那种）
/// 需要一整张表，换来的是"名字里有中文时匹配不到"这种比"匹配得不准"难查得多的
/// 失败方式。
char ascii_lower(char c) {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool contains_ci(std::string const& hay, std::string const& needle) {
  if (needle.empty()) return true;
  if (needle.size() > hay.size()) return false;
  for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
    std::size_t j = 0;
    while (j < needle.size() && ascii_lower(hay[i + j]) == ascii_lower(needle[j])) ++j;
    if (j == needle.size()) return true;
  }
  return false;
}

/// 把候选设备列成一段能贴进错误信息的清单。**带上符号链接**：友好名会重
/// （两台一模一样的摄像头），符号链接不会，而"重名时该写哪一个"正是用户
/// 看到这条错误时唯一想问的事。
std::string device_list(std::vector<DeviceEntry> const& devs) {
  std::string s;
  for (std::size_t i = 0; i < devs.size(); ++i) {
    s += fmt("\n    %zu) %s", i + 1, devs[i].name.c_str());
    if (!devs[i].link.empty()) s += "  " + devs[i].link;
  }
  return s;
}

/// 把 selector 解释成"第几台"。失败时填 err（里面带着完整候选清单）。
///
/// 匹配顺序是有理由的，不是随手排的：
///   1. **符号链接精确**优先 —— 它是系统里唯一标识一台设备的东西，配置里存它最准；
///   2. **友好名精确**其次 —— 人写的配置多半就是这一种（"FHD Camera"）；
///   3. **友好名子串（不区分大小写）**最后 —— 手打半截名字也能用。
///
/// 每一层都要求**唯一**，不唯一就报错。静默挑第一个的坏处是它看起来**成功了**：
/// 用户看到的是"通道 2 录出来的不是我要的那台"，而日志里一个字都不提 —— 和
/// `display_source.cpp` 里"下标越界不许退回 0 号输出"是同一条纪律。
bool resolve_device(std::vector<DeviceEntry> const& devs, std::string const& selector,
                    std::size_t& index, std::string& err) {
  if (devs.empty()) {
    err = "本机一台摄像头都没有（设备管理器里看看是不是被禁用了）";
    return false;
  }
  // 空 = 第一台。这条规则必须和 `--list-cameras` 报出来的顺序一致，否则用户按
  // 清单上的名字写进配置，程序按"第一个非空字符串"去理解 —— 两台摄像头的时候
  // 两者的结论不同。
  if (selector.empty()) {
    index = 0;
    return true;
  }

  std::vector<std::size_t> hit;
  for (std::size_t i = 0; i < devs.size(); ++i) {
    if (devs[i].link == selector) hit.push_back(i);
  }
  if (hit.empty()) {
    for (std::size_t i = 0; i < devs.size(); ++i) {
      if (devs[i].name == selector) hit.push_back(i);
    }
  }
  if (hit.empty()) {
    for (std::size_t i = 0; i < devs.size(); ++i) {
      if (contains_ci(devs[i].name, selector)) hit.push_back(i);
    }
  }

  if (hit.empty()) {
    err = fmt("找不到摄像头「%s」。本机现在是这几台：%s\n"
              "  用 --list-cameras 看完整清单（配置里可以写符号链接，也可以写友好名）",
              selector.c_str(), device_list(devs).c_str());
    return false;
  }
  if (hit.size() > 1) {
    // 挑一个"看起来对的"在这里是最坏的选择：摄像头选错了，画面照样出、编码照样
    // 成功、NVR 那边一切正常，只有人看得出来"这不是我要的那台"。
    err = fmt("「%s」匹配到 %zu 台摄像头，不给你猜：%s\n"
              "  请把名字写完整，或者直接用 --list-cameras 里的符号链接",
              selector.c_str(), hit.size(), device_list(devs).c_str());
    return false;
  }
  index = hit[0];
  return true;
}

// ---------------------------------------------------------------------------
// 5. 帧率文本
// ---------------------------------------------------------------------------

/// 帧率写成给人看的样子。`MF_MT_FRAME_RATE` 是分数，所以 30000/1001 这种
/// （29.97）必须原样保留 —— 整除不了就说明它不是整数帧率，硬约成 30 是在编。
std::string fps_text(unsigned num, unsigned den) {
  if (num == 0) return "帧率未知";
  if (den == 0) return fmt("%ufps", num);
  if (num % den == 0) return fmt("%ufps", num / den);
  return fmt("%.2ffps", static_cast<double>(num) / static_cast<double>(den));
}

}  // namespace

// ---------------------------------------------------------------------------
// 6. 公开接口
// ---------------------------------------------------------------------------

std::vector<CameraInfo> enumerate_cameras(std::string* err) {
  std::string e;
  std::vector<DeviceEntry> devs = collect_devices(e);
  std::vector<CameraInfo> out;
  out.reserve(devs.size());
  for (std::size_t i = 0; i < devs.size(); ++i) {
    CameraInfo c;
    c.name = devs[i].name;
    c.symbolic_link = devs[i].link;
    c.is_default = (i == 0);
    out.push_back(std::move(c));
  }
  // 无条件写：成功时 `e` 是空的，这一句就等于**把 err 清掉** —— 调用方复用同一个
  // 串去接下一次调用的结果时，不会读到上一次留下的理由（那种陈旧理由是"明明枚举
  // 成功却报上一台机器的错"的来源）。枚举成功但本机确实没有摄像头时也是空串，
  // 于是调用方拿到的是"空清单 + 空理由"，它自己会把它说成"没有摄像头"。
  if (err) *err = e;
  return out;
}

WebcamSource::~WebcamSource() { close(); }

bool WebcamSource::open(std::string& err) {
  // **重入保护**：同一个对象再 open 一次（配置改了、网页上点了保存）不该泄漏上一个
  // reader。更要紧的是：两台 reader 同时抓一台摄像头，第二个拿到的是 E_ACCESSDENIED
  // —— 而那个错看起来像"摄像头坏了"，不像"我们自己开重了"。
  close();

  // **这里不调 `CoInitializeEx`。** 实测（out\ae-probe\com_check.cpp）：一条完全
  // 没有初始化过 COM 的线程，从 MFStartup 一路做到 ReadSample 拿到真帧，全程成功
  // —— 设备源这条路上没有需要套间的东西。而"顺手 CoInitializeEx 一下"在这个位置
  // 是**有害**的：这条线程不归我们管（是采集会话的线程），它可能已经被别人按别的
  // 套间模型初始化过，而我们一旦在 `close()` 里配对 `CoUninitialize`，就是替别人
  // 解掉他的初始化 —— `audio/audio_device.h` 里那段"别人的初始化不能替人解"
  // 说的正是这件事。

  MfPlatform& platform = mf_platform();
  if (!platform.ok) {
    err = platform.err;
    return false;
  }

  std::string derr;
  std::vector<DeviceEntry> devs = collect_devices(derr);
  if (devs.empty()) {
    err = derr.empty() ? "本机没有找到任何摄像头（设备管理器里看看是不是被禁用了）"
                       : derr;
    return false;
  }

  std::size_t index = 0;
  if (!resolve_device(devs, selector_, index, err)) return false;
  // `devs` 活到函数末尾，而 `activate` 是里面的一份引用 —— 下面一路都用它。
  name_ = devs[index].name;
  link_ = devs[index].link;
  D2O_INFO("目标摄像头：#%zu %s（%s）", index, name_.c_str(), link_.c_str());

  // --- 1. 设备的源对象 ---------------------------------------------------
  ComPtr<IMFMediaSource> src;
  HRESULT hr = devs[index].activate->ActivateObject(
      __uuidof(IMFMediaSource), reinterpret_cast<void**>(src.put()));
  if (FAILED(hr)) {
    // 打不开设备的两个原因都在这条路径上：被别的程序占着（E_ACCESSDENIED），
    // 或者设备被拔了/驱动重启过 —— 人话在 `mf_hr_text` 里。
    err = fmt("打不开摄像头「%s」：%s（%s）", name_.c_str(), mf_hr_text(hr).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return false;
  }

  // --- 2. reader 的属性表：一条，但它是整条路的开关 ------------------------
  //
  // 不加这条属性，请求 RGB32 直接 MF_E_INVALIDMEDIATYPE（实测 0xc00d36b4）—— 因为
  // 这台摄像头原生只给 NV12 / MJPG / YUY2。加上它，MF 会在中间插一个 Video
  // Processor MFT 替我们做色彩转换，于是**我们一行像素转换代码都不用写**，而且
  // 它给出的 BGRA 与 DXGI 桌面那条路**同字节序、同行宽**（实测首字节 54 2d 2a ff
  // = B,G,R,A），下游的 YUV 转换、水印、快照一行都不用改。
  ComPtr<IMFAttributes> ratts;
  hr = ::MFCreateAttributes(ratts.put(), 1);
  if (FAILED(hr)) {
    err = "建不出 SourceReader 的属性表（MFCreateAttributes " +
          hresult_hex(static_cast<long>(hr)) + "）";
    return false;
  }
  ratts->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);

  // --- 3. reader（它**拥有** src，见下面那条警告）--------------------------
  //
  // ⚠️ **绝不能**在这里设 `MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN`。
  // reader 的默认行为是"我释放的时候顺手把拿到的 source `Shutdown()` 掉"，而我们
  // 要的**正是**这个：一个通道一个 reader，reader 就是这一路摄像头的会话，`close()`
  // 时才放（见 webcam_capture.h 第 ④ 条）。设上那个标志等于把 ownership 拆开，
  // 于是关闭顺序稍有不慎就会留下一个半死不活的 source，而现场症状是"关掉再打开
  // 就报 0xc00d3e85"—— 看起来像设备的毛病。
  ComPtr<IMFSourceReader> reader;
  hr = ::MFCreateSourceReaderFromMediaSource(src.get(), ratts.get(), reader.put());
  if (FAILED(hr)) {
    err = fmt("建不出摄像头的读取器：%s（%s）", mf_hr_text(hr).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return false;
  }

  // --- 4. 协商输出格式：RGB32，**不钉尺寸** -------------------------------
  //
  // 只写两项：主类型和子类型。**不写尺寸、不写帧率** —— 写了就是"我来选档"，而
  // 那些档位是摄像头特有的（本机这台是 1920x1080 / 1280x720 / …六档）。不写，
  // MF 就挑它默认的那一档，也就是用户在系统相机程序里看到的那一档。
  ComPtr<IMFMediaType> want;
  hr = ::MFCreateMediaType(want.put());
  if (FAILED(hr) || !want) {
    // 不看这个返回值的话，`want` 是空指针，下一行就是空指针解引用 —— 失败方式
    // 从"报一句话"变成"崩在 open() 里"，而 open() 是每个通道都要走的。
    err = "建不出媒体类型（MFCreateMediaType " +
          hresult_hex(static_cast<long>(hr)) + "）";
    return false;
  }
  want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
  want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
  hr = reader->SetCurrentMediaType(kVideoStream, nullptr, want.get());
  if (FAILED(hr)) {
    err = fmt("摄像头不接受 RGB32 输出：%s（%s）", mf_hr_text(hr).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return false;
  }

  // --- 5. 回读协商结果 ---------------------------------------------------
  //
  // **必须回读，不能拿上面那份请求当结果**：我们请求里根本没写尺寸，真正的尺寸
  // 只有摄像头回答了之后才知道。
  ComPtr<IMFMediaType> cur;
  hr = reader->GetCurrentMediaType(kVideoStream, cur.put());
  if (FAILED(hr) || !cur) {
    err = fmt("拿不到摄像头协商后的格式：%s", mf_hr_text(hr).c_str());
    return false;
  }
  UINT32 w = 0;
  UINT32 h = 0;
  UINT32 num = 0;
  UINT32 den = 0;
  ::MFGetAttributeSize(cur.get(), MF_MT_FRAME_SIZE, &w, &h);
  ::MFGetAttributeRatio(cur.get(), MF_MT_FRAME_RATE, &num, &den);
  if (w == 0 || h == 0) {
    err = "摄像头报了一个 0 尺寸的输出格式";
    return false;
  }

  width_ = static_cast<int>(w);
  height_ = static_cast<int>(h);
  fps_num_ = num;
  fps_den_ = den;
  source_ = std::move(src);
  reader_ = std::move(reader);
  describe_ = fmt("%s（%ux%u @ %s，Media Foundation）", name_.c_str(), w, h,
                  fps_text(num, den).c_str());
  D2O_INFO("摄像头已打开：%s", describe_.c_str());
  return true;
}

std::string WebcamSource::describe() const {
  if (is_open()) return describe_;
  // 还没打开：能说多少说多少。**不知道是哪一台就别装作知道** —— selector 为空时
  // "第一台"是打开那一刻才确定的，而那台机器上插着几台、插的是哪几台都会变。
  // 这里编一个名字出来的话，日志和 `/api/status` 上就会挂着上一次的那台设备名。
  if (!selector_.empty()) return fmt("摄像头「%s」（还没打开）", selector_.c_str());
  return "摄像头（还没打开；没配设备名 = 用枚举到的第一台）";
}

bool WebcamSource::reopen(std::string& err) {
  // 与 `DisplaySource::reopen()` 的区别值得说清楚：显示器那一路能**留着 D3D 设备**、
  // 只重建 DXGI 的复制会话，所以它的 reopen 比 open 便宜得多；摄像头这条路上没有
  // 这一层 —— MF 的设备源本身就是会话，reader 一释放 source 就被 Shutdown（实测
  // 0xc00d3e85），根本没有"留着设备只换会话"的中间态。所以这里就是关掉再打开。
  //
  // 退避**不在这里做**：调用方（采集会话）已经有一份"150 ms 起、翻倍、封顶"的
  // 重试节奏，而且它还负责"重建成功之后尺寸变了就重启整条链"。在这里再加一层睡眠
  // 只会把两层退避叠起来，症状是插回摄像头之后要等十几秒才恢复。
  //
  // 这里不再显式 `close()`：`open()` 自己第一件事就是 close（重入保护），关两次
  // 也无所谓（它是幂等的），但少一次调用少一个"将来有人把 open 里那句删掉"的机会。
  return open(err);
}

void WebcamSource::close() {
  // **顺序：先 reader 后 source。** reader 的 `Release()` 会对它拿到的 source 调
  // `Shutdown()`（MF 的默认行为，也是 mf_probe2 里那个 0xc00d3e85 的来源）。反过来
  // 先放 source 不会崩，但那时 reader 手里那份引用还在，等于把一个已经没人管的源
  // 留着 —— 而它下次 ActivateObject 就是"设备被占用"。
  reader_.reset();
  source_.reset();

  // **幂等**：重复调用、以及 `open()` 失败之后调用，都只是把空指针再置空一次，
  // 顺便把上一次成功时留下的尺寸/描述清干净 —— 否则 `describe()` 会拿着一张
  // 已经不存在的会话的尺寸说话。
  width_ = 0;
  height_ = 0;
  fps_num_ = 0;
  fps_den_ = 0;
  describe_.clear();
  name_.clear();
  link_.clear();
  // 底图也丢掉：它是**那一次会话**的画面，会话没了它就过期了，而且尺寸可能是
  // 另一台摄像头的。留着的话 `reemit()` 会交出一张别的设备的旧帧。
  last_ = BgraFrame{};
  have_last_ = false;
  logged_first_frame_ = false;
}

SourceStatus WebcamSource::acquire(std::uint32_t timeout_ms, BgraFrame& dst,
                                   bool& changed, std::string& err) {
  changed = false;
  if (!reader_) {
    err = "摄像头还没打开（open() 没成功过）";
    return SourceStatus::Fatal;
  }

  // 等到什么程度为止。**这个上限是"软"的**：`ReadSample` 是同步阻塞的，MF 没有
  // 给它超时参数（DXGI 的 `AcquireNextFrame` 有）。摄像头正常时它每次最多阻塞一帧
  // 的时长（30fps = 33 ms）；真正会长时间不给帧的只有"设备还在、但没有数据"那种，
  // 而那时 MF 用 STREAMTICK 把我们放回来（见下面）。
  auto const deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

  ComPtr<IMFSample> sample;
  for (;;) {
    DWORD flags = 0;
    LONGLONG ts = 0;
    HRESULT const hr = reader_->ReadSample(kVideoStream, 0, nullptr, &flags, &ts,
                                           sample.put());
    if (FAILED(hr)) {
      // 失败也要**分类**：能"重建就好"的（拔了 / 被抢了 / 会话已关）报 AccessLost，
      // 交给采集会话那条已经写好的退避重建；其余的才是真故障。
      err = fmt("读取摄像头画面失败：%s（%s）", mf_hr_text(hr).c_str(),
                hresult_hex(static_cast<long>(hr)).c_str());
      return classify_read_failure(hr);
    }
    if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
      // 设备源给 EOS 不等于"录完了"（摄像头流没有尽头这回事），它等于**设备没了**
      // —— 多半是拔线。按会话失效处理：插回来的话重建之后接着录。
      err = "摄像头没有数据了（多半是拔掉了）";
      return SourceStatus::AccessLost;
    }
    if (flags & MF_SOURCE_READERF_ERROR) {
      err = "摄像头报了一个错误（多半是被别的程序抢走了）";
      return SourceStatus::AccessLost;
    }
    if (sample) break;

    // 走到这里 = 空样本（STREAMTICK）：**"还没有内容"，不是一个错误**。
    // 这条路真的会发生，不是理论：刚打开时的第一次 ReadSample 返回的就是
    // STREAMTICK（flags=0x100、无样本，见 out\ae-probe\com_check.cpp 的输出）。
    if (std::chrono::steady_clock::now() >= deadline) return SourceStatus::Timeout;
    // 没到点就再要一次。**不立刻返回 Timeout**：一次 STREAMTICK 只说明"这一拍没有
    // 内容"，而通道的一拍只有 33 ms，立刻返回会让采集循环在这一拍里空转几百次
    // （每次都要重新走一遍上面的判定、并且每次都拿同一个空样本回来）。
  }

  // --- 取出像素 -----------------------------------------------------------
  ComPtr<IMFMediaBuffer> buf;
  HRESULT hr = sample->ConvertToContiguousBuffer(buf.put());
  if (FAILED(hr)) {
    err = fmt("拿不到摄像头这一帧的缓冲：%s", mf_hr_text(hr).c_str());
    return SourceStatus::Fatal;
  }
  BYTE* p = nullptr;
  DWORD len = 0;
  hr = buf->Lock(&p, nullptr, &len);
  if (FAILED(hr)) {
    err = fmt("锁不住摄像头这一帧的缓冲：%s", mf_hr_text(hr).c_str());
    return SourceStatus::Fatal;
  }

  // 行宽**只能从缓冲区长度推**（webcam_capture.h 第 ③ 条）：这台摄像头对任何格式
  // 都不报 `MF_MT_DEFAULT_STRIDE`（实测 present=0、值 0），读它得到的是 0，算出来
  // 的每个像素都落在第 0 行上 —— 画面斜着糊成一条，而代码里看起来"我读了官方给的
  // 行宽字段"。判据：`pitch = 字节数 / 高度`，并要求 `pitch >= 4*宽度`。
  std::size_t const packed = 4u * static_cast<std::size_t>(width_);
  std::size_t const pitch = len / static_cast<std::size_t>(height_);
  if (pitch < packed || len < packed * static_cast<std::size_t>(height_)) {
    buf->Unlock();
    err = fmt("摄像头这一帧只有 %lu 字节，装不下 %dx%d 的 BGRA（要 %zu 字节）——"
              "协商出来的尺寸和实际缓冲对不上",
              static_cast<unsigned long>(len), width_, height_,
              packed * static_cast<std::size_t>(height_));
    return SourceStatus::Fatal;
  }

  // 拷进 dst。**输出一律紧凑**（stride = 4*宽度）：摄像头万一给了带填充的行
  // （pitch > 4*宽度），这里把填充**剥掉**。理由不是省内存：dst 是帧缓冲池那块
  // 按 stride=4*宽度 分配的内存，把 pitch 原样报上去，后面的水印合成会按错误的
  // 行距写像素 —— 画出来是斜的，而且**写到缓冲外面**（8 MB 的越界写，症状是
  // 随机的崩溃或画面里出现别人的内存）。
  dst.width = width_;
  dst.height = height_;
  dst.stride = static_cast<int>(packed);
  std::size_t const need = packed * static_cast<std::size_t>(height_);
  if (dst.pixels.size() < need) dst.pixels.resize(need);
  if (pitch == packed) {
    // 实测就是这一种：1920x1080 给的缓冲恰好 8294400 = 4*1920*1080，没有行填充，
    // 所以正常路径上就是**一次**整块 memcpy。
    std::memcpy(dst.pixels.data(), p, need);
  } else {
    for (int y = 0; y < height_; ++y) {
      std::memcpy(dst.pixels.data() + packed * static_cast<std::size_t>(y),
                  p + pitch * static_cast<std::size_t>(y), packed);
    }
  }
  buf->Unlock();

  // 首帧的实测数字报一次。这几行是"画面斜着/颜色不对"这类问题的**唯一**现场证据：
  // 只有把缓冲字节数和推出的行宽摆出来，才能一眼看出是"摄像头给的行带填充"还是
  // "我们算错了"。每帧都报就是每秒三十行，所以只报一次。
  if (!logged_first_frame_) {
    logged_first_frame_ = true;
    D2O_INFO("摄像头首帧：%dx%d，缓冲 %lu 字节 → 行宽 %zu（%s）", width_, height_,
             static_cast<unsigned long>(len), pitch,
             pitch == packed ? "紧凑，无行填充" : "带行填充，已剥掉");
  }

  // content_id **不在这里碰**。它是"内容版本号"，由**采集会话**在把帧交给编码线程
  // 之前统一递增（capture_session.cpp：`buf->content_id = ++content_id`）—— 源这一侧
  // 根本不知道水印有没有画上去，而我们刚写满的 dst 下一刻就会被画上水印。由源来定
  // 这个号，等于让"还没画水印的版本"和"画完的版本"共用一个号，编码器就会把带新水印
  // 的那一帧当成"内容没变"而复用旧的 YUV（水印上的秒就此冻住）。

  // 留一份**没有烧过水印**的原始帧给 `reemit()`（见那里的说明）。代价是每帧多一次
  // 整块 memcpy：1080p@30 就是 240 MB/s 的内存带宽 —— 这是"水印配置改了之后还能
  // 画对"所付的代价，而它买到的是整个通道不至于报出一个画着旧水印的画面。
  last_ = dst;
  have_last_ = true;

  // 摄像头的一帧**永远是"新内容"**：桌面那边"静止"的局面（没有指针可动、没有像素
  // 变化）在这里不存在，摄像头自己就在按它的帧率给帧，而且传感器的噪声让任意两帧
  // 都不可能逐字节相同。报 changed=false 的后果是编码器复用上一帧的 YUV，而画面
  // 其实变了 —— 那正是"画面卡住不动"的成因，并且它只在特定内容下才出现。
  changed = true;
  return SourceStatus::Ok;
}

SourceStatus WebcamSource::reemit(BgraFrame& dst) {
  // 会话要这个，是因为"画面没动、但画上去的东西要变"（水印配置改了）。摄像头的
  // 画面**永远不会**静止 —— 但那是**我们会话**知道的事，`IFrameSource` 的调用方
  // 不知道（它只看到 changed=true/false，而接口上"没有新内容"这件事对我们并不
  // 成立）。所以这一路照样要实现：会话那边的兜底是"把上一帧拷过来重画水印"，而
  // 上一帧里**已经烧着旧水印**了，照着重画只会把旧的又画一遍 —— 症状是"水印改了
  // 没反应、关也关不掉"。这里交出来的那一份是**从摄像头直接拷下来、没有水印的**，
  // 重画之后才是对的。
  if (!have_last_) return SourceStatus::Timeout;
  dst.width = last_.width;
  dst.height = last_.height;
  dst.stride = last_.stride;
  if (dst.pixels.size() < last_.pixels.size()) dst.pixels.resize(last_.pixels.size());
  std::memcpy(dst.pixels.data(), last_.pixels.data(), last_.pixels.size());
  // content_id 照样不动（和 acquire 一样，由会话统一递增）。
  return SourceStatus::Ok;
}

// `make_frame_source` 那一边的入口。**声明在 `frame_source.h`、定义在这里**
// 是刻意的：声明放那边让 `frame_source.cpp` 能调它而**不必把 mfidl.h /
// mfreadwrite.h 拖进来**，而这两个头文件和 `windows.h` 的包含顺序很挑（见
// 本文件顶上那几条实测约束）。定义放这里是因为只有这里认识 `WebcamSource`。
//
// 传值不是笔误：selector 就是一层"用户写的字符串"，拷进来之后各走各的。
std::unique_ptr<IFrameSource> make_webcam_source(std::string selector) {
  return std::unique_ptr<IFrameSource>(new WebcamSource(std::move(selector)));
}

}  // namespace d2o
