/**
 * @file tests/mjpeg_check.cpp
 * @brief MjpegEncoder 的独立验证程序：不解码、只看字节是验不出来的。
 *
 * 五件事必须验到，因为它们的失败模式都是"图还是能看，只是不对"：
 *
 *   1. SOI/EOI —— 少了 EOI 的 JPEG 有些解码器照样能出图，只是会报 warning；
 *   2. 带 stride 的输入与紧凑输入**逐字节相同** —— 这是行距处理的唯一硬证据。
 *      按 width*4 硬拷的实现会沿着对角线斜切，而 JPEG 的头、尺寸、能否解码
 *      全都正常，只有肉眼看得见；
 *   3. 三档 qscale 的体积必须不同 —— frame->quality 的单位是 lambda 不是
 *      qscale，写错了会让所有档位塌成同一档（见 mjpeg_encoder.cpp 里的说明）；
 *   4. 同一个对象重新 init 到别的分辨率后仍能编 —— NVR 改分辨率走的就是这条路；
 *   5. 编出来的图不能是"一张几乎全空的 jpg" —— 全同色画面的 JPEG 只有几百字节，
 *      即使编码器完全忽略了输入像素也能过前四条。
 *
 * 编译与运行见本文件末尾的说明（或用 scripts 里的命令行，不用 cmake）。
 */

#include "encoder/mjpeg_encoder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

int g_failed = 0;
int g_total = 0;

void check(bool ok, char const* what) {
  ++g_total;
  if (ok) {
    std::printf("  [ok]   %s\n", what);
  } else {
    std::printf("  [FAIL] %s\n", what);
    ++g_failed;
  }
}

/// 一张有横向渐变 + 几块实心矩形的 BGRA 图。
///
/// 渐变保证每一行、每一列都不同 —— 斜切（行距处理错）或行错位都会让字节不同；
/// 矩形则给出大片高频边缘，保证 qscale 变化能被体积反映出来。纯色渐变
/// 或纯色块单独用都不够：前者在斜切下偶尔还能撞上相似的 DCT 系数，
/// 后者在有损压缩下体积几乎不随 qscale 变。
std::vector<std::uint8_t> make_bgra(int w, int h, int stride) {
  // 结尾多留 64 字节：swscale 的 SIMD 路径会按批处理若干行，读最后一行时
  // 可能多读几个字节。真正喂给编码器的仍是 stride*h 那么大的图，
  // 这点余量只是让"紧凑 stride"这一路不做越界读。
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) *
                                    static_cast<std::size_t>(h) + 64,
                                0);
  for (int y = 0; y < h; ++y) {
    std::uint8_t* row = buf.data() + static_cast<std::size_t>(y) * stride;
    for (int x = 0; x < w; ++x) {
      std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
      p[0] = static_cast<std::uint8_t>(x * 255 / (w > 1 ? w - 1 : 1));  // B
      p[1] = static_cast<std::uint8_t>(y * 255 / (h > 1 ? h - 1 : 1));  // G
      p[2] = static_cast<std::uint8_t>((x + y) & 0xFF);                 // R
      p[3] = 0xFF;                                                     // A
    }
  }
  // 几块实心矩形：制造大面积平坦区 + 锐利边缘，两者对 qscale 的敏感度
  // 正好相反，放一起才能保证体积随 qscale 单调变化。
  struct Rect { int x, y, w, h; std::uint8_t b, g, r; };
  Rect const rects[] = {
      {20, 20, 120, 60, 0, 0, 255},
      {200, 40, 80, 200, 0, 255, 0},
      {400, 150, 180, 120, 255, 0, 0},
      {300, 260, 300, 70, 255, 255, 255},
  };
  for (Rect const& rc : rects) {
    for (int y = rc.y; y < rc.y + rc.h && y < h; ++y) {
      if (y < 0) continue;
      std::uint8_t* row = buf.data() + static_cast<std::size_t>(y) * stride;
      for (int x = rc.x; x < rc.x + rc.w && x < w; ++x) {
        if (x < 0) continue;
        std::uint8_t* p = row + static_cast<std::size_t>(x) * 4;
        p[0] = rc.b;
        p[1] = rc.g;
        p[2] = rc.r;
        p[3] = 0xFF;
      }
    }
  }
  // 行距填充部分写成固定噪声，而不是 0：如果实现错误地把 stride 当成了
  // 有效行宽，这些字节会被当成像素编进去，输出就不再和紧凑版本相同。
  // 填 0 的话，错的那一版在渐变背景下碰巧接近黑色也可能"看起来对"。
  for (int y = 0; y < h; ++y) {
    std::uint8_t* row = buf.data() + static_cast<std::size_t>(y) * stride;
    for (int x = w * 4; x < stride; ++x) {
      row[x] = static_cast<std::uint8_t>(0xA5 ^ (y * 31 + x * 7));
    }
  }
  return buf;
}

bool is_jpeg(std::string const& s) {
  if (s.size() < 4) return false;
  auto const* b = reinterpret_cast<std::uint8_t const*>(s.data());
  return b[0] == 0xFF && b[1] == 0xD8 && b[s.size() - 2] == 0xFF &&
         b[s.size() - 1] == 0xD9;
}

void dump(std::string const& s) {
  std::ofstream f("out/mjpeg_check.jpg", std::ios::binary | std::ios::trunc);
  f.write(s.data(), static_cast<std::streamsize>(s.size()));
}

}  // namespace

int main() {
  int const W = 640, H = 360;
  int const tight = W * 4;

  // ---------- 1. 基本编码 + SOI/EOI ----------
  std::printf("== 1. 640x360 基本编码 ==\n");
  std::vector<std::uint8_t> src = make_bgra(W, H, tight);

  d2o::MjpegEncoder enc;
  check(!enc.ready(), "init 之前 ready() 为 false");
  check(enc.encode(src.data(), tight).empty(),
        "init 之前 encode() 返回空串（不崩）");

  check(enc.init(W, H, 5), "init(640,360,q=5) 成功");
  check(enc.ready(), "ready() 为 true");
  check(enc.width() == W && enc.height() == H, "width()/height() 正确");

  std::string const jpeg = enc.encode(src.data(), tight);
  check(!jpeg.empty(), "encode() 返回非空");
  check(is_jpeg(jpeg), "以 FF D8 开头、FF D9 结尾（SOI/EOI 齐全）");
  std::printf("        体积 %zu 字节\n", jpeg.size());
  // 全同色 / 忽略输入的退化输出只有几百字节到 2 KB。
  check(jpeg.size() > 8000, "体积 > 8 KB（说明真的编进了画面内容）");
  dump(jpeg);

  // ---------- 2. stride：加 64 字节填充后必须逐字节相同 ----------
  std::printf("== 2. stride 处理（width*4 + 64）==\n");
  int const padded = tight + 64;
  std::vector<std::uint8_t> src_padded = make_bgra(W, H, padded);
  // 先确认两个输入的有效像素确实一样，否则下面的比较没有意义。
  bool pixels_equal = true;
  for (int y = 0; y < H && pixels_equal; ++y) {
    pixels_equal = std::memcmp(src.data() + static_cast<std::size_t>(y) * tight,
                               src_padded.data() + static_cast<std::size_t>(y) * padded,
                               static_cast<std::size_t>(tight)) == 0;
  }
  check(pixels_equal, "（前置）两个输入的有效像素相同");

  std::string const jpeg_padded = enc.encode(src_padded.data(), padded);
  check(is_jpeg(jpeg_padded), "带 stride 的输入也能编出合法 JPEG");
  check(jpeg_padded == jpeg,
        "带 stride 的输出与紧凑输出逐字节相同（行距被正确遵守）");
  if (jpeg_padded != jpeg) {
    std::printf("        紧凑 %zu 字节，带间距 %zu 字节\n", jpeg.size(),
                jpeg_padded.size());
  }

  // ---------- 3. qscale 必须真的接上 frame->quality ----------
  std::printf("== 3. 三档 qscale 的体积 ==\n");
  std::size_t sizes[3] = {0, 0, 0};
  int const qs[3] = {2, 10, 28};
  for (int i = 0; i < 3; ++i) {
    check(enc.init(W, H, qs[i]), "init 到该 qscale 成功");
    std::string const j = enc.encode(src.data(), tight);
    check(is_jpeg(j), "该档位编出合法 JPEG");
    sizes[i] = j.size();
    std::printf("        qscale=%2d → %zu 字节\n", qs[i], sizes[i]);
  }
  check(sizes[0] != sizes[1] && sizes[1] != sizes[2] && sizes[0] != sizes[2],
        "三档体积互不相同（qscale 真的传到了 frame->quality）");
  check(sizes[0] > sizes[1] && sizes[1] > sizes[2],
        "体积随 qscale 单调下降（qscale 越小越清晰）");

  // ---------- 4. 重新 init 到另一个分辨率 ----------
  std::printf("== 4. 同一个对象重新 init 到 320x240 ==\n");
  int const W2 = 320, H2 = 240, tight2 = W2 * 4;
  check(enc.init(W2, H2, 5), "init(320,240,q=5) 成功（重开路径）");
  check(enc.width() == W2 && enc.height() == H2, "重开之后宽高已更新");
  std::vector<std::uint8_t> src2 = make_bgra(W2, H2, tight2);
  std::string const jpeg2 = enc.encode(src2.data(), tight2);
  check(is_jpeg(jpeg2), "重开之后仍能编出合法 JPEG");
  std::printf("        体积 %zu 字节\n", jpeg2.size());
  // 分辨率变小，内容也变少，但绝不能是空图。
  check(jpeg2.size() > 2000, "重开之后的输出不是退化图");

  // 再重开一次回原来的分辨率，确认识别得到新尺寸（没有残留旧状态）。
  check(enc.init(W, H, 5), "再 init 回 640x360 成功");
  std::string const jpeg3 = enc.encode(src.data(), tight);
  check(is_jpeg(jpeg3) && jpeg3 == jpeg,
        "回到 640x360 后输出与第一次完全相同（重开没有残留状态）");

  // ---------- 5. 奇数分辨率 ----------
  // 桌面尺寸基本是偶数，但外接显示器/自定义缩放下不是（H.264 那条链路就
  // 必须挡一下）。这里要确认 MJPEG 这条路没有同样的限制 —— 有的话调用方
  // 就得自己去凑偶数，属于必须提前知道的约束。
  std::printf("== 5. 奇数分辨率 641x361 ==\n");
  int const W3 = 641, H3 = 361, tight3 = W3 * 4;
  check(enc.init(W3, H3, 5), "init(641,361) 成功（4:4:4 没有偶数约束）");
  std::vector<std::uint8_t> src3 = make_bgra(W3, H3, tight3);
  std::string const jpeg4 = enc.encode(src3.data(), tight3);
  check(is_jpeg(jpeg4), "奇数分辨率也能编出合法 JPEG");

  // ---------- 汇总 ----------
  std::printf("\n%d/%d 通过\n", g_total - g_failed, g_total);
  if (g_failed) {
    std::printf("失败 %d 项\n", g_failed);
    return 1;
  }
  return 0;
}
