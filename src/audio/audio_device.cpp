#include "audio/audio_device.h"

#include <windows.h>

#include <mmdeviceapi.h>
#include <propsys.h>

#include <cstdio>

#include "common/com_ptr.h"
#include "common/log.h"   // d2o::fmt
#include "common/text.h"

namespace d2o {

ComApartment::ComApartment() {
  HRESULT const hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  hr_ = static_cast<long>(hr);
  if (SUCCEEDED(hr)) {
    owns_ = true;
    ok_ = true;
  } else if (hr == RPC_E_CHANGED_MODE) {
    // 这个线程已经是 STA 了（别人初始化的）。能用，只是不归我们解。
    ok_ = true;
  }
}

ComApartment::~ComApartment() {
  if (owns_) ::CoUninitialize();
}

namespace {

/// PROPERTYKEY 的**定义**在 msys64 里一个都找不到：`functiondiscoverykeys_devpkey.h`
/// 用 `DEFINE_PROPERTYKEY` 声明，而它在没有 INITGUID 时展开成 `EXTERN_C const
/// PROPERTYKEY name`（裸声明）。官方做法是在某一个 TU 里定义 INITGUID 再 include
/// 那个头 —— 但那样一次会**物化那个头里的两百多个 PKEY**，而且 `DECLSPEC_SELECTANY`
/// 在 GCC 下是 `__declspec(selectany)`，靠它去兜多 TU 的重复定义是在赌编译器。
///
/// 这里只写我们**真的用到**的两个，字节值抄自 `functiondiscoverykeys_devpkey.h`
/// （同一行的 pid 也抄下来）。常量放在匿名命名空间里 = 内部链接，
/// 不存在和别人的定义撞车的问题。
///
/// 抄错的话症状是"设备名是空的"而不是崩溃 —— 所以 `--list-audio` 的验收
/// 必须断言**名字非空**，只断言"列出了几个设备"是拦不住这个的。
constexpr PROPERTYKEY kPkeyFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr PROPERTYKEY kPkeyDeviceDesc = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};

std::string hr_text(HRESULT hr) {
  return fmt("0x%08lX", static_cast<unsigned long>(hr));
}

/// 读一个字符串属性。读不到返回 false（**不是错误**：有些驱动的
/// FriendlyName 是空的，那是驱动的事，不该让整次枚举失败）。
bool read_string(IPropertyStore* props, PROPERTYKEY const& key, std::string& out) {
  PROPVARIANT pv;
  ::PropVariantInit(&pv);
  bool got = false;
  if (SUCCEEDED(props->GetValue(key, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal != nullptr) {
    out = to_utf8(pv.pwszVal, -1);
    got = !out.empty();
  }
  ::PropVariantClear(&pv);
  return got;
}

/// FriendlyName 优先，没有就退到 DeviceDesc。
///
/// 两个都要试：FriendlyName 是"扬声器 (Realtek(R) Audio)"这种给人看的，
/// 而**虚拟声卡（VB-Cable 那类）经常只填 DeviceDesc**，只读 FriendlyName
/// 的话它们在列表里是一行空白 —— 而那正好是最需要被选中的那类设备。
std::string endpoint_display_name(IPropertyStore* props) {
  std::string name;
  if (read_string(props, kPkeyFriendlyName, name)) return name;
  if (read_string(props, kPkeyDeviceDesc, name)) return name;
  return std::string();
}

}  // namespace

std::vector<AudioEndpoint> list_audio_endpoints(AudioFlow flow, std::string& err) {
  err.clear();
  std::vector<AudioEndpoint> out;

  ComApartment com;
  if (!com.ok()) {
    err = "这个线程的 COM 起不来（CoInitializeEx " + hr_text(static_cast<HRESULT>(com.hr())) + "）";
    return out;
  }

  ComPtr<IMMDeviceEnumerator> en;
  HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void**>(en.put()));
  if (FAILED(hr)) {
    err = "拿不到设备枚举器（CoCreateInstance " + hr_text(hr) + "）";
    return out;
  }

  // 先问默认设备。**不能靠"列表里第一个"代替**：EnumAudioEndpoints 的顺序
  // 没有承诺，而"默认"是一件真事（用户在系统设置里选的那个）。
  std::string default_id;
  {
    ComPtr<IMMDevice> dev;
    EDataFlow const df = (flow == AudioFlow::Render) ? eRender : eCapture;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(df, eConsole, dev.put())) && dev) {
      LPWSTR wid = nullptr;
      if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
        default_id = to_utf8(wid, -1);
        ::CoTaskMemFree(wid);
      }
    }
  }

  ComPtr<IMMDeviceCollection> col;
  hr = en->EnumAudioEndpoints((flow == AudioFlow::Render) ? eRender : eCapture,
                              DEVICE_STATE_ACTIVE, col.put());
  if (FAILED(hr)) {
    err = "枚举音频端点失败（" + hr_text(hr) + "）";
    return out;
  }

  UINT count = 0;
  if (FAILED(col->GetCount(&count))) return out;

  for (UINT i = 0; i < count; ++i) {
    ComPtr<IMMDevice> dev;
    if (FAILED(col->Item(i, dev.put())) || !dev) continue;

    AudioEndpoint ep;
    LPWSTR wid = nullptr;
    if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
      ep.id = to_utf8(wid, -1);
      ::CoTaskMemFree(wid);
    }
    if (ep.id.empty()) continue;  // 没有 id 就没法在配置里存，列出来也没用

    ComPtr<IPropertyStore> props;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, props.put())) && props) {
      ep.name = endpoint_display_name(props.get());
    }
    if (ep.name.empty()) ep.name = "（未命名设备）";

    ep.is_default = (!default_id.empty() && ep.id == default_id);
    // 环回只对渲染端点成立。**不按设备类型去猜** —— 有些虚拟声卡两种端点都有，
    // 而"这个渲染端点能不能环回"的答案恒为是。
    ep.loopback_ok = (flow == AudioFlow::Render);
    out.push_back(std::move(ep));
  }
  return out;
}

std::string default_endpoint_id(AudioFlow flow, std::string& err) {
  err.clear();
  ComApartment com;
  if (!com.ok()) {
    err = "这个线程的 COM 起不来（CoInitializeEx " + hr_text(static_cast<HRESULT>(com.hr())) + "）";
    return std::string();
  }
  ComPtr<IMMDeviceEnumerator> en;
  HRESULT const hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void**>(en.put()));
  if (FAILED(hr)) {
    err = "拿不到设备枚举器（CoCreateInstance " + hr_text(hr) + "）";
    return std::string();
  }
  ComPtr<IMMDevice> dev;
  EDataFlow const df = (flow == AudioFlow::Render) ? eRender : eCapture;
  HRESULT const h2 = en->GetDefaultAudioEndpoint(df, eConsole, dev.put());
  if (FAILED(h2)) {
    // **不是错误**：这台机器没有任何播放/录音设备。空串就是答案。
    return std::string();
  }
  LPWSTR wid = nullptr;
  std::string id;
  if (SUCCEEDED(dev->GetId(&wid)) && wid != nullptr) {
    id = to_utf8(wid, -1);
    ::CoTaskMemFree(wid);
  }
  return id;
}

std::string endpoint_name_by_id(AudioFlow flow, std::string const& id) {
  if (id.empty()) return std::string();
  std::string err;
  for (auto const& ep : list_audio_endpoints(flow, err)) {
    if (ep.id == id) return ep.name;
  }
  return std::string();
}

}  // namespace d2o
