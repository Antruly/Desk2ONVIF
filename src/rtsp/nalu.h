#pragma once

/**
 * @file src/rtsp/nalu.h
 * @brief NALU 表示与 Annex-B 切分。
 *
 * 为什么必须在进 Live555 之前就把起始码剥掉：`H264VideoStreamDiscreteFramer`
 * 收的是"离散 NALU"，它明确要求内容**不含**起始码。带进去的后果不是报错退出 ——
 * 它只在 `afterGettingFrame1()` 里打一行
 *
 *     H264or5VideoStreamDiscreteFramer error: MPEG 'start code' seen in the input
 *
 * 然后照常把这一帧投递下去（那是个 if / else-if 链，命中了就跳过 SPS/PPS 的
 * 记录而已）。症状是"客户端连得上、RTP 包在流、但画面是花的"，而这行 error
 * 混在别的输出里极容易被忽略。所以起始码在这里就得去掉，别指望下游报错。
 *
 * 另外注意 `split_annex_b()` 顺带干的第二件事：**去掉 NALU 末尾的零字节**。
 * Annex-B 允许 NALU 之间有 `trailing_zero_8bits` 填充，x264 在 CABAC 之后
 * 还会插 `cabac_zero_word`。4 字节起始码 `00 00 00 01` 多出来的那个前导零，
 * 也正是上一个 NALU 的"尾零"。不剪掉的话，这些零会跟着 NALU 一起进 RTP 负载。
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace d2o {

/// 一个不含起始码的 NALU。
struct Nalu {
  std::vector<std::uint8_t> data;
  std::int64_t pts = 0;    ///< 编码器的帧号，用来还原呈现时间
  bool keyframe = false;   ///< 这个 NALU 属于一个关键帧

  /// nal_unit_type（H.264 头字节的低 5 位）
  std::uint8_t type() const {
    return data.empty() ? 0 : static_cast<std::uint8_t>(data[0] & 0x1F);
  }
  /// VCL = 真正带图像数据的 NALU（1..5）。Live555 判断"一个访问单元结束了吗"
  /// 就是靠这个，而那决定了 RTP 的 M 位。单 slice 时一帧恰好一个 VCL NALU，
  /// 判断才成立。
  ///
  /// 出处（H264or5VideoStreamDiscreteFramer.cpp:155，已核对）：
  ///
  ///     Boolean ...::nalUnitEndsAccessUnit(u_int8_t nal_unit_type) {
  ///       //  ... we can't do this reliably, because we don't yet know anything
  ///       //  about the *next* NAL unit that we'll see.  So, we guess this as best
  ///       //  as we can, by assuming that if this NAL unit is a VCL NAL unit,
  ///       //  then it ends the current 'access unit'.
  ///       //
  ///       //  This will be wrong if you are streaming multiple 'slices' per
  ///       //  picture.  In that case, you can define a subclass that reimplements
  ///       //  this virtual function to do the right thing.
  ///       return isVCL(nal_unit_type);
  ///     }
  ///
  /// 库自己的注释就写着"多 slice 时这个判断是错的" —— 实测踩到过一次，
  /// 代价是整条流表面全绿但画面糊成一片（见 x264_encoder.cpp 里 slices 那段）。
  bool is_vcl() const {
    std::uint8_t const t = type();
    return t >= 1 && t <= 5;
  }
  bool is_sps() const { return type() == 7; }
  bool is_pps() const { return type() == 8; }
  bool is_idr() const { return type() == 5; }
};

/// 按 Annex-B 切分，剥掉起始码并剪掉末尾的填充零。
///
/// out **不会**被清空，切出来的 NALU 追加在后面（方便"参数集 + 帧"连着入队）。
/// 返回值是本次追加的个数。
std::size_t split_annex_b(std::uint8_t const* data, std::size_t size,
                          std::int64_t pts, bool keyframe,
                          std::vector<Nalu>& out);

/// 从 Annex-B 的 extradata 里挑出第一组 SPS 和 PPS。
///
/// 编码器打开 `AV_CODEC_FLAG_GLOBAL_HEADER` 之后，SPS/PPS 只存在于 extradata
/// 里，码流本身不含 —— 而 SDP 的 `sprop-parameter-sets` 和"每个 IDR 前重发
/// 参数集"这两件事都要用它们。缺了参数集，部分 NVR 加进来就是黑屏。
bool extract_sps_pps(std::uint8_t const* extradata, std::size_t size,
                     std::vector<std::uint8_t>& sps,
                     std::vector<std::uint8_t>& pps);

}  // namespace d2o
