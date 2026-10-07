#include "rtsp/nalu.h"

namespace d2o {

namespace {

constexpr std::size_t kNpos = static_cast<std::size_t>(-1);

/// 找下一个 `00 00 01`，返回**三字节里第一个 0x00** 的下标。
///
/// 不必单独处理 4 字节起始码 `00 00 00 01`：它多出来的那个前导零会落在上一个
/// NALU 的尾部，而尾部零本来就要剪掉（见下面的说明）。所以 4 字节和 3 字节
/// 在这里是同一条路径。
///
/// 为什么可以放心地对整个缓冲区做模式扫描：NALU 负载里不可能出现 `00 00 01`。
/// H.264 的防竞争（emulation prevention）规定，负载中任何 `00 00 00/01/02/03`
/// 都要插入一个 0x03 字节隔开，所以 `00 00 01` 只可能是起始码。
std::size_t find_start_code(std::uint8_t const* p, std::size_t n,
                            std::size_t from) {
  for (std::size_t i = from; i + 2 < n; ++i) {
    if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
  }
  return kNpos;
}

}  // namespace

std::size_t split_annex_b(std::uint8_t const* data, std::size_t size,
                          std::int64_t pts, bool keyframe,
                          std::vector<Nalu>& out) {
  std::size_t count = 0;
  std::size_t sc = find_start_code(data, size, 0);

  while (sc != kNpos) {
    std::size_t const body = sc + 3;  // 跳过 00 00 01
    std::size_t const next = find_start_code(data, size, body);
    std::size_t end = (next == kNpos) ? size : next;

    // 剪掉末尾的填充零：Annex-B 的 trailing_zero_8bits、x264 在 CABAC 之后插的
    // cabac_zero_word，以及 4 字节起始码多出来的那个前导零。
    //
    // 不会剪掉真正的数据：NALU 的 rbsp_trailing_bits() 以一个 1 位收尾，所以
    // 最后一个有效字节必定非零。
    while (end > body && data[end - 1] == 0) --end;

    if (end > body) {
      out.emplace_back();
      Nalu& n = out.back();
      n.data.assign(data + body, data + end);
      n.pts = pts;
      n.keyframe = keyframe;
      ++count;
    }
    sc = next;
  }
  return count;
}

bool extract_sps_pps(std::uint8_t const* extradata, std::size_t size,
                     std::vector<std::uint8_t>& sps,
                     std::vector<std::uint8_t>& pps) {
  sps.clear();
  pps.clear();
  if (!extradata || size == 0) return false;

  std::vector<Nalu> nalus;
  split_annex_b(extradata, size, 0, true, nalus);

  for (Nalu const& n : nalus) {
    if (sps.empty() && n.is_sps()) {
      sps = n.data;
    } else if (pps.empty() && n.is_pps()) {
      pps = n.data;
    }
  }
  return !sps.empty() && !pps.empty();
}

}  // namespace d2o
