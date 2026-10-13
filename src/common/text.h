#pragma once

/**
 * @file src/common/text.h
 * @brief 字符串与错误码的小工具。
 */

#include <ctime>
#include <string>

namespace d2o {

/// Windows 的宽字符串 → UTF-8。
///
/// 显卡名、显示器设备名都是从 DXGI 拿的宽字符，可能带中文（"英特尔(R) 超核芯
/// 显卡"）。项目内部一律用 UTF-8 的 std::string，只在调 Windows API 的边界上
/// 转成宽字符。
std::string to_utf8(wchar_t const* w, int len = -1);

/// UTF-8 → Windows 宽字符串，`to_utf8` 的反方向。
///
/// 需要它的只有 GDI 的 `TextOutW`（水印里可能有中文的摄像头名称）。**不能靠
/// ANSI 代码页**：同一份 UTF-8 名字在 936 和 65001 的机器上会得到不同的字形，
/// 而在 65001 之外的机器上中文直接变成乱码 —— 并且只在那台机器上出现。
std::wstring from_utf8(std::string const& s);

/// `time_t` → **本地**时间的 `std::tm`。
///
/// 包一层是因为这个转换在两个平台上叫不同的名字（`localtime_s` /
/// `localtime_r`）**且参数顺序相反**；而标准的 `std::localtime` 返回的是静态
/// 缓冲区，多线程下会互相踩 —— 这里有两三个线程同时在跑，不能用它。
std::tm local_time(std::time_t t);

/// HRESULT → 人能看懂的一句话。
///
/// 不用 FormatMessage：它给 DXGI 的错误码返回的多半是
/// "The GPU device instance has been suspended" 这类和实际场景对不上的通用文案，
/// 而 0x887A0027（DXGI_ERROR_WAIT_TIMEOUT）根本没有消息文本。
/// 这里手写一张表，覆盖桌面复制实际会遇到的那几个，其余退回十六进制。
std::string hresult_text(long hr);

/// HRESULT 的十六进制形式，写日志时附在文字后面用。
std::string hresult_hex(long hr);

}  // namespace d2o
