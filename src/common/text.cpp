#include "common/text.h"

#include <cstdio>

#ifdef _WIN32
#  include <windows.h>
#endif

namespace d2o {

std::string to_utf8(wchar_t const* w, int len) {
  if (!w) return std::string();
#ifdef _WIN32
  // len < 0 表示"以空字符结尾"，这时**必须自己先量长度**再传。
  // 直接传 -1 给 WideCharToMultiByte 会让它把结尾的空字符也转换进去并计入
  // 返回值，于是结果串里多一个内嵌的 '\0'。这种串用 c_str() 打印看不出来，
  // 但拿去做 `+=` 拼接时会**在那里截断** —— 表现为名字后面少一截，
  // 而单独打印那个名字又是好的，非常难查。
  if (len < 0) len = static_cast<int>(::wcslen(w));
  if (len == 0) return std::string();
  int const n = ::WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
  if (n <= 0) return std::string();
  std::string s(static_cast<std::size_t>(n), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
  return s;
#else
  // 非 Windows 只是为了让这个文件也能编过（工具链检查用的），不做真转换。
  (void)len;
  std::string s;
  for (wchar_t const* p = w; *p; ++p) s.push_back(static_cast<char>(*p & 0x7F));
  return s;
#endif
}

std::string hresult_hex(long hr) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
  return buf;
}

std::string hresult_text(long hr) {
  switch (static_cast<unsigned long>(hr)) {
    // --- 桌面复制专有 ---------------------------------------------------
    case 0x887A0026UL:
      return "DXGI_ERROR_ACCESS_LOST —— 捕获会话失效（分辨率切换、锁屏、"
             "显示器热插拔、切换用户）。这不是崩溃，重建会话即可继续";
    case 0x887A0027UL:
      return "DXGI_ERROR_WAIT_TIMEOUT —— 超时内桌面没有更新。正常情况，不是错误";
    case 0x887A0022UL:
      return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE —— 该输出上的复制会话已被占满"
             "（同一进程内每个输出最多 4 个，且可能被别的录屏软件独占）";
    case 0x887A0028UL:
      return "DXGI_ERROR_SESSION_DISCONNECTED —— 当前是远程桌面会话且已断开。"
             "DXGI 桌面复制在断开的 RDP 会话里抓不到画面";
    case 0x887A0004UL:
      return "DXGI_ERROR_UNSUPPORTED —— 这个输出不支持桌面复制。"
             "间接显示驱动（IDD，如向日葵/Parsec/usbmmidd 的虚拟屏）返回的就是它";
    case 0x887A0001UL:
      return "DXGI_ERROR_INVALID_CALL —— 调用参数或调用时机不对（例如重复 "
             "AcquireNextFrame 而没有 ReleaseFrame）";
    case 0x887A0005UL:
      return "DXGI_ERROR_DEVICE_REMOVED —— 显卡设备已移除或驱动重置";
    case 0x887A0006UL:
      return "DXGI_ERROR_DEVICE_HUNG —— 显卡无响应";
    case 0x887A0007UL:
      return "DXGI_ERROR_DEVICE_RESET —— 显卡驱动被重置";

    // --- 通用 -----------------------------------------------------------
    case 0x80070057UL:
      return "E_INVALIDARG —— 参数非法";
    case 0x80070005UL:
      return "E_ACCESSDENIED —— 拒绝访问";
    case 0x8007000EUL:
      return "E_OUTOFMEMORY —— 内存不足";
    case 0x80004005UL:
      return "E_FAIL —— 未指明的失败";
    case 0x8007007EUL:
      return "ERROR_MOD_NOT_FOUND —— 找不到指定的模块";
    case 0x80040154UL:
      return "REGDB_E_CLASSNOTREG —— 类未注册";
    case 0x00000000UL:
      return "S_OK";
    default:
      break;
  }
  return "未知 HRESULT " + hresult_hex(hr);
}

}  // namespace d2o
