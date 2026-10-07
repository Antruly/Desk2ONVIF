#pragma once

/**
 * @file src/common/text.h
 * @brief 字符串与错误码的小工具。
 */

#include <string>

namespace d2o {

/// Windows 的宽字符串 → UTF-8。
///
/// 显卡名、显示器设备名都是从 DXGI 拿的宽字符，可能带中文（"英特尔(R) 超核芯
/// 显卡"）。项目内部一律用 UTF-8 的 std::string，只在调 Windows API 的边界上
/// 转成宽字符 —— 所以这里只做向内的那一个方向。
std::string to_utf8(wchar_t const* w, int len = -1);

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
