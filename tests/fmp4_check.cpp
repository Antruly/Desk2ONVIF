/**
 * @file tests/fmp4_check.cpp
 * @brief fmp4_muxer 的端到端验证：真码流进去，真浏览器能播的文件出来。
 *
 * 这个程序的存在本身就是一条结论：**fMP4 封装"看起来对"和"真的对"之间没有
 * 中间态**。sizeof/moov/ftyp 都对、ffprobe 能报出正确的分辨率帧率、VLC 能播，
 * 这些全都不能说明浏览器会接受它 —— MSE 拒收一个分片的原因是
 * "DEMUXER_ERROR_COULD_NOT_PARSE" 或者干脆什么都不说。
 *
 * 所以这里不 mock 任何东西：
 *   1. 用真正的编码器（和主链路同一套 x264 参数）产出一段 Annex-B 码流；
 *   2. 按访问单元喂进 Fmp4Muxer；
 *   3. 把 init 段 + 所有分片拼成一个文件；
 *   4. 用 ffprobe 的 -count_frames **解码**一遍，再用 ffmpeg 解一遍，
 *      并且在任何一行解码器日志出现时报失败（"concealing ... errors" 这类
 *      警告意味着 NALU 被切错了位置，而文件本身还是能播的 —— 只差肉眼看不出来）。
 *
 * 全部检查都打印 [ OK ]/[FAIL]，最后以失败条数作为退出码。
 */

#include "encoder/fmp4_muxer.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void check(bool ok, std::string const& what) {
  std::printf("%s %s\n", ok ? "[ OK ]" : "[FAIL]", what.c_str());
  if (!ok) ++g_failures;
}

void note(std::string const& what) { std::printf("       %s\n", what.c_str()); }

/// 跑一条外部命令，把它的标准输出（以及重定向进来的标准错误）取回来。
/// 用 _popen 而不是 CreateProcess：要的就只是"跑一下、把输出捞回来"，
/// 少一层 API 就少一层出错的地方。
std::string run(std::string const& cmd, int* status = nullptr) {
  std::string out;
  FILE* f = _popen(cmd.c_str(), "r");
  if (!f) {
    if (status) *status = -1;
    return out;
  }
  char buf[4096];
  std::size_t n = 0;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
  int const st = _pclose(f);
  if (status) *status = st;
  return out;
}

std::vector<std::uint8_t> read_file(std::string const& path, bool* ok = nullptr) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (ok) *ok = false;
    return {};
  }
  std::vector<std::uint8_t> b((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
  if (ok) *ok = true;
  return b;
}

bool write_file(std::string const& path, std::vector<std::uint8_t> const& b) {
  std::ofstream f(path, std::ios::binary);
  if (!f) return false;
  f.write(reinterpret_cast<char const*>(b.data()),
          static_cast<std::streamsize>(b.size()));
  return f.good();
}

struct Nal {
  std::size_t start = 0;  ///< 起始码的第一个字节
  std::size_t hdr = 0;    ///< NALU 头的第一个字节（type 在这里）
};

/// 找出所有 NALU 的起点。3 字节和 4 字节起始码都要认 —— 4 字节起始码的头
/// 三个字节本身就是一个合法的 3 字节起始码，先后顺序反了就会漏掉一个 NALU。
std::vector<Nal> find_nals(std::vector<std::uint8_t> const& b) {
  std::vector<Nal> nals;
  std::size_t i = 0;
  while (i + 3 <= b.size()) {
    if (b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1) {
      nals.push_back({i, i + 3});
      i += 3;
    } else if (i + 4 <= b.size() && b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 0 &&
               b[i + 3] == 1) {
      nals.push_back({i, i + 4});
      i += 4;
    } else {
      ++i;
    }
  }
  return nals;
}

struct AccessUnit {
  std::size_t offset = 0;
  std::size_t size = 0;
  bool keyframe = false;
};

/// 把 Annex-B 码流切成访问单元。
///
/// 切法就是 H.264 7.4.1.2.4 那条规则的可操作版本：AUD/SPS/PPS 起新单元，
/// 第一个 VCL（类型 1..5）之后再来 VCL 也起新单元。之所以不能简单地"每个
/// NAL 一个访问单元"，是因为关键帧的 AU 是 SPS+PPS+IDR 三个 NAL 一起的 ——
/// 拆开之后 SPS/PPS 会被当成独立的样本，封装出来的文件里码流长度对不上，
/// 浏览器解到第一个关键帧就崩。x264 的 slices=1 保证一帧只有一个 slice，
/// 所以"再来一个 VCL"就等价于"换了一帧"。
std::vector<AccessUnit> split_access_units(std::vector<std::uint8_t> const& b) {
  std::vector<Nal> const nals = find_nals(b);
  std::vector<AccessUnit> aus;
  if (nals.empty()) return aus;

  std::size_t start_idx = 0;
  bool have_vcl = false;
  bool au_has_idr = false;
  for (std::size_t k = 0; k < nals.size(); ++k) {
    if (nals[k].hdr >= b.size()) break;
    int const t = b[nals[k].hdr] & 0x1F;
    bool const vcl = (t >= 1 && t <= 5);

    if (k > 0) {
      bool const split = (t == 9) ||                          // AUD
                         (vcl && have_vcl) ||                 // 第二个 slice/帧
                         ((t == 7 || t == 8) && have_vcl);    // 参数集跟在图像后面
      if (split) {
        aus.push_back({nals[start_idx].start,
                       nals[k].start - nals[start_idx].start, au_has_idr});
        start_idx = k;
        have_vcl = false;
        au_has_idr = false;
      }
    }
    if (vcl) have_vcl = true;
    if (t == 5) au_has_idr = true;
  }
  aus.push_back({nals[start_idx].start, b.size() - nals[start_idx].start,
                 au_has_idr});
  return aus;
}

/// 从码流里抠出第一组 SPS/PPS，拼成编码器风格的 Annex-B extradata。
/// 真实链路里这份东西由 x264 通过 AV_CODEC_FLAG_GLOBAL_HEADER 给出，
/// 这里是从码流里"考古"出来的等价物 —— 内容和顺序都一样。
std::vector<std::uint8_t> extract_extradata(std::vector<std::uint8_t> const& b) {
  std::vector<Nal> const nals = find_nals(b);
  std::vector<std::uint8_t> extra;
  bool got_sps = false;
  bool got_pps = false;
  for (std::size_t k = 0; k < nals.size() && !(got_sps && got_pps); ++k) {
    std::size_t const hdr = nals[k].hdr;
    if (hdr >= b.size()) break;
    int const t = b[hdr] & 0x1F;
    if (t != 7 && t != 8) continue;
    if (t == 7 && got_sps) continue;
    if (t == 8 && got_pps) continue;
    std::size_t const end = (k + 1 < nals.size()) ? nals[k + 1].start : b.size();
    // 统一用 4 字节起始码补出来：编码器给的 extradata 也是 4 字节的。
    extra.push_back(0);
    extra.push_back(0);
    extra.push_back(0);
    extra.push_back(1);
    extra.insert(extra.end(), b.begin() + hdr, b.begin() + end);
    if (t == 7) got_sps = true;
    if (t == 8) got_pps = true;
  }
  return extra;
}

/// 顶层 box 的类型列表。init 段的结构对不对看这个就够了 —— 比在整段字节里
/// 搜 "moov" 强：搜字符串会命中码流里的随机字节，而这里必须是一个真正的
/// box 头。
std::vector<std::string> top_level_boxes(std::vector<std::uint8_t> const& b) {
  std::vector<std::string> types;
  std::size_t p = 0;
  while (p + 8 <= b.size()) {
    std::uint64_t size = (static_cast<std::uint64_t>(b[p]) << 24) |
                         (static_cast<std::uint64_t>(b[p + 1]) << 16) |
                         (static_cast<std::uint64_t>(b[p + 2]) << 8) |
                         static_cast<std::uint64_t>(b[p + 3]);
    std::string type(reinterpret_cast<char const*>(&b[p + 4]), 4);
    std::size_t hdr = 8;
    if (size == 1) {
      if (p + 16 > b.size()) break;
      size = 0;
      for (int i = 0; i < 8; ++i) {
        size = (size << 8) | b[p + 8 + static_cast<std::size_t>(i)];
      }
      hdr = 16;
    } else if (size == 0) {
      size = b.size() - p;  // 一直到最后
    }
    if (size < hdr || p + size > b.size()) {
      types.push_back(type + "(坏长度)");
      break;
    }
    types.push_back(type);
    p += static_cast<std::size_t>(size);
  }
  return types;
}

/// 一个 box：类型 + 载荷的起点与长度（长度**不含** 8/16 字节的 box 头）。
struct BoxRef {
  std::string type;
  std::size_t payload = 0;
  std::size_t len = 0;
};

/// 走一遍 [begin, end) 里的所有 box。
///
/// 与 top_level_boxes 的区别是它给的是**区间**而不是整段：读 tfdt 要一层层往下
/// 钻（moof → traf → tfdt），每一层都只认自己那一段。长度说不通时返回 false
/// —— 不猜、不修，因为"猜"在这里的表现是读到一个看着很合理的错时间戳。
bool walk_boxes(std::vector<std::uint8_t> const& b, std::size_t begin,
                std::size_t end, std::vector<BoxRef>* out) {
  std::size_t p = begin;
  while (p + 8 <= end) {
    std::uint64_t size = (static_cast<std::uint64_t>(b[p]) << 24) |
                         (static_cast<std::uint64_t>(b[p + 1]) << 16) |
                         (static_cast<std::uint64_t>(b[p + 2]) << 8) |
                         static_cast<std::uint64_t>(b[p + 3]);
    std::string const type(reinterpret_cast<char const*>(&b[p + 4]), 4);
    std::size_t hdr = 8;
    if (size == 1) {
      if (p + 16 > end) return false;
      size = 0;
      for (int i = 0; i < 8; ++i) {
        size = (size << 8) | b[p + 8 + static_cast<std::size_t>(i)];
      }
      hdr = 16;
    } else if (size == 0) {
      size = end - p;  // 一直到最后
    }
    if (size < hdr || p + static_cast<std::size_t>(size) > end) return false;
    out->push_back({type, p + hdr, static_cast<std::size_t>(size) - hdr});
    p += static_cast<std::size_t>(size);
  }
  return true;
}

/// 从一个 moof 里读 baseMediaDecodeTime。
///
/// tfdt 的载荷是 version(1) + flags(3) + 时间（version 0 是 32 位、version 1 是
/// 64 位，都是大端）。这个数就是该分片第一个样本的解码时间，单位是 mdhd 里的
/// timescale —— 而"媒体时间轴的 0 点在哪儿"问的正是它，所以这一段非量不可。
bool moof_tfdt_at(std::vector<std::uint8_t> const& b, BoxRef const& moof,
                  std::uint64_t* out) {
  std::vector<BoxRef> moof_kids;
  if (!walk_boxes(b, moof.payload, moof.payload + moof.len, &moof_kids)) {
    return false;
  }
  for (BoxRef const& traf : moof_kids) {
    if (traf.type != "traf") continue;
    std::vector<BoxRef> traf_kids;
    if (!walk_boxes(b, traf.payload, traf.payload + traf.len, &traf_kids)) {
      return false;
    }
    for (BoxRef const& tfdt : traf_kids) {
      if (tfdt.type != "tfdt" || tfdt.len < 4) continue;
      std::size_t const n = (b[tfdt.payload] == 1) ? 8u : 4u;
      if (tfdt.len < 4 + n) return false;
      std::uint64_t v = 0;
      for (std::size_t i = 0; i < n; ++i) {
        v = (v << 8) | b[tfdt.payload + 4 + i];
      }
      *out = v;
      return true;
    }
  }
  return false;
}

/// 把累积的分片缓冲按顶层 box 走一遍，每个 moof 取一条 tfdt —— 一个分片一条，
/// 顺序就是它们在文件里的顺序。
bool fragment_tfdts(std::vector<std::uint8_t> const& b,
                    std::vector<std::uint64_t>* out) {
  std::vector<BoxRef> tops;
  if (!walk_boxes(b, 0, b.size(), &tops)) return false;
  for (BoxRef const& t : tops) {
    if (t.type != "moof") continue;
    std::uint64_t v = 0;
    if (!moof_tfdt_at(b, t, &v)) return false;
    out->push_back(v);
  }
  return true;
}

/// 每个分片的 trun 里 sample_count 之和 —— "分片切碎了，样本有没有丢"的直接量法。
///
/// trun 的载荷是 version(1)+flags(3)+sample_count(4)，**version 0 与 1 在
/// sample_count 这一段的宽度相同**（版本只影响后面那些可选字段的宽度），所以
/// 读这 4 个字节不需要按版本分支。切碎之后每个分片的样本数各不相同（尾巴那一片
/// 短），所以只把总数与逐片条数报出来，不去猜某一片应该是几个。
bool fragment_sample_total(std::vector<std::uint8_t> const& b,
                           std::vector<std::uint32_t>* per_frag,
                           std::size_t* total) {
  std::vector<BoxRef> tops;
  if (!walk_boxes(b, 0, b.size(), &tops)) return false;
  std::size_t sum = 0;
  for (BoxRef const& moof : tops) {
    if (moof.type != "moof") continue;
    std::vector<BoxRef> moof_kids;
    if (!walk_boxes(b, moof.payload, moof.payload + moof.len, &moof_kids)) {
      return false;
    }
    std::size_t frag_samples = 0;
    for (BoxRef const& traf : moof_kids) {
      if (traf.type != "traf") continue;
      std::vector<BoxRef> traf_kids;
      if (!walk_boxes(b, traf.payload, traf.payload + traf.len, &traf_kids)) {
        return false;
      }
      for (BoxRef const& trun : traf_kids) {
        if (trun.type != "trun" || trun.len < 8) continue;
        std::uint32_t const n =
            (static_cast<std::uint32_t>(b[trun.payload + 4]) << 24) |
            (static_cast<std::uint32_t>(b[trun.payload + 5]) << 16) |
            (static_cast<std::uint32_t>(b[trun.payload + 6]) << 8) |
            static_cast<std::uint32_t>(b[trun.payload + 7]);
        frag_samples += n;
      }
    }
    if (per_frag) per_frag->push_back(static_cast<std::uint32_t>(frag_samples));
    sum += frag_samples;
  }
  if (total) *total = sum;
  return true;
}

std::string hex_head(std::vector<std::uint8_t> const& b, std::size_t n) {
  std::string s;
  char t[8];
  for (std::size_t i = 0; i < n && i < b.size(); ++i) {
    std::snprintf(t, sizeof t, "%02X ", b[i]);
    s += t;
  }
  return s;
}

/// 从 ffprobe 的 default=nw=1 输出里取 key=value。
std::string field(std::string const& probe_out, std::string const& key) {
  std::string const pat = key + "=";
  std::size_t p = 0;
  while ((p = probe_out.find(pat, p)) != std::string::npos) {
    // 必须落在行首，否则 "width=" 会命中 "coded_width="。
    if (p == 0 || probe_out[p - 1] == '\n') {
      std::size_t const e = probe_out.find('\n', p);
      return probe_out.substr(p + pat.size(),
                              (e == std::string::npos ? probe_out.size() : e) -
                                  (p + pat.size()));
    }
    p += pat.size();
  }
  return std::string();
}

/// _popen 走的是 cmd.exe，而 cmd.exe 不认正斜杠：`third_party/ffmpeg/bin/x.exe`
/// 会被当成一个不存在的命令，报一句本地化之后完全看不出所以然的
/// "'third_party' 不是内部或外部命令"。所以拼命令行之前统一转成反斜杠。
/// （C 运行时的文件 API 两种都吃，所以只有这里的字符串要转。）
std::string cmd_path(std::string const& p) {
  std::string s = p;
  for (char& c : s) {
    if (c == '/') c = '\\';
  }
  return s;
}

std::string const kFfprobe = cmd_path("third_party/ffmpeg/bin/ffprobe.exe");
std::string const kFfmpeg = cmd_path("third_party/ffmpeg/bin/ffmpeg.exe");

}  // namespace

int main() {
  std::string const kSrc = "out/fmp4_src.h264";
  std::string const kOut = "out/fmp4_check.mp4";
  std::string const src_cmd = cmd_path(kSrc);
  std::string const out_cmd = cmd_path(kOut);

  std::printf("=== 1. 准备 Annex-B 码流 ===\n");
  bool have_recording = false;
  {
    std::ifstream f("out/phase1-10s.mp4", std::ios::binary);
    have_recording = f.good();
  }
  // 编码参数和主链路一致（video_encoder.h 里那套）：ultrafast+zerolatency+
  // baseline+bframes=0，slices=1 且关掉 sliced-threads。用别的参数也能验封装，
  // 但那样验的就不是我们真正要发出去的东西了。
  std::string const x264 = "-c:v libx264 -preset ultrafast -tune zerolatency "
                           "-x264-params \"slices=1:sliced-threads=0\" "
                           "-profile:v baseline -g 60 -bf 0 -an -f h264";
  std::string cmd;
  if (have_recording) {
    note("输入：out/phase1-10s.mp4（项目自己的录制）");
    cmd = kFfmpeg + " -hide_banner -loglevel error -y -i " +
          cmd_path("out/phase1-10s.mp4") + " " + x264 + " -y " + src_cmd;
  } else {
    note("out/phase1-10s.mp4 不存在，退回到合成画面（300 帧 @30fps）");
    cmd = kFfmpeg + " -hide_banner -loglevel error -y -f lavfi -i "
                    "testsrc2=size=1280x720:rate=30:duration=10 " +
          x264 + " -y " + src_cmd;
  }
  note("$ " + cmd);
  int st = 0;
  std::string const enc_log = run(cmd + " 2>&1", &st);
  if (!enc_log.empty()) note("ffmpeg: " + enc_log);
  check(st == 0, "转码出一条 Annex-B 码流");

  std::vector<std::uint8_t> src = read_file(kSrc);
  check(!src.empty(), "码流非空");
  if (src.empty()) return 1;
  note("码流 " + std::to_string(src.size()) + " 字节");

  std::string const src_probe = run(
      kFfprobe +
      " -v error -show_entries stream=codec_name,width,height,avg_frame_rate,"
      "nb_read_frames -count_frames -of default=nw=1 " + src_cmd);
  note("源码流：" + src_probe);
  int const src_frames = std::atoi(field(src_probe, "nb_read_frames").c_str());
  int const width = std::atoi(field(src_probe, "width").c_str());
  int const height = std::atoi(field(src_probe, "height").c_str());
  int const fps = 30;
  check(width > 0 && height > 0 && src_frames > 0,
        "能读出源码流的尺寸与帧数");

  std::printf("\n=== 2. 切访问单元并喂进 Fmp4Muxer ===\n");
  std::vector<AccessUnit> const aus = split_access_units(src);
  int key_aus = 0;
  for (AccessUnit const& a : aus) {
    if (a.keyframe) ++key_aus;
  }
  note("切出 " + std::to_string(aus.size()) + " 个访问单元，其中 " +
       std::to_string(key_aus) + " 个含 IDR");
  check(static_cast<int>(aus.size()) == src_frames,
        "访问单元数 == 源码流帧数（切分没错位）");

  std::vector<std::uint8_t> const extradata = extract_extradata(src);
  check(!extradata.empty(), "从码流里抠出了 SPS/PPS 作为 extradata");
  note("extradata " + std::to_string(extradata.size()) + " 字节");

  d2o::Fmp4Muxer muxer;
  bool const inited = muxer.init(width, height, extradata, fps);
  check(inited, "Fmp4Muxer::init");
  if (!inited) return 1;
  check(muxer.ready(), "Fmp4Muxer::ready");

  std::printf("\n=== 3. init 段（ftyp+moov）===\n");
  std::vector<std::uint8_t> const& init_seg = muxer.init_segment();
  note("init 段 " + std::to_string(init_seg.size()) + " 字节");
  if (!init_seg.empty()) {
    note("开头字节：" + hex_head(init_seg, 24));
  }
  check(!init_seg.empty(), "init 段不是空的（empty_moov 生效）");
  check(init_seg.size() >= 8 && init_seg[4] == 'f' && init_seg[5] == 't' &&
            init_seg[6] == 'y' && init_seg[7] == 'p',
        "init 段以 ftyp box 开头");
  std::vector<std::string> const boxes = top_level_boxes(init_seg);
  std::string box_list;
  for (std::string const& t : boxes) box_list += t + " ";
  note("init 段的顶层 box：" + box_list);
  bool has_moov = false;
  for (std::string const& t : boxes) {
    if (t == "moov") has_moov = true;
  }
  check(has_moov, "init 段里有 moov box（而且写头时就出来了）");
  // 只有 ftyp+moov：一旦多出 mdat，说明分片已经跟着 init 段一起漏出来了，
  // 那 appending 顺序就会错。
  check(boxes.size() == 2 && boxes[0] == "ftyp",
        "init 段只有 ftyp+moov 两个顶层 box");

  // 把整段码流喂进一个封装器，末尾再补一个关键帧把最后一个 GOP 顶出来。
  //
  // 抽成 lambda 是因为**下面 4b 要用第二遍**（把整体 dts 平移 74 秒），而两遍
  // 必须喂进去一模一样的东西 —— 复制一份迟早会漂，一漂，"两遍的 tfdt 相同"
  // 这条判据就变成在比两段不同的输入了，而它照样会绿或红。
  auto feed_all = [&](d2o::Fmp4Muxer& m, std::int64_t base_us,
                      std::vector<std::uint8_t>& out, int& calls,
                      std::vector<std::size_t>* per_call, bool verbose,
                      std::size_t* tail_added) {
    int bad = 0;
    for (std::size_t i = 0; i < aus.size(); ++i) {
      d2o::Fmp4Sample s;
      s.data = src.data() + aus[i].offset;
      s.size = aus[i].size;
      s.pts_us = base_us + static_cast<std::int64_t>(i) * 1000000 / fps;
      s.dts_us = s.pts_us;
      s.keyframe = aus[i].keyframe;
      std::size_t const before = out.size();
      if (!m.write(s, out)) ++bad;
      std::size_t const added = out.size() - before;
      if (added > 0) {
        ++calls;
        if (per_call) per_call->push_back(added);
      }
      if (verbose && ((i + 1) % 60 == 0 || i + 1 == aus.size())) {
        std::printf("       喂到第 %3zu 帧：累计分片 %zu 字节\n", i + 1, out.size());
      }
    }

    // 到这里最后一个 GOP 还压在封装器里：mov 要等**下一个**关键帧才知道上一个
    // 分片在哪结束。这条路是 `fragment_on_flush = false`（默认值），而**关掉
    // 开关时 `flush()` 到底切不切由 mov 自己决定** —— 接口现在有 flush() 了，
    // 但"关掉开关它是不是等价于没有"这件事这一段并没有量过（4c 的对照组去量）。
    //
    // 所以这里照旧补喂一个关键帧把尾巴顶出来：mov 在收下它之前会先 flush 上一个
    // 分片，而这个补喂的样本自己会留在待发状态，不会进文件。文件里因此正好是
    // 喂进去的那 300 帧（源码流是 10 秒 30fps）。
    //
    // **"补喂一个样本"而不是"改成调 flush()"是刻意的**：这一遍是基线，它要证明
    // 的是"开关关掉时，逐字节与加这个开关之前一样"。拿 flush() 替掉这里，基线就
    // 不再是基线，4c 那个对照也就没有对照物了 —— 那正是"两边都改了一点，于是
    // 差值什么也说明不了"的形状。
    //
    // 打开开关的那一路（4c）**不补喂任何东西**：flush() 自己会把尾巴切出来。
    // 也就是说"尾部不再需要一个假样本"是这个接口带来的第一处直接改善，
    // 而它在 4c 里是被断言盯着的（样本总数 == 喂进去的帧数）。
    {
      d2o::Fmp4Sample s;
      // 用最后一个关键帧的字节，只是把时间戳推到所有真实样本之后。
      std::size_t k = 0;
      for (std::size_t i = aus.size(); i-- > 0;) {
        if (aus[i].keyframe) {
          k = i;
          break;
        }
      }
      s.data = src.data() + aus[k].offset;
      s.size = aus[k].size;
      s.pts_us = base_us + static_cast<std::int64_t>(aus.size()) * 1000000 / fps;
      s.dts_us = s.pts_us;
      s.keyframe = true;
      std::size_t const before = out.size();
      if (!m.write(s, out)) ++bad;
      std::size_t const added = out.size() - before;
      if (tail_added) *tail_added = added;
      if (verbose) {
        note("补喂一个关键帧，顶出最后一个 GOP：" + std::to_string(added) +
             " 字节（这一个样本自己不会进文件）");
      }
      if (added > 0) {
        ++calls;
        if (per_call) per_call->push_back(added);
      }
    }
    return bad;
  };

  std::printf("\n=== 4. 逐样本写入 ===\n");
  std::vector<std::uint8_t> fragments;
  int calls_with_output = 0;
  std::vector<std::size_t> per_call;  // 每次调用新增的字节数（只记非零的）
  std::size_t tail_added = 0;
  int const failed = feed_all(muxer, 0, fragments, calls_with_output, &per_call,
                              true, &tail_added);
  check(failed == 0, "所有样本都写进去了（没有 write() 返回 false）");
  check(tail_added > 0, "最后一个 GOP 被下一个关键帧顶了出来");

  // 这条是整份检查里最要紧的一条。分片是"下一个关键帧到来时"才落下来的，
  // 如果全堆在最后一次 write() 里，说明 frag_keyframe 没生效 —— 浏览器那边
  // 的表现是 init 段之后长时间一片漆黑，直到流结束才突然出现画面。
  note("产生输出的 write() 调用次数：" + std::to_string(calls_with_output));
  std::string sizes;
  for (std::size_t i = 0; i < per_call.size(); ++i) {
    sizes += std::to_string(per_call[i]);
    if (i + 1 < per_call.size()) sizes += ", ";
  }
  note("每次的字节数：" + sizes);
  check(calls_with_output >= 3,
        "分片是增量吐出来的（至少 3 次 write() 产生了字节）");
  note("分片总量 " + std::to_string(fragments.size()) + " 字节");

  std::printf("\n=== 4b. t0 映射：媒体时间轴的 0 点是不是第一个样本 ===\n");
  // 网页预览要报"真实延迟"，就得知道"媒体时间轴的 0 点对应哪个墙钟时刻"
  // （第四轮 Phase W 的 t0_wall_ms）。这条映射成立的前提是：**喂进封装器的第一个
  // 样本落在媒体时间 0 上**。
  //
  // 这个前提原先是从代码里推出来的（write_nal() 里 `dts_us = s.dts_us -
  // base_dts_us_`，而 base_dts_us_ 在第一个样本进来时被设成 s.dts_us），所以按
  // "一条断言要先量一次"的纪律在这儿量一遍。量法是**两遍喂同一段码流**，第二遍
  // 整体 dts 平移 74 秒（模拟"程序已经跑了 74 秒才有人打开预览页"）。
  //
  // **量出来的结果比原来推的那句更强，而且把机制改了。** 第一版的变异检验
  // （out\fmp4-t0-mut-sweep.ps1 的 M1）把 `base_dts_us_ = s.dts_us;` 改成常数 0
  // —— 于是第二遍真正交给 av_write_frame 的 dts 是从 74000000 起的 ——
  // 输出的 tfdt **照样是 0, 2000000, 4000000, 6000000, 8000000**，一条都没红。
  // 也就是说那个 0 点**不是我们那次减法给的**，是 libavformat 的 mov 封装器自己
  // 做的（它按"这条轨看到的第一个 dts"重新定基准）。我们那次减法防的是 int64
  // 溢出（见 write_nal() 的注释），对时间轴的 0 点在观测上是**冗余的**。
  //
  // 对本阶段的意义：t0_wall_ms = 喂进第一个样本那一刻的墙钟 —— 这个映射成立，
  // 而且**不依赖我们那行减法**，是这一层里最稳的一个前提。
  //
  // 断言分成三条，为的是让红的只能是该红的那一条（陷阱 91 要的形状）：
  //   ① 第一遍的 tfdt 不能全是 0 —— 否则说明这把尺子根本读不出东西；
  //   ② 每个分片的 tfdt 逐条等于**该分片第一个关键帧在输入里的媒体时间**
  //      （关键帧位置从 aus 现数，不是写死的常数）—— 这一条才真的在量
  //      "时间轴是不是 1:1 跟着输入走"；
  //   ③ 两遍的 tfdt 逐条相同 —— 媒体时间轴与"采集是什么时候开始的"无关。
  //
  // ②的变异检验是同一次 sweep 的 M2（把每个样本都标成关键帧 ⇒ 分片节奏从
  // "每 2 秒一个"变成"每帧一个"）：**只红②**，①③同伴全绿。
  //
  // ③就是 t0_wall_ms 的全部依据。要说清它的性质：**③不是我们这行代码的
  // 变更探测器**（M1 已经证明改那行它照样绿），它的证伪面在依赖那一层
  // （换 movflags / 换封装器）。留着它是因为它量的正是本阶段要依赖的那件事。
  //
  // 断言"第一个分片的 tfdt 是 0"是**与时基无关**的（零在任何时基下都是零），
  // 所以这一段不需要去解析 mdhd 拿 timescale。
  std::vector<std::uint64_t> base_tfdt;
  bool const base_read = fragment_tfdts(fragments, &base_tfdt);
  std::string base_list;
  for (std::size_t i = 0; i < base_tfdt.size(); ++i) {
    base_list += std::to_string(base_tfdt[i]);
    if (i + 1 < base_tfdt.size()) base_list += ", ";
  }
  note("第一遍（dts 从 0 起）：" + std::to_string(base_tfdt.size()) +
       " 个分片，tfdt = " + base_list);
  check(base_read && base_tfdt.size() >= 3,
        "能按分片读出 tfdt（moof → traf → tfdt 三层都走通了）");
  bool any_nonzero = false;
  for (std::uint64_t v : base_tfdt) {
    if (v != 0) any_nonzero = true;
  }
  check(any_nonzero, "tfdt 里存在非 0 值（这把尺子读得出非 0，不是恒 0）");
  check(!base_tfdt.empty() && base_tfdt[0] == 0,
        "第一个分片的 tfdt 是 0 —— 媒体时间 0 点就是第一个样本");

  // ②每个分片的 tfdt == 该分片第一个关键帧在输入里的媒体时间。期望值从 aus
  // **现数**（不是写死的常数、也不是从 tfdt 自己推出来的），所以它同时验了两件事：
  // tfdt 这把尺子读得出真实数字，且时间轴确实是 1:1 跟着输入走的。
  // 变异检验 = 同一次 sweep 的 M2（每个样本都标成关键帧 ⇒ 分片节奏变成每帧一个）。
  std::vector<std::uint64_t> expect_tfdt;
  for (std::size_t i = 0; i < aus.size(); ++i) {
    if (aus[i].keyframe) {
      expect_tfdt.push_back(static_cast<std::uint64_t>(
          static_cast<std::int64_t>(i) * 1000000 / fps));
    }
  }
  std::string expect_list;
  for (std::size_t i = 0; i < expect_tfdt.size(); ++i) {
    expect_list += std::to_string(expect_tfdt[i]);
    if (i + 1 < expect_tfdt.size()) expect_list += ", ";
  }
  note("按输入里的关键帧算出来的期望 tfdt = " + expect_list + "（" +
       std::to_string(expect_tfdt.size()) + " 条）");
  check(base_tfdt == expect_tfdt,
        "每个分片的 tfdt 都等于该分片第一个关键帧的媒体时间（期望 " +
            std::to_string(expect_tfdt.size()) + " 条，实读 " +
            std::to_string(base_tfdt.size()) + " 条）");

  std::int64_t const kShiftUs = 74000000;  // 74 秒
  d2o::Fmp4Muxer muxer2;
  bool const inited2 = muxer2.init(width, height, extradata, fps);
  check(inited2, "第二个封装器 init（同一段码流、同一份 extradata）");
  if (inited2) {
    std::vector<std::uint8_t> frag2;
    int calls2 = 0;
    std::size_t tail2 = 0;
    int const failed2 =
        feed_all(muxer2, kShiftUs, frag2, calls2, nullptr, false, &tail2);
    note("第二遍：dts 整体平移 " + std::to_string(kShiftUs / 1000000) +
         " 秒，分片 " + std::to_string(frag2.size()) + " 字节");
    check(failed2 == 0, "第二遍所有样本都写进去了");

    std::vector<std::uint64_t> shift_tfdt;
    bool const shift_read = fragment_tfdts(frag2, &shift_tfdt);
    std::string shift_list;
    for (std::size_t i = 0; i < shift_tfdt.size(); ++i) {
      shift_list += std::to_string(shift_tfdt[i]);
      if (i + 1 < shift_tfdt.size()) shift_list += ", ";
    }
    note("第二遍的 tfdt = " + shift_list);

    check(shift_read && shift_tfdt.size() == base_tfdt.size(),
          "两遍的分片数相同（喂进去的东西一模一样）");
    check(!shift_tfdt.empty() && shift_tfdt == base_tfdt,
          "两遍的 tfdt 逐条相同 —— 时间轴与「采集什么时候开始」无关");
  }

  std::printf("\n=== 4c. 切碎不等于切坏（分片不再等关键帧）===\n");
  // 这一段是 Phase W (b) 的主判据，也是 `fmp4_muxer.h` 里 flush() 那段 ⚠️
  // 欠下的那次测量。两件事：
  //
  //   (1) **主判据**：打开 `fragment_on_flush` 之后，用同一个馈入序列、同一个
  //       flush 节奏切出来的每一个分片，都能被 libavformat **顺序读回**、样本
  //       一个不少、时间戳不乱。分片边界从"跟着 GOP 走"变成"跟着调用方走"，
  //       这件事本身的代价必须是零 —— 否则省下的那半个 GOP 是拿画面换来的，
  //       而它在浏览器上只表现为"偶尔糊一下"，没人会归因到分片边界上。
  //   (2) **对照组**：关掉开关、喂同一段、调同样多次 flush，看 mov 是不是照样
  //       在我们要求的位置切。**结论只报告、不断言**：它回答的是"关掉 frag_custom
  //       时 mov 怎么对待一次强制 flush"，而随包发布的头文件里两个 write 函数的
  //       NULL 语义写得很清楚，这一句却一个字都没有，这里又读不到 movenc 的源码。
  //       所以它是测量不是判据 —— 量出来是哪一边，都不该让这份检查红。
  //
  // 两个前提写在这里，免得读的人以为这一段在验别的东西：**两遍用的是同一段
  // 码流、同一个 flush 节奏**（否则两个分片数没有可比性），而且**都没有补喂
  // 关键帧** —— flush() 自己会把尾巴切出来，那一手是 feed_all 那条基线才需要的。
  int const flush_every = fps / 4 > 0 ? fps / 4 : 1;  // ≈250 ms 一刀
  note("flush 节奏：每 " + std::to_string(flush_every) + " 帧一刀（标称帧率 " +
       std::to_string(fps) + "，源码流关键帧每 60 帧一个）");

  auto feed_with_flush = [&](d2o::Fmp4Muxer& m, std::vector<std::uint8_t>& out,
                             int& writes_bad, int& flush_bad, int& flushes,
                             bool verbose) {
    for (std::size_t i = 0; i < aus.size(); ++i) {
      d2o::Fmp4Sample s;
      s.data = src.data() + aus[i].offset;
      s.size = aus[i].size;
      s.pts_us = static_cast<std::int64_t>(i) * 1000000 / fps;
      s.dts_us = s.pts_us;
      s.keyframe = aus[i].keyframe;
      if (!m.write(s, out)) ++writes_bad;
      if ((static_cast<int>(i) + 1) % flush_every == 0) {
        ++flushes;
        if (!m.flush(out)) ++flush_bad;
      }
    }
    // 收尾那一刀：把最后不足 flush_every 帧的尾巴切出来。**有了 flush()，
    // 尾部不再需要"补喂一个假关键帧"那套手法** —— 那是没有它时的唯一出路，
    // 而代价是那个假样本必须永远不进文件（feed_all 里那一大段注释）。
    ++flushes;
    if (!m.flush(out)) ++flush_bad;
    if (verbose) {
      note("共调 flush() " + std::to_string(flushes) + " 次（含收尾那一次）");
    }
  };

  std::string const kShred = "out/fmp4_check_shredded.mp4";
  std::string const shred_cmd = cmd_path(kShred);
  std::vector<std::uint32_t> shred_per;
  std::size_t shred_total = 0;
  // 切碎版的**完整文件字节**。它声明在这一层（而不是 4c-2 那个块里）是为了让
  // 下面的对照组拿它做逐字节比对 —— 那一条是本段最硬的读数，而它在 4c-2 那个
  // 块的作用域之外（那里 805 行原先写的是在这里就地声明）。shred_ok 为假时它
  // 是空的，比对那一处会显式让开。
  std::vector<std::uint8_t> shred_file;

  d2o::Fmp4Muxer shred;
  bool const shred_ok =
      shred.init(width, height, extradata, fps, /*fragment_on_flush=*/true);
  check(shred_ok, "打开 fragment_on_flush 的封装器 init");
  if (shred_ok) {
    std::vector<std::uint8_t> shred_frags;
    int writes_bad = 0, flush_bad = 0, flushes = 0;
    feed_with_flush(shred, shred_frags, writes_bad, flush_bad, flushes, true);
    check(writes_bad == 0, "所有样本都写进去了（frag_custom 这一遍）");
    check(flush_bad == 0, "每一次 flush() 都返回成功");

    // 空操作特性：没有新样本时 flush() 必须**一个字节都不写**。
    // 这一条不是锦上添花 —— mov 在强制 flush 之下会写出**零样本**的 moof+mdat，
    // 而那是 MSE 的 SourceBuffer 当场拒收的东西（AppendError）。接口里那道闸门
    // 就是为它设的，所以在这里直接量它，而不是只把理由写在注释里。
    std::vector<std::uint8_t> extra;
    bool const noop_ret = shred.flush(extra);
    check(noop_ret && extra.empty(),
          "没有新样本时 flush() 是空操作（写 " + std::to_string(extra.size()) +
              " 字节，应为 0）");

    bool const counted =
        fragment_sample_total(shred_frags, &shred_per, &shred_total);
    std::string counts;
    for (std::size_t i = 0; i < shred_per.size(); ++i) {
      counts += std::to_string(shred_per[i]);
      if (i + 1 < shred_per.size()) counts += ",";
    }
    note("切碎后分片 " + std::to_string(shred_per.size()) + " 个，每片样本数 " +
         counts);
    check(counted, "能按 moof → traf → trun 读出每个分片的样本数");
    note("样本总数 " + std::to_string(shred_total) + "（喂进去 " +
         std::to_string(aus.size()) + "）");
    check(shred_total == aus.size(),
          "切碎之后样本一个不少（trun 的 sample_count 之和 == 喂进去的帧数）");

    // 分片数必须跟着 flush 的节奏走，而不是又落回"每 2 秒一个关键帧"。
    // 判据取**下界**、不取精确值：mov 完全可以在我们要求的点之外再切几刀
    // （它是分片器的主人），多切不违反任何东西；少切才是这个功能没生效。
    std::size_t const min_frags =
        aus.size() / static_cast<std::size_t>(flush_every);
    check(shred_per.size() >= min_frags,
          "分片数 " + std::to_string(shred_per.size()) + " ≥ " +
              std::to_string(min_frags) + "（跟着 flush 的节奏切，不是跟着 GOP）");

    std::vector<std::uint64_t> shred_tfdt;
    bool const shred_read = fragment_tfdts(shred_frags, &shred_tfdt);
    bool ordered = shred_read && shred_tfdt.size() == shred_per.size();
    for (std::size_t i = 1; i < shred_tfdt.size() && ordered; ++i) {
      if (shred_tfdt[i] <= shred_tfdt[i - 1]) ordered = false;
    }
    check(ordered, "每个分片的 tfdt 严格递增（切碎了时间戳也不乱）");
    check(shred_read && !shred_tfdt.empty() && shred_tfdt[0] == 0,
          "第一个分片的 tfdt 还是 0（0 点没被切分改动）");

    // ── 4c-2：把切碎版交给别人的解码器顺序读回 ──────────────────────
    // 这一步是"切碎不等于切坏"最硬的一条：**不是我们自己的解析器说它没问题，
    // 是 libavformat 从头到尾读了一遍**。分片不以关键帧开头在这里是常态（每 7 帧
    // 一刀，而关键帧每 60 帧一个），所以它同时也在验"连续 append 时第一个样本
    // 不是同步样本"这件事本身 —— 这正是浏览器那一半（latency_probe.mjs）
    // 要回答的问题的进程内版本，只不过这里量的是封装格式，不是 MSE 收不收。
    std::printf("\n=== 4c-2. 切碎版 => %s ===\n", kShred.c_str());
    shred_file = init_seg;
    shred_file.insert(shred_file.end(), shred_frags.begin(), shred_frags.end());
    check(write_file(kShred, shred_file), "写出切碎版文件");
    note("文件总长 " + std::to_string(shred_file.size()) + " 字节");

    std::string const sh_probe = run(
        kFfprobe +
        " -v error -show_entries stream=codec_name,width,height,nb_read_frames "
        "-count_frames -of default=nw=1 " +
        shred_cmd);
    note("切碎版 ffprobe：" + sh_probe);
    check(field(sh_probe, "codec_name") == "h264", "切碎版还是 h264");
    int const sh_frames = std::atoi(field(sh_probe, "nb_read_frames").c_str());
    note("切碎版解出 " + std::to_string(sh_frames) + " 帧（源 " +
         std::to_string(src_frames) + "）");
    check(sh_frames == src_frames, "切碎版解出的帧数 == 源码流帧数");

    int sh_st = 0;
    std::string const sh_dec =
        run(kFfmpeg + " -v error -i " + shred_cmd + " -f null -" + " 2>&1", &sh_st);
    if (!sh_dec.empty()) {
      std::printf("------- 切碎版解码输出 -------\n%s"
                  "-----------------------------\n",
                  sh_dec.c_str());
    }
    check(sh_st == 0 && sh_dec.empty(),
          "切碎版整段解码零错误输出（分片不以关键帧开头也不掉帧）");
    if (sh_st == 0 && sh_dec.empty()) std::printf("DECODE OK (shredded)\n");
  }

  // ── 对照组：同一个馈入、同样多次 flush，但开关**关**着 ─────────────
  // ⚠️ 这段是 Phase W (b) 唯一一处"只报告、不断言"的地方，理由不是省事：
  // 它量的是 libavformat 的行为，不是我们的产品——把上游的行为写成我们的判据，
  // 换一个 FFmpeg 版本就会红，而红的原因与我们的代码无关（陷阱 87 的同一族：
  // 判据依赖了一个没被写进前提的外部条件）。所以它**报告两个数**，人去读。
  if (shred_ok) {
    d2o::Fmp4Muxer plain;
    if (plain.init(width, height, extradata, fps, /*fragment_on_flush=*/false)) {
      std::vector<std::uint8_t> plain_frags;
      int writes_bad = 0, flush_bad = 0, flushes = 0;
      feed_with_flush(plain, plain_frags, writes_bad, flush_bad, flushes, false);
      std::vector<std::uint32_t> plain_per;
      std::size_t plain_total = 0;
      bool const plain_counted =
          fragment_sample_total(plain_frags, &plain_per, &plain_total);
      note("对照（开关关、同样 " + std::to_string(flushes) + " 次 flush）：分片 " +
           std::to_string(plain_per.size()) + " 个，样本 " +
           (plain_counted ? std::to_string(plain_total)
                          : std::string("读不出")) +
           "，flush() 报错 " + std::to_string(flush_bad) + " 次，write 失败 " +
           std::to_string(writes_bad) + " 次");
      note("主路（开关开）：分片 " + std::to_string(shred_per.size()) + " 个，样本 " +
           std::to_string(shred_total));

      // ⚠️ **光比"分片数是几个"是不够的，这一条是量到之后才补上的。**
      // 第一次跑出来的读数是**两态都 47 个** —— 于是"两态相同"这句话既可以读成
      // "mov 不靠 frag_custom 也会在强制 flush 处切、所以这个开关是个空开关"，
      // 也可以读成"两边都切了 47 刀，但关掉开关那一侧的 47 片有一半是坏的"。
      // **分片数这一个数字分不出这两种**，而它们的结论正好相反。
      // 所以往下再量三件事，全都只报告：tfdt 单不单调、别人的解码器读不读得回、
      // 样本数对不对得上。**这一段仍然是只报告不断言**（理由见上面那段），
      // 但"只报告"不等于"只报一个数" —— 报一个分不出结论的数等于没报。
      std::vector<std::uint64_t> plain_tfdt;
      bool const plain_read = fragment_tfdts(plain_frags, &plain_tfdt);
      bool plain_ordered = plain_read && plain_tfdt.size() == plain_per.size();
      for (std::size_t i = 1; i < plain_tfdt.size() && plain_ordered; ++i) {
        if (plain_tfdt[i] <= plain_tfdt[i - 1]) plain_ordered = false;
      }
      note(std::string("对照的 tfdt：") +
           (plain_read ? (plain_ordered ? "逐片严格递增、与分片数对得上"
                                        : "**不是严格递增**（或条数与分片数不符）")
                       : "读不出"));

      std::string const kPlain = "out/fmp4_check_plain_control.mp4";
      std::string const plain_cmd = cmd_path(kPlain);
      std::vector<std::uint8_t> plain_file = init_seg;
      plain_file.insert(plain_file.end(), plain_frags.begin(), plain_frags.end());
      note(std::string("对照版写出 ") + kPlain + "：" +
           (write_file(kPlain, plain_file) ? "成功" : "失败") + "，共 " +
           std::to_string(plain_file.size()) + " 字节");

      std::string const pl_probe = run(
          kFfprobe +
          " -v error -show_entries stream=codec_name,width,height,nb_read_frames "
          "-count_frames -of default=nw=1 " +
          plain_cmd);
      int const pl_frames = std::atoi(field(pl_probe, "nb_read_frames").c_str());
      note("对照版 ffprobe：codec_name=" + field(pl_probe, "codec_name") + " " +
           field(pl_probe, "width") + "x" + field(pl_probe, "height") +
           " nb_read_frames=" + std::to_string(pl_frames) + "（源 " +
           std::to_string(src_frames) + "）");

      int pl_st = 0;
      std::string const pl_dec =
          run(kFfmpeg + " -v error -i " + plain_cmd + " -f null -" + " 2>&1", &pl_st);
      note(std::string("对照版整段解码：退出码 ") + std::to_string(pl_st) + "，" +
           (pl_dec.empty() ? std::string("零错误输出")
                           : std::to_string(pl_dec.size()) + " 字节输出"));
      if (!pl_dec.empty()) {
        std::printf("------- 对照版解码输出 -------\n%s"
                    "-----------------------------\n",
                    pl_dec.c_str());
      }

      note(plain_per.size() == shred_per.size()
               ? "=> 两态分片数相同：mov 不靠 frag_custom 也会在强制 flush 处切"
               : "=> 两态分片数不同：按调用方节奏切是 frag_custom 才有的");

      // 再往前一步：逐字节比对两个文件。它比"分片数相同"硬得多 —— 后者还能读成
      // "两边都切了 47 刀，只是一边走运写对了"，而字节相同没有第二种读法。
      // 头文件里 fragment_on_flush 那段写着的"逐字节相同"就是这一行量出来的，
      // 所以它必须落在测试里，不能只存在于某次手敲的 `cmp` 里。
      // 同样**只报告不断言**（理由见上面那段：它量的是 libavformat 的行为）。
      if (shred_file.empty()) {
        note("=> 两态逐字节比对：跳过（切碎那一侧的封装器没 init 成功，没有可比的东西）");
      } else {
        bool const same_bytes =
            shred_file.size() == plain_file.size() &&
            std::memcmp(shred_file.data(), plain_file.data(), shred_file.size()) == 0;
        note(std::string("=> 两态输出逐字节") + (same_bytes ? "相同" : "**不同**") +
             "（切碎 " + std::to_string(shred_file.size()) + " 字节 / 对照 " +
             std::to_string(plain_file.size()) + " 字节）" +
             (same_bytes ? "：这个开关在本版 FFmpeg 下是个空开关"
                         : "：这个开关确实改变了输出"));
      }

      note(std::string("=> 但「相同」只说明切了几刀；这一侧能不能读回来要看上面那三行：") +
           "tfdt " + (plain_read && plain_ordered ? "有序" : "**有问题**") +
           "，解出 " + std::to_string(pl_frames) + "/" +
           std::to_string(src_frames) + " 帧，解码" +
           (pl_st == 0 && pl_dec.empty() ? "干净" : "**有输出**"));
    } else {
      note("对照组的封装器 init 失败，这一次没量到 —— 结论仍是「未核实」");
    }
  }

  std::printf("\n=== 5. init 段 + 分片 => %s ===\n", kOut.c_str());
  std::vector<std::uint8_t> whole = init_seg;
  whole.insert(whole.end(), fragments.begin(), fragments.end());
  check(write_file(kOut, whole), "写出文件");
  note("文件总长 " + std::to_string(whole.size()) + " 字节");

  std::printf("\n=== 6. 用别人的解码器验一遍 ===\n");
  std::string const probe_cmd =
      kFfprobe +
      " -v error -show_entries "
      "stream=codec_name,width,height,avg_frame_rate,nb_read_frames "
      "-count_frames -of default=nw=1 " +
      out_cmd;
  note("$ " + probe_cmd);
  std::string const probe = run(probe_cmd);
  std::printf("------- ffprobe 输出 -------\n%s---------------------------\n",
              probe.c_str());
  check(field(probe, "codec_name") == "h264", "codec_name == h264");
  check(std::atoi(field(probe, "width").c_str()) == width &&
            std::atoi(field(probe, "height").c_str()) == height,
        "分辨率与源一致");
  int const out_frames = std::atoi(field(probe, "nb_read_frames").c_str());
  note("解出帧数 " + std::to_string(out_frames) + "（源 " +
       std::to_string(src_frames) + "）");
  check(out_frames == src_frames, "解出的帧数和喂进去的一样多");

  std::string const dec_cmd =
      kFfmpeg + " -v error -i " + out_cmd + " -f null -";
  note("$ " + dec_cmd);
  std::string const dec_log = run(dec_cmd + " 2>&1", &st);
  if (!dec_log.empty()) std::printf("------- ffmpeg 输出 -------\n%s"
                                    "---------------------------\n",
                                    dec_log.c_str());
  // "concealing N DC/AC/MV errors" 是最阴的一种失败：文件能播、ffprobe 报的
  // 分辨率和帧率全对，只有解码器自己知道画面是糊的。所以这里要求输出**为空**，
  // 而不是"退出码为 0"。
  check(st == 0 && dec_log.empty(),
        "整段解码零错误输出（没有任何 concealing/warning）");
  if (st == 0 && dec_log.empty()) std::printf("DECODE OK\n");

  std::printf("\n=== 结论 ===\n");
  if (g_failures == 0) {
    std::printf("全部通过。init 段 %zu 字节，分片 %zu 字节，%d 次 write() 产出。\n",
                init_seg.size(), fragments.size(), calls_with_output);
    return 0;
  }
  std::printf("失败 %d 项。\n", g_failures);
  return 1;
}
