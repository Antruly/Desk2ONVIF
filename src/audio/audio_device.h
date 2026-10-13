#pragma once

/**
 * @file src/audio/audio_device.h
 * @brief 枚举系统上的音频端点（扬声器 / 麦克风）。
 *
 * 只做"有哪些、叫什么、哪个是默认的"，不碰采集。`--list-audio` 和
 * `GET /api/audio/devices` 共用这一份，免得两个界面对设备的称呼不一致。
 *
 * **不定义 MMDevice 的那几个 GUID。** 直觉上需要：`guiddef.h` 在 GCC 下把
 * `DEFINE_GUID` 展开成**裸声明**（注释原文写着 must not be used here），
 * 而 msys64 的 `lib\*.a` 里也确实没有 `CLSID_MMDeviceEnumerator` 这些符号。
 * 但那只是"别去引用符号"——mingw 的 `_mingw.h` 有一套 `__uuidof` 的模板模拟
 * （`__CRT_UUID_DECL`），而 `mmdeviceapi.h` / `audioclient.h` 对我们需要的
 * 每个接口**都写了那一行**，所以 `__uuidof(IMMDeviceEnumerator)` 是**编译期
 * 常量**，一个符号都不引用。已实测：只链 `-lole32`（**不给 `-luuid`**）
 * 就能 `CoCreateInstance` 成功并枚举出端点。
 *
 * 手抄 GUID 字节是这条路看起来唯一的选择，但它同时是"抄错一个字节就静默
 * 拿不到设备"这种最难查的缺陷 —— 能用 `__uuidof` 就别抄。
 */

#include <string>
#include <vector>

namespace d2o {

/// 端点方向。
///
/// 分这两个不是审美：**环回（"系统声音"）只对渲染端点成立**，而麦克风是
/// 采集端点。把两者混成一个列表，用户会在麦克风那一栏里选到一个扬声器，
/// 然后得到一个永远静音的音轨 —— 而 SDP 和 ffprobe 都一切正常。
enum class AudioFlow {
  Render,   ///< 播放端点 —— "系统声音"从这一侧环回采
  Capture,  ///< 录音端点 —— 麦克风
};

struct AudioEndpoint {
  std::string id;    ///< IMMDevice::GetId()，跨重启稳定的标识
  std::string name;  ///< 友好名（"扬声器 (Realtek(R) Audio)" 这种）
  bool is_default = false;
  /// 这个端点能不能做环回采集。渲染端点恒 true，采集端点恒 false。
  bool loopback_ok = false;
};

/// 枚举当前**活跃**（DEVICE_STATE_ACTIVE）的端点。
///
/// 失败时返回空 vector 并填 `err`（带 HRESULT，不然"没设备"和"COM 没起来"
/// 长得一模一样）。**"一个设备都没有"不算失败**：那是一台没有声卡的机器，
/// 返回空列表、err 为空，由调用方决定怎么说这句话。
std::vector<AudioEndpoint> list_audio_endpoints(AudioFlow flow, std::string& err);

/// 默认端点的 id。没有默认端点时返回空串（err 为空）。
std::string default_endpoint_id(AudioFlow flow, std::string& err);

/// 按 id 找端点的友好名。找不到返回空串 —— 调用方拿它只是为了显示。
std::string endpoint_name_by_id(AudioFlow flow, std::string const& id);

/// 按当前线程进入 MTA。
///
/// **每个碰 COM 的线程都得有一个**：`CoInitializeEx` 是按线程的，而我们的音频
/// 采集跑在一条自己的线程上，不是主线程。
///
/// 用 MTA 而不是 STA：这里没有窗口消息要泵，而 STA 的话每次跨线程调用都要
/// Marshal。`RPC_E_CHANGED_MODE`（这个线程已经被别人初始化成 STA 了）**不当
/// 失败** —— 那就用别人那份，只是析构时不能去 `CoUninitialize`：那是**别人的**
/// 初始化，我们把它解掉就是替别人做决定。
class ComApartment {
 public:
  ComApartment();
  ~ComApartment();
  ComApartment(ComApartment const&) = delete;
  ComApartment& operator=(ComApartment const&) = delete;
  bool ok() const { return ok_; }
  long hr() const { return hr_; }

 private:
  bool owns_ = false;
  bool ok_ = false;
  long hr_ = 0;
};

}  // namespace d2o
