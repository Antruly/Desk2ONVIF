#include "capture/display_info.h"

#include "common/com_ptr.h"
#include "common/log.h"
#include "common/text.h"

#include <cstdio>

#include <d3d11.h>

namespace d2o {

std::vector<OutputInfo> enumerate_outputs() {
  std::vector<OutputInfo> result;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.put()));
  if (FAILED(hr)) {
    D2O_ERROR("CreateDXGIFactory1 失败：%s（%s）",
              hresult_text(static_cast<long>(hr)).c_str(),
              hresult_hex(static_cast<long>(hr)).c_str());
    return result;
  }

  for (unsigned ai = 0;; ++ai) {
    ComPtr<IDXGIAdapter1> adapter;
    if (factory->EnumAdapters1(ai, adapter.put()) == DXGI_ERROR_NOT_FOUND) break;
    if (!adapter) break;

    DXGI_ADAPTER_DESC1 ad{};
    if (FAILED(adapter->GetDesc1(&ad))) continue;

    for (unsigned oi = 0;; ++oi) {
      ComPtr<IDXGIOutput> output;
      if (adapter->EnumOutputs(oi, output.put()) == DXGI_ERROR_NOT_FOUND) break;
      if (!output) break;

      DXGI_OUTPUT_DESC od{};
      if (FAILED(output->GetDesc(&od))) continue;

      OutputInfo info;
      info.adapter_index = ai;
      info.output_index = oi;
      info.adapter_name = to_utf8(ad.Description);
      info.device_name = to_utf8(od.DeviceName);
      info.desktop_rect = od.DesktopCoordinates;
      info.attached = od.AttachedToDesktop != FALSE;
      info.is_software = (ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
      info.vendor_id = ad.VendorId;
      info.device_id = ad.DeviceId;
      result.push_back(std::move(info));
    }
  }
  return result;
}

CaptureProbe probe_capturable(OutputInfo const& out) {
  CaptureProbe probe;

  ComPtr<IDXGIFactory1> factory;
  HRESULT hr = ::CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void**>(factory.put()));
  if (FAILED(hr)) {
    probe.hresult = static_cast<long>(hr);
    probe.message = "CreateDXGIFactory1 失败：" + hresult_text(probe.hresult);
    return probe;
  }

  ComPtr<IDXGIAdapter1> adapter;
  if (FAILED(factory->EnumAdapters1(out.adapter_index, adapter.put())) || !adapter) {
    probe.message = "枚举适配器失败（下标对不上？）";
    return probe;
  }

  ComPtr<IDXGIOutput> output;
  if (FAILED(adapter->EnumOutputs(out.output_index, output.put())) || !output) {
    probe.message = "枚举输出失败（下标对不上？）";
    return probe;
  }

  // 必须真的拿这块输出所属的适配器建 D3D 设备：用别的适配器建出来的设备
  // 去 DuplicateOutput 会返回 E_INVALIDARG，那是"用错了"不是"不支持"，
  // 会把这个探测变成误报。
  D3D_FEATURE_LEVEL const levels[] = {
      D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
      D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
  ComPtr<ID3D11Device> device;
  HRESULT const dhr = ::D3D11CreateDevice(
      adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels,
      static_cast<UINT>(sizeof levels / sizeof levels[0]), D3D11_SDK_VERSION,
      device.put(), nullptr, nullptr);
  if (FAILED(dhr)) {
    probe.hresult = static_cast<long>(dhr);
    probe.message = "D3D11CreateDevice 失败：" + hresult_text(probe.hresult);
    return probe;
  }

  ComPtr<IDXGIOutput1> output1;
  if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1),
                                    reinterpret_cast<void**>(output1.put()))) ||
      !output1) {
    probe.message = "该输出没有 IDXGIOutput1 接口（不到 Windows 8？）";
    return probe;
  }

  IDXGIOutputDuplication* dup = nullptr;
  HRESULT const hr2 = output1->DuplicateOutput(device.get(), &dup);
  if (dup) dup->Release();

  probe.hresult = static_cast<long>(hr2);
  probe.ok = SUCCEEDED(hr2);
  probe.message = probe.ok ? "可以捕获" : hresult_text(probe.hresult);
  return probe;
}

std::string describe(OutputInfo const& out) {
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s（%dx%d @ %ld,%ld）",
                out.device_name.c_str(), out.width(), out.height(),
                static_cast<long>(out.desktop_rect.left),
                static_cast<long>(out.desktop_rect.top));
  std::string s = out.adapter_name;
  s += " — ";
  s += buf;
  if (!out.attached) s += " [未接在当前桌面]";
  if (out.is_software) s += " [软件适配器]";
  return s;
}

}  // namespace d2o
