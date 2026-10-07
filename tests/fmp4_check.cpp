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

  std::printf("\n=== 4. 逐样本写入 ===\n");
  std::vector<std::uint8_t> fragments;
  int calls_with_output = 0;
  int failed = 0;
  std::vector<std::size_t> per_call;  // 每次调用新增的字节数（只记非零的）
  for (std::size_t i = 0; i < aus.size(); ++i) {
    d2o::Fmp4Sample s;
    s.data = src.data() + aus[i].offset;
    s.size = aus[i].size;
    s.pts_us = static_cast<std::int64_t>(i) * 1000000 / fps;
    s.dts_us = s.pts_us;
    s.keyframe = aus[i].keyframe;
    std::size_t const before = fragments.size();
    if (!muxer.write(s, fragments)) ++failed;
    std::size_t const added = fragments.size() - before;
    if (added > 0) {
      ++calls_with_output;
      per_call.push_back(added);
    }
    if ((i + 1) % 60 == 0 || i + 1 == aus.size()) {
      std::printf("       喂到第 %3zu 帧：累计分片 %zu 字节\n", i + 1,
                  fragments.size());
    }
  }
  check(failed == 0, "所有样本都写进去了（没有 write() 返回 false）");

  // 到这里最后一个 GOP 还压在封装器里：mov 要等**下一个**关键帧才知道上一个
  // 分片在哪结束。直播流里不存在"最后一个 GOP"（调用方会一直喂下去），所以
  // 模块本身没有 flush —— 接口里也没有这个位置。
  //
  // 但"尾部的分片到底对不对"是必须验的（trun 里的 sample count/duration、
  // 最后一个样本的时长、mdat 长度都在里面），所以这里补喂一个关键帧把尾巴顶
  // 出来：mov 在收下它之前会先 flush 上一个分片，而这个补喂的样本自己会留在
  // 待发状态，不会进文件。文件里因此正好是喂进去的那 300 帧。
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
    s.pts_us = static_cast<std::int64_t>(aus.size()) * 1000000 / fps;
    s.dts_us = s.pts_us;
    s.keyframe = true;
    std::size_t const before = fragments.size();
    if (!muxer.write(s, fragments)) ++failed;
    std::size_t const added = fragments.size() - before;
    note("补喂一个关键帧，顶出最后一个 GOP：" + std::to_string(added) + " 字节" +
         "（这一个样本自己不会进文件）");
    if (added > 0) {
      ++calls_with_output;
      per_call.push_back(added);
    }
    check(added > 0, "最后一个 GOP 被下一个关键帧顶了出来");
  }

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
