/**
 * @file tests/latency_tier_check.cpp
 * @brief Phase W 的主判据：延时档位展开出来的 `threads` 真的变成了编码器的
 *        **流水线深度**。
 *
 * 为什么需要它。延时档位看起来只是"界面上多一个下拉框"，但它要成立必须满足
 * 一条**可观测**的关系：帧级并行下 x264 的编码流水线深度等于线程数，而"深度"
 * 是有定义的 —— **提交了第 K 帧之后才吐出第一个包**。这个量不需要采集设备、
 * 不受机器负载影响、也不看墙钟，所以它是这一档唯一一条能当判据的量。
 *
 * 反过来，如果只验"POST 了 realtime 之后配置文件里 threads == 1"，验的是配置层
 * 自己（`tests/web_contract.py` 那条已经盖住了），而"threads 到了编码器之后真的
 * 少吃了延迟"一个字都没验。这个项目已经在同一个形状上翻过车：`capture.tune`
 * 曾经是一句空话（陷阱 28）—— 写得进去、读得出来、没有一行代码用它。
 * 所以这里**刻意走产品的那条路**（档位 key → validate_config 展开 →
 * capture.threads / capture.gop_seconds → EncoderConfig），而不是直接
 * `ec.threads = tier->threads` 把中间那一段跳过去。
 *
 * 喂的是合成的 BGRA 小帧（320x240），与真实桌面无关 —— 这里量的是流水线深度，
 * 不是画质，也不是码率。
 *
 * 编译（在 D:\tools\Desk2ONVIF 下，PATH 里要有 C:\msys64\mingw64\bin，
 * 否则 cc1 会静默死掉，陷阱 18）：
 *
 *   g++ -std=c++17 -Isrc -Ithird_party/ffmpeg/include tests/latency_tier_check.cpp \
 *       -o build/latency_tier_check.exe \
 *       build/lib/libdesk2onvif_core.a \
 *       -Lthird_party/ffmpeg/lib -lavformat -lavcodec -lswscale -lswresample -lavutil \
 *       -Wl,--start-group third_party/live555/lib/libliveMedia.a \
 *           third_party/live555/lib/libgroupsock.a \
 *           third_party/live555/lib/libBasicUsageEnvironment.a \
 *           third_party/live555/lib/libUsageEnvironment.a -Wl,--end-group \
 *       -Lthird_party/uvcpp/lib -luvcpp -luv \
 *       C:/msys64/mingw64/lib/libssl.dll.a C:/msys64/mingw64/lib/libcrypto.dll.a \
 *       -ld3d11 -ldxgi -liphlpapi -lgdi32 -ladvapi32 -lwtsapi32 -luserenv -lshell32 \
 *       -lole32 -lmmdevapi -lavrt -lws2_32 -lwinmm -loleaut32 -luuid -lcomdlg32
 *
 * 跑（DLL 在 build/bin，所以 PATH 要带上 third_party\ffmpeg\bin 和 mingw64\bin）。
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "common/config.h"
#include "encoder/video_encoder.h"

using namespace d2o;

namespace {

int g_pass = 0;
int g_fail = 0;

void check(bool ok, std::string const& what) {
  if (ok) {
    ++g_pass;
    std::printf("[OK]   %s\n", what.c_str());
  } else {
    ++g_fail;
    std::printf("[FAIL] %s\n", what.c_str());
  }
}

std::string num(long long v) { return std::to_string(v); }

constexpr int kW = 320;
constexpr int kH = 240;

/// 一块在动的棋盘格 + 每帧都变的伪随机噪声。**每帧都要真的不一样**，
/// 否则编码器会走"内容没变、复用上一帧 YUV"那条路，P 帧小到看不出流水线行为
/// （手法照 tests/x264_reconfig_check.cpp）。
void fill_frame(BgraFrame& f, int i) {
  std::uint8_t* p = f.pixels.data();
  for (int y = 0; y < kH; ++y) {
    for (int x = 0; x < kW; ++x) {
      std::uint8_t* q = p + (static_cast<std::size_t>(y) * f.stride +
                             static_cast<std::size_t>(x) * 4);
      int const block = ((x / 32 + y / 32 + i) % 2) ? 200 : 40;
      q[0] = static_cast<std::uint8_t>(block);
      q[1] = static_cast<std::uint8_t>((x * 3 + i * 7) & 0xFF);
      q[2] = static_cast<std::uint8_t>((y * 5 + i * 11) & 0xFF);
      q[3] = 255;
    }
  }
  f.content_id = static_cast<std::uint64_t>(i) + 1;
}

/// 把一份配置按某个档位展开 —— 走的就是产品的那条路。
AppConfig expand(std::string const& latency_key, std::vector<std::string>* notes) {
  AppConfig cfg;
  cfg.capture.latency = latency_key;
  // 先污染成档位绝不会给的值：这样"展开真的发生了"和"值本来就是对的"分得开。
  cfg.capture.threads = 0;
  cfg.capture.gop_seconds = 3;
  std::vector<std::string> local;
  validate_config(cfg, notes ? *notes : local);
  return cfg;
}

struct DepthResult {
  bool ok = false;
  std::string err;
  int threads_used = 0;      ///< 配置里那个值（还没被核数夹过）
  int first_packet_at = -1;  ///< 吐出第一个包的那一刻，已经提交了多少帧
  int packets = 0;
  int gop_size = 0;
  int fps = 0;
};

/// 造一个真编码器，喂合成帧，记下"第一个包是在提交到第几帧时出来的"。
DepthResult measure_depth(std::string const& latency_key, int max_frames) {
  DepthResult r;

  AppConfig const cfg = expand(latency_key, nullptr);

  EncoderConfig ec;
  ec.width = kW;
  ec.height = kH;
  ec.fps = cfg.capture.fps;
  ec.bitrate_kbps = 2048;
  ec.threads = cfg.capture.threads;
  ec.gop_seconds = cfg.capture.gop_seconds;
  r.gop_size = ec.gop_size();
  r.fps = ec.fps;
  r.threads_used = ec.threads;

  X264Encoder enc;
  std::string err;
  if (!enc.open(ec, err)) {
    r.err = "open 失败：" + err;
    return r;
  }

  BgraFrame f;
  f.width = kW;
  f.height = kH;
  f.stride = kW * 4;
  f.pixels.assign(static_cast<std::size_t>(f.stride) * kH, 0);

  int submitted = 0;
  for (int i = 0; i < max_frames; ++i) {
    fill_frame(f, i);
    ++submitted;
    bool const ok = enc.encode(
        f, i, true,
        [&r, submitted](AVPacket&) {
          ++r.packets;
          if (r.first_packet_at < 0) r.first_packet_at = submitted;
        },
        err);
    if (!ok) {
      r.err = "encode 失败：" + err;
      enc.close();
      return r;
    }
    if (r.first_packet_at >= 0) break;
  }
  enc.close();
  r.ok = true;
  return r;
}

/// [1] 档位表本身。
void test_table() {
  std::printf("---- [1] 档位表 ----\n");
  auto const& tiers = latency_tiers();
  check(tiers.size() == 3, "档位表是三档，实测 " + num(static_cast<long long>(tiers.size())));

  struct Want {
    char const* key;
    int threads;
    int gop_seconds;
  };
  Want const want[] = {{"realtime", 1, 1}, {"balanced", 3, 2}, {"quality", 6, 4}};
  for (auto const& w : want) {
    LatencyTier const* t = find_latency_tier(w.key);
    if (!t) {
      check(false, std::string("档位表里有 ") + w.key);
      continue;
    }
    check(t->threads == w.threads && t->gop_seconds == w.gop_seconds,
          std::string(w.key) + " 是 threads=" + num(w.threads) + " / gop=" +
              num(w.gop_seconds) + " 秒，实测 " + num(t->threads) + " / " +
              num(t->gop_seconds));
    check(t->label && *t->label, std::string(w.key) + " 有中文名（界面直接显示）");
  }

  // 顺序：从延迟最低到最高。界面按这个顺序排，所以它是有语义的，不只是美观。
  bool ordered = true;
  for (std::size_t i = 1; i < tiers.size(); ++i) {
    if (tiers[i].threads < tiers[i - 1].threads) ordered = false;
  }
  check(ordered, "档位表从延迟最低排到最高（threads 单调不减）");

  // 名字刻意**不是**「极致0延时」—— 那个名字会被量出来证伪（开音频地板 ~320 ms）。
  bool no_zero_name = true;
  for (auto const& t : tiers) {
    if (std::string(t.label).find("0延时") != std::string::npos) no_zero_name = false;
  }
  check(no_zero_name, "没有哪一档挂着「0延时」这种会被实测证伪的名字");

  check(find_latency_tier("custom") == nullptr, "find_latency_tier(\"custom\") 回 nullptr");
  check(find_latency_tier("zero") == nullptr,
        "find_latency_tier(\"zero\") 回 nullptr（不认识的值不走档位表）");
}

/// [2] validate_config 的展开与"不认识的值要说话"。
void test_expand() {
  std::printf("---- [2] 档位展开 ----\n");
  for (auto const& t : latency_tiers()) {
    std::vector<std::string> notes;
    AppConfig const c = expand(t.key, &notes);
    check(c.capture.threads == t.threads,
          std::string(t.key) + " 展开后 capture.threads == " + num(t.threads) +
              "（实测 " + num(c.capture.threads) + "）");
    check(c.capture.gop_seconds == t.gop_seconds,
          std::string(t.key) + " 展开后 capture.gop_seconds == " + num(t.gop_seconds) +
              "（实测 " + num(c.capture.gop_seconds) + "）");
    check(!notes.empty(), std::string(t.key) + " 展开改了值就记了一条 note（不静默改）");
  }

  // custom = 谁都不动。
  {
    AppConfig cfg;
    cfg.capture.latency = "custom";
    cfg.capture.threads = 7;
    cfg.capture.gop_seconds = 9;
    std::vector<std::string> notes;
    validate_config(cfg, notes);
    check(cfg.capture.threads == 7 && cfg.capture.gop_seconds == 9,
          "custom 一个字段都不动（threads 仍是 7、gop 仍是 9）");
  }

  // 不认识的值：要**说出来**再改成 custom，不能静默当成 custom ——
  // 静默的那种，用户会以为档位在管着，实际一个字段都没被管。
  {
    AppConfig cfg;
    cfg.capture.latency = "zero";
    std::vector<std::string> notes;
    validate_config(cfg, notes);
    check(cfg.capture.latency == "custom", "不认识的档位值被改成 custom");
    bool listed = false;
    for (auto const& n : notes) {
      if (n.find("realtime") != std::string::npos &&
          n.find("balanced") != std::string::npos &&
          n.find("quality") != std::string::npos) {
        listed = true;
      }
    }
    check(listed, "那条 note 里列出了合法值（realtime / balanced / quality）");
  }

  // preset / tune / profile **三档一律不动**（陷阱 20：放松 preset 会把
  // H.264 profile 从 Constrained Baseline 换成 Main，而 NVR 兼容性不在档位表上）。
  {
    std::string const P = "veryfast", T = "film", R = "high";
    bool untouched = true;
    for (auto const& t : latency_tiers()) {
      AppConfig cfg;
      cfg.capture.preset = P;
      cfg.capture.tune = T;
      cfg.capture.profile = R;
      cfg.capture.latency = t.key;
      std::vector<std::string> notes;
      validate_config(cfg, notes);
      if (cfg.capture.preset != P || cfg.capture.tune != T || cfg.capture.profile != R) {
        untouched = false;
      }
    }
    check(untouched, "三档都不动 preset / tune / profile（用 veryfast/film/high 当哨兵验的）");
  }
}

/// [3] 与画质档位正交 —— 两组字段同时落对、互不覆盖。
void test_orthogonal() {
  std::printf("---- [3] 与画质档位正交 ----\n");
  QualityTier const* smooth = find_quality_tier("smooth");
  if (!smooth) {
    check(false, "画质档位表里有 smooth");
    return;
  }
  {
    AppConfig cfg;
    cfg.capture.quality = "smooth";
    cfg.capture.latency = "realtime";
    cfg.capture.threads = 0;
    cfg.capture.gop_seconds = 3;
    std::vector<std::string> notes;
    validate_config(cfg, notes);
    check(cfg.capture.fps == smooth->fps && cfg.capture.bitrate_kbps == smooth->bitrate_kbps &&
              cfg.capture.max_height == smooth->max_height,
          "smooth + realtime：画质那三项落成 smooth 的值（fps=" + num(smooth->fps) +
              "，实测 " + num(cfg.capture.fps) + "）");
    check(cfg.capture.threads == 1 && cfg.capture.gop_seconds == 1,
          "smooth + realtime：延时那两项同时落成 realtime 的值（实测 threads=" +
              num(cfg.capture.threads) + " / gop=" + num(cfg.capture.gop_seconds) + "）");
  }
  {
    // 反方向再走一次：只验一组的话，"后一段把前一段整个覆盖掉"这种错看不出来。
    AppConfig cfg;
    cfg.capture.quality = "smooth";
    cfg.capture.latency = "quality";
    cfg.capture.threads = 0;
    cfg.capture.gop_seconds = 3;
    std::vector<std::string> notes;
    validate_config(cfg, notes);
    check(cfg.capture.fps == smooth->fps && cfg.capture.threads == 6 &&
              cfg.capture.gop_seconds == 4,
          "smooth + quality：fps 仍是 " + num(smooth->fps) + "、threads=6、gop=4（实测 " +
              num(cfg.capture.fps) + " / " + num(cfg.capture.threads) + " / " +
              num(cfg.capture.gop_seconds) + "）");
  }
}

/// [4] 主判据：流水线深度。
void test_depth() {
  std::printf("---- [4] 流水线深度（主判据）----\n");
  unsigned const cores = std::thread::hardware_concurrency();
  int const cores_i = cores > 0 ? static_cast<int>(cores) : 0;
  std::printf("     逻辑核数 = %d（x264_encoder.cpp 会把 threads 夹到它，"
              "所以下面判的是 min(档位, 核数)）\n", cores_i);

  struct Row {
    std::string key;
    int expect;
    DepthResult r;
  };
  std::vector<Row> rows;
  for (auto const& t : latency_tiers()) {
    Row row;
    row.key = t.key;
    row.expect = cores_i > 0 ? std::min(t.threads, cores_i) : t.threads;
    row.r = measure_depth(t.key, 40);
    std::printf("     %-9s threads=%-2d gop_size=%-4d -> 提交第 %d 帧时吐出第一个包"
                "（共 %d 包）\n",
                row.key.c_str(), row.r.threads_used, row.r.gop_size,
                row.r.first_packet_at, row.r.packets);
    rows.push_back(std::move(row));
  }

  for (auto const& row : rows) {
    if (!row.r.ok) {
      check(false, row.key + " 的编码器跑得起来（" + row.r.err + "）");
      continue;
    }
    check(row.r.first_packet_at == row.expect,
          row.key + " 的流水线深度 == " + num(row.expect) + " 帧（实测 " +
              num(row.r.first_packet_at) + "）");
    // gop_seconds 落成 fps * 该档秒数。这一条与 threads 无关，是同一个档位的
    // 第二个旋钮 —— "多久能看到第一帧"（分片只在关键帧处产生）。
    LatencyTier const* t = find_latency_tier(row.key);
    check(t != nullptr && row.r.gop_size == row.r.fps * t->gop_seconds,
          row.key + " 的 gop_size == fps(" + num(row.r.fps) + ") × " +
              num(t ? t->gop_seconds : -1) + " = " + num(row.r.gop_size));
  }

  // 纵向可比：极低延迟档的流水线必须真的比优先画质档浅。只验三个绝对值的话，
  // "全都等于 1"（= threads 根本没传到编码器）也会通过其中一条。
  if (rows.size() == 3 && rows[0].r.ok && rows[2].r.ok) {
    check(rows[0].r.first_packet_at < rows[2].r.first_packet_at,
          "realtime 的流水线比 quality 浅（" + num(rows[0].r.first_packet_at) +
              " < " + num(rows[2].r.first_packet_at) + "）");
  }
}

}  // namespace

int main() {
  std::printf("=== 延时档位（Phase W）===\n");
  test_table();
  test_expand();
  test_orthogonal();
  test_depth();
  std::printf("----\n通过 %d 项，失败 %d 项。\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
