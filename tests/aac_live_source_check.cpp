/**
 * @file tests/aac_live_source_check.cpp
 * @brief `AacLiveSource` 的呈现时间：**首包 pts 是负的**这件事会不会把时间轴搞乱。
 *
 * ## 为什么这条只能单元测，端到端量不出来
 *
 * 音频首包的 pts 实测是 **−1024**（编码器的启动填充），而视频那一路的首帧号是
 * 0 —— 所以视频那边可以拿 `anchor_pts_ < 0` 当"还没定过锚"的哨兵，音频这边
 * **不行**。这条差异是陷阱 85 意义上的"必须真的量一遍"：
 *
 * 实测过（把 `aac_live_source.cpp` 里的 `anchored_` 换回 `if (anchor_pts_ < 0)`
 * 那个哨兵写法、重编、再跑 `tests\rtsp_audio_check.ps1`）：**整条端到端照样全绿**，
 * 解出来的仍是 6.000 秒、1000.0 Hz、逐格均匀。原因是播放器是在服务起来之后
 * ~0.5 秒才连上来的，那时**第一个被投递的** AU 的 pts 早就不是负数了，哨兵根本
 * 没机会出错。也就是说这个缺陷在 e2e 里是**量不出来的**。
 *
 * 而它确实是错的：客户端要是赶在编码器第二个包之前接上（NVR 就是会这么干 ——
 * 服务一起来就拉），第一个投递的 pts 就是 −1024，哨兵于是把锚点定到 −1024，
 * 下一个包 pts = 0 时 `anchor_pts_ < 0` **仍然成立**，锚点又被重定到 0 ⇒
 * **前两个 AU 拿到同一个呈现时间**。差 21 ms，量级小，但形状是"两个 RTP 包
 * 时间戳相同"，而它本来可以完全不存在。
 *
 * 所以这里直接把队列填成"编码器刚启动时的样子"（pts = −1024, 0, 1024, …），
 * 一个一个拉，逐个检查 `fPresentationTime`：必须**严格递增**、间距恰好是一个
 * AU（1024 样本 / 48000 Hz = 21333 µs）。
 *
 * 顺带盯住三件同一层的事：
 *   - 包的内容是不是原样进到 `fTo`（`memmove` 那一步的偏移写错会静默出噪音）
 *   - 缓冲不够时必须走 `fNumTruncatedBytes`，而不是越界写
 *   - 队列空时**不能**投递（投了就等于凭空造一帧 —— 拉模型里最脏的一种错），
 *     而 `wake()` 那一侧要真的能把调度器叫起来
 *
 * ## 为什么不用跑事件循环就能测
 *
 * `deliver()` 在队列非空时是**就地**投递的（`FramedSource::afterGetting` 在
 * `getNextFrame` 返回之前就被调用），所以主序列不需要调度器。[1] 那一段是例外：
 * "队列空 → 之后 push + wake() → 投出来了"这条正是 `triggerEvent` 那条路，
 * 必须真的跑一小段 `doEventLoop` 才验得到。
 *
 * 编译（在项目根目录下跑）。OpenSSL 那两行**不是可选项**：libliveMedia 把
 * TLS/SRTP 无条件编了进去，链到 `MediaSink.o` 那一串就会把 `TLSState.o` /
 * `SRTPCryptographicContext.o` / `HMAC_SHA1.o` 一起拉进来（陷阱 17）。
 * 必须排在 live555 四个库**之后**，否则 --start-group 里那一圈解不开：
 *   g++ -std=c++17 -Wall -Wextra -Isrc -Ithird_party/live555/include \
 *       tests/aac_live_source_check.cpp \
 *       src/rtsp/aac_live_source.cpp src/common/log.cpp \
 *       -o build/aac_live_source_check.exe \
 *       -Lthird_party/live555/lib -Wl,--start-group -lliveMedia -lgroupsock \
 *           -lBasicUsageEnvironment -lUsageEnvironment -Wl,--end-group \
 *       C:/msys64/mingw64/lib/libssl.dll.a C:/msys64/mingw64/lib/libcrypto.dll.a \
 *       -lws2_32
 *
 * 运行（Live555 的运行期依赖只有 ws2_32，没有别的 DLL 要找）：
 *   ./build/aac_live_source_check.exe
 */

#include "rtsp/aac_live_source.h"
#include "rtsp/au_queue.h"

#include "BasicUsageEnvironment.hh"
#include "FramedSource.hh"
#include "Media.hh"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

using namespace d2o;

namespace {

int g_failed = 0;
int g_checks = 0;

void check(bool ok, std::string const& what) {
  ++g_checks;
  if (ok) {
    std::printf("  [OK]   %s\n", what.c_str());
  } else {
    std::printf("  [FAIL] %s\n", what.c_str());
    ++g_failed;
  }
}

constexpr unsigned kSampleRate = 48000;
constexpr unsigned kSamplesPerAu = 1024;
/// 一个 AU 的时长：1024 / 48000 秒 = 21333.3 µs（整除后是 21333）。
constexpr int kAuUs = static_cast<int>(static_cast<std::int64_t>(kSamplesPerAu) *
                                       1'000'000 / kSampleRate);

/// 一次"拉一帧"的结果。`getNextFrame` 在队列非空时是同步回调的，所以这里拿到的
/// 就是投递那一刻的字段。
struct Pull {
  bool got = false;
  unsigned frame_size = 0;
  unsigned truncated = 0;
  unsigned duration_us = 0;
  long sec = 0;
  long usec = 0;
};

void after_getting(void* clientData, unsigned frameSize, unsigned numTruncatedBytes,
                   struct timeval presentationTime, unsigned durationInMicroseconds) {
  auto* p = static_cast<Pull*>(clientData);
  p->got = true;
  p->frame_size = frameSize;
  p->truncated = numTruncatedBytes;
  p->duration_us = durationInMicroseconds;
  p->sec = presentationTime.tv_sec;
  p->usec = presentationTime.tv_usec;
}

/// 呈现时间的绝对值（µs）。tv_sec 是**挂钟**（锚点是构造时刻），约 1.8e9 秒，
/// 乘 1e6 之后仍在 int64 里。
std::int64_t to_us(Pull const& p) {
  return static_cast<std::int64_t>(p.sec) * 1'000'000 + p.usec;
}

/// 造一个 AU。内容是可识别的（首字节跟着序号走），这样"拷贝偏移写错"一眼能看出来。
Au make_au(int index, std::size_t size) {
  Au au;
  au.data.resize(size);
  for (std::size_t i = 0; i < size; ++i) {
    au.data[i] = static_cast<std::uint8_t>((index * 31 + static_cast<int>(i)) & 0xFF);
  }
  // 编码器的第一个包 pts = −1024，之后每包 +1024（实测；见 aac_encoder.h）。
  au.pts = -static_cast<std::int64_t>(kSamplesPerAu) +
           static_cast<std::int64_t>(index) * kSamplesPerAu;
  return au;
}

/// 拉一帧（队列非空时同步投递）。
Pull pull_one(AacLiveSource* src, std::vector<std::uint8_t>& buf) {
  Pull p;
  src->getNextFrame(buf.data(), static_cast<unsigned>(buf.size()), &after_getting, &p,
                    nullptr, nullptr);
  return p;
}

/// 跑一小段事件循环。`wake()` 只置一个位（`triggerEvent` 是全库唯一允许跨线程
/// 调用的入口），真正派发得有人跑 `doEventLoop` —— 这里就是扮演那个调度线程。
void pump(TaskScheduler& sched, int ms) {
  EventLoopWatchVariable stop(0);
  sched.scheduleDelayedTask(static_cast<std::int64_t>(ms) * 1000,
                            [](void* cd) {
                              static_cast<EventLoopWatchVariable*>(cd)->store(1);
                            },
                            &stop);
  sched.doEventLoop(&stop);
}

}  // namespace

int main() {
  // Live555 的一套最小环境。**不用调度器**也能测主序列，理由见文件头。
  TaskScheduler* scheduler = BasicTaskScheduler::createNew();
  UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);

  // ------------------------------------------------------------------
  // [1] 队列空时什么都不投；push + wake() 之后要真的投出来
  //
  // 这一段用**独立的**队列和 source：它会把 source 留在"正在等数据"的状态上，
  // 而 Live555 的契约是同一个 source 不能同时有两次 getNextFrame（第二次会
  // `internalError()` 把进程直接结束掉）。让主序列从干净的 source 开始，免得
  // 这里的状态影响后面那几条时间戳断言。
  // ------------------------------------------------------------------
  std::printf("[1] 空队列不投递；push + wake() 之后投出来\n");
  {
    auto q = std::make_shared<AuQueue>(8);
    auto* s1 = AacLiveSource::createNew(*env, q, kSampleRate, kSamplesPerAu);
    std::vector<std::uint8_t> buf(2048);

    Pull p = pull_one(s1, buf);
    check(!p.got, "队列空时不投递（没调 afterGetting）");
    // 这条是拉模型的核心约定：没数据不是"投一帧空的"，而是**停在原地等**。
    // 投了空帧的话收端会拿到一段凭空造出来的静音。
    check(s1->isCurrentlyAwaitingData(), "……而且停在「有人在等」这个状态上");

    q->set_armed(true);
    q->push(make_au(0, 64));
    s1->wake();
    pump(*scheduler, 200);
    check(p.got, "wake() 之后调度器一转，那一帧就投出来了（triggerEvent 那条路是通的）");
    check(p.frame_size == 64, "投出来的正是推进去的那 64 字节");
    check(q->size() == 0, "队列被取空了");

    p.got = false;
    q->clear();
    s1->wake();
    pump(*scheduler, 100);
    check(!p.got, "队列空时 wake() 不会凭空投一帧");

    s1->stopGettingFrames();  // 解掉待读状态，close 的时候不挂着
    Medium::close(s1);
  }

  // ------------------------------------------------------------------
  // [2] 未武装时入队等于丢弃
  // ------------------------------------------------------------------
  std::printf("[2] 没人在看时 AU 不入队\n");
  auto queue = std::make_shared<AuQueue>(96);
  {
    unsigned const dropped = queue->push(make_au(0, 64));
    check(dropped == 0 && queue->size() == 0,
          "没人在看时 AU 不入队（没人 SETUP 音频时连这次拷贝都不必做）");
  }

  // ------------------------------------------------------------------
  // [3] 首包 pts = −1024 时，头两个 AU 的呈现时间必须不同
  // ------------------------------------------------------------------
  std::printf("[3] 首包 pts = −1024 时，头两个 AU 的呈现时间必须不同\n");
  queue->set_armed(true);
  constexpr int kCount = 6;
  for (int i = 0; i < kCount; ++i) {
    queue->push(make_au(i, 128));
  }
  check(queue->size() == kCount, "六个 AU 都进了队列");

  auto* src = AacLiveSource::createNew(*env, queue, kSampleRate, kSamplesPerAu);
  std::vector<Pull> pulls(kCount);
  std::vector<std::vector<std::uint8_t>> raw(kCount, std::vector<std::uint8_t>(2048));
  for (int i = 0; i < kCount; ++i) {
    pulls[i] = pull_one(src, raw[i]);
    check(pulls[i].got, "第 " + std::to_string(i) + " 次拉到数据");
  }

  // **这一条就是这个文件存在的理由。** 锚点要是用"首包 pts 是不是负的"当判据，
  // 第 0 个和第 1 个 AU 会拿到同一个呈现时间。
  bool strict = true;
  for (int i = 1; i < kCount; ++i) {
    if (to_us(pulls[i]) <= to_us(pulls[i - 1])) strict = false;
  }
  std::printf("       首包 %ld.%06ld，第二包 %ld.%06ld，末包 %ld.%06ld\n",
              pulls[0].sec, pulls[0].usec, pulls[1].sec, pulls[1].usec,
              pulls[kCount - 1].sec, pulls[kCount - 1].usec);
  check(strict, "六次投递的呈现时间严格递增（首包是负数也不能并到同一时刻）");

  // 逐包对**绝对**偏差：第 i 包相对首包必须恰好是 floor(i*1024*1e6/48000) µs。
  // 这是代码里那一步整除的确切结果，也是"锚点有没有跟着包跑"最直接的判据
  // ——锚点被重定的话这里会整体错位。
  bool spacing = true;
  for (int i = 1; i < kCount; ++i) {
    std::int64_t const want = static_cast<std::int64_t>(i) * kSamplesPerAu *
                              1'000'000 / kSampleRate;
    if (to_us(pulls[i]) - to_us(pulls[0]) != want) spacing = false;
  }
  check(spacing, "第 i 包相对首包恰好是 " + std::to_string(kAuUs) + "×i µs（不多不少）");

  // 单步间隔只能落在 21333 或 21334 上：1024 样本 / 48000 Hz = 21333.33 µs 除不尽，
  // 余下的那 1 µs 落在哪一步都合法。**判"每步都等于 21333"会把一个正确的实现
  // 报成失败** —— 第一版就是这么写的，实测第 3 步落在 21334 上。
  bool step_ok = true;
  int min_step = 1 << 30, max_step = 0;
  for (int i = 1; i < kCount; ++i) {
    int const d = static_cast<int>(to_us(pulls[i]) - to_us(pulls[i - 1]));
    if (d < min_step) min_step = d;
    if (d > max_step) max_step = d;
    if (d != kAuUs && d != kAuUs + 1) step_ok = false;
  }
  check(step_ok, "单步间隔全落在 " + std::to_string(kAuUs) + "~" +
                     std::to_string(kAuUs + 1) + " µs（实测 " +
                     std::to_string(min_step) + "~" + std::to_string(max_step) + "）");

  check(pulls[0].duration_us == static_cast<unsigned>(kAuUs),
        "fDurationInMicroseconds = " + std::to_string(pulls[0].duration_us) +
            "（发送节奏靠它：MultiFramedRTPSink 每包把它加进 fNextSendTime，"
            "给 0 会一口气全推出去）");

  // ------------------------------------------------------------------
  // [4] 内容原样过去，一个字节都不能错位
  // ------------------------------------------------------------------
  std::printf("[4] 内容原样过去，一个字节都不能错位\n");
  {
    bool same = true;
    int bad = -1;
    for (int i = 0; i < kCount && same; ++i) {
      Au const expected = make_au(i, 128);
      if (pulls[i].frame_size != 128 || pulls[i].truncated != 0) {
        same = false;
        bad = i;
        break;
      }
      for (std::size_t k = 0; k < 128; ++k) {
        if (raw[i][k] != expected.data[k]) {
          same = false;
          bad = i;
          break;
        }
      }
    }
    if (bad >= 0) {
      std::printf("       第 %d 包对不上（frame_size=%u truncated=%u）\n", bad,
                  pulls[bad].frame_size, pulls[bad].truncated);
    }
    check(same, "六包的内容与推入的完全一致");
  }

  // ------------------------------------------------------------------
  // [5] 缓冲不够时截断，而不是越界写
  // ------------------------------------------------------------------
  std::printf("[5] 缓冲不够时截断，而不是越界写\n");
  {
    queue->clear();
    queue->push(make_au(9, 4096));
    std::vector<std::uint8_t> small(256);
    Pull p = pull_one(src, small);
    check(p.got && p.frame_size == 256 && p.truncated == 4096 - 256,
          "投递 256 字节 + 声明截断 " + std::to_string(p.truncated) + " 字节");
  }

  // ------------------------------------------------------------------
  // [6] 队列深度有界，满了丢最旧的
  // ------------------------------------------------------------------
  std::printf("[6] 队列深度有界，满了丢最旧的\n");
  {
    auto q2 = std::make_shared<AuQueue>(4);
    q2->set_armed(true);
    unsigned dropped = 0;
    for (int i = 0; i < 7; ++i) dropped += q2->push(make_au(i, 64));
    check(q2->size() == 4 && dropped == 3,
          "推 7 个进深度 4 的队列：留下 4 个、丢弃 " + std::to_string(dropped) +
              " 个（丢最旧，和视频那一路同一个哲学）");
  }

  // ------------------------------------------------------------------
  // [7] 停机：disarm 之后队列清空
  // ------------------------------------------------------------------
  std::printf("[7] 停机：disarm 之后队列清空\n");
  {
    queue->set_armed(false);
    check(queue->size() == 0, "set_armed(false) 把队列清干净");
    queue->push(make_au(0, 64));
    check(queue->size() == 0, "disarm 之后 push 是空操作");
  }

  src->stopGettingFrames();
  Medium::close(src);
  // reclaim() 返回 False 说明还有 Medium 挂着（就是没 close 干净）。
  // 这条值一个断言：EventTrigger 是有限资源（32 个），泄漏起来是"用久了突然
  // 静音"，没有任何报错。
  Boolean const clean = env->reclaim();
  check(clean != 0, "reclaim() 说没有残留的 Medium（EventTrigger 都还回去了）");
  delete scheduler;

  std::printf("----------------------------------------------------------------\n");
  if (g_failed > 0) {
    std::printf("失败 %d / %d 项\n", g_failed, g_checks);
    return 1;
  }
  std::printf("全部通过（%d 项）。\n", g_checks);
  return 0;
}
