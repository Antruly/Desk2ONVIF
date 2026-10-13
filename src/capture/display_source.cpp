/**
 * @file src/capture/display_source.cpp
 * @brief 见 display_source.h 的说明。这里只有两个值得读的地方：那个不写 default
 *        的 switch（给未来的失败方式留一个编译期路口），和 reopen() 为什么不
 *        重新挑一遍输出。
 */

#include "capture/display_source.h"

#include "common/log.h"
#include "common/text.h"

namespace d2o {

bool DisplaySource::open(std::string& err) {
  std::vector<OutputInfo> outputs = enumerate_outputs();
  if (outputs.empty()) {
    err = "没有找到任何显示输出";
    return false;
  }
  if (want_ >= outputs.size()) {
    // 报错而不是退到 0 号输出。多通道下这一条是硬的：通道 3 指着不存在的屏，
    // 它就必须红着，而不是安静地去录主屏 —— 后者会让人以为配置生效了。
    err = fmt("显示输出 #%u 不存在，本机只有 %zu 个输出。用 --list-displays 看清单",
              want_, outputs.size());
    return false;
  }
  out_ = outputs[want_];
  // 显式写 `d2o::`：不写的话这个名字会解析到**本类的成员函数** `describe()`
  // （无参），于是变成一句"用 OutputInfo 调 describe()"—— 错误信息是
  // "no matching function for call"，看不出是名字查找的问题。
  describe_ = d2o::describe(out_);
  D2O_INFO("目标输出：#%u %s", want_, describe_.c_str());

  // 真的探一次。这一步很值得：本机装着向日葵，它的虚拟屏在 DXGI 上根本建不了
  // 复制会话，早点说清楚比录出十分钟黑屏好。
  CaptureProbe const probe = probe_capturable(out_);
  if (!probe.ok) {
    err = fmt("这个输出无法做桌面复制：\n    %s\n  输出：%s\n"
              "  用 --list-displays 看全部输出，选一个标着 [可捕获] 的换 --display 序号",
              probe.message.c_str(), describe_.c_str());
    return false;
  }

  return capture_.open(out_, err);
}

bool DisplaySource::reopen(std::string& err) {
  // **不重新挑一遍输出。** `DesktopCapture::reopen()` 自己会按 `device_name`
  // 把那台显示器找回来（找得到就顺手修正下标），找不到才退回按下标枚举。
  //
  // 在这里重新 `enumerate_outputs()` 反而是错的：显示器一插一拔，枚举顺序就
  // 可能整体错位，于是"重建会话"会变成"换一台显示器录"—— 而用户看到的只是
  // 画面突然变成了别人的桌面。`DesktopCapture` 里那段按名字核对的逻辑正是为了
  // 挡住这件事，绕过它就把那层保护丢了。
  return capture_.reopen(err);
}

void DisplaySource::close() { capture_.close(); }

SourceStatus DisplaySource::acquire(std::uint32_t timeout_ms, BgraFrame& dst,
                                    bool& changed, std::string& err) {
  switch (capture_.acquire(timeout_ms, dst, changed, err)) {
    case DesktopCapture::Status::Ok:
      return SourceStatus::Ok;
    case DesktopCapture::Status::Timeout:
      return SourceStatus::Timeout;
    case DesktopCapture::Status::AccessLost:
      return SourceStatus::AccessLost;
    case DesktopCapture::Status::Fatal:
      return SourceStatus::Fatal;
  }
  // **故意不写 default**（见 frame_source.h）：给 DesktopCapture::Status 加一个
  // 取值时，这一行会变成编译错误，逼着人回答新失败方式该映射成哪一个。
  // 写 default 的话它会静默落进兜底分支，而失败方式被归错类的症状是
  // "重试策略莫名其妙不对"，从现象上完全看不出是这里。
  err = "采集返回了一个本函数不认识的结局";
  return SourceStatus::Fatal;
}

SourceStatus DisplaySource::reemit(BgraFrame& dst) {
  switch (capture_.reemit(dst)) {
    case DesktopCapture::Status::Ok:
      return SourceStatus::Ok;
    case DesktopCapture::Status::Timeout:
      return SourceStatus::Timeout;
    case DesktopCapture::Status::AccessLost:
      return SourceStatus::AccessLost;
    case DesktopCapture::Status::Fatal:
      return SourceStatus::Fatal;
  }
  return SourceStatus::Timeout;
}

}  // namespace d2o
