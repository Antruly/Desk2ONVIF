# 第三方组件与许可证

Desk2ONVIF 的**源码**是 **MIT**（全文见 `LICENSE`）；而**这个发行包里的
可执行文件是 GPL-3.0-or-later**（全文见 `LICENSE-GPL-3.0.txt`）。

为什么二进制不能是 MIT：它链接了下面那份带 `--enable-libx264` 的 FFmpeg 构建，
而 x264 是 GPL-2.0-or-later —— 链接进二进制即成为 GPL 下的组合作品。**这不是
本项目的选择，是许可本身的约束。** 想要纯 MIT 的二进制，得换成 LGPL 版
FFmpeg 并去掉 x264。

GPL 要求分发二进制时能让接收者拿到对应源码，所以每一项都给出上游地址 ——
全部是未经修改的官方版本（唯一的例外见文末）。

## 随包分发的 DLL

| 组件 | 版本 | 许可证 | 文件 | 源码 |
|---|---|---|---|---|
| FFmpeg | 9.0.2 (gyan.dev full_build shared) | **GPL-3.0-or-later** | `avcodec-63.dll` `avdevice-63.dll` `avfilter-12.dll` `avformat-63.dll` `avutil-61.dll` `swresample-7.dll` `swscale-10.dll` | https://ffmpeg.org/download.html |
| libuvcpp | 1.5.0 | MIT | `libuvcpp.dll` | https://github.com/Antruly/libuvcpp |
| libuv | 1.51 | MIT | `libuv.dll` | https://github.com/libuv/libuv |
| llhttp | 9.x | MIT | `libllhttp.dll` | https://github.com/nodejs/llhttp |
| OpenSSL | 3.5.x | Apache-2.0 | `libssl-3-x64.dll` `libcrypto-3-x64.dll` | https://www.openssl.org/source/ |
| zlib | 1.3.x | Zlib | `zlib1__.dll` | https://zlib.net/ |
| GCC 运行库 | GCC 15.1.0 | GPL-3.0-or-later **with GCC Runtime Library Exception** | `libstdc++-6.dll` `libgcc_s_seh-1.dll` `libwinpthread-1.dll` | https://gcc.gnu.org/ |

> **FFmpeg 这个构建是 GPL 的**，不是 LGPL 的。它的 configure 串里有
> `--enable-gpl --enable-version3 --enable-libx264`，所以整个 `avcodec-63.dll`
> 受 GPL-3.0 约束。与发行包的 GPL-3.0 一致，可以一起分发；但**正是它**让
> 本发行包无法以 MIT 分发 —— 换成 LGPL 构建（并去掉 x264）才谈得上宽松许可。
>
> GCC 运行库那份的 **Runtime Library Exception** 明确允许把这三个 DLL 随
> 非 GPL 程序一起分发，没有额外义务。

## 静态链接进可执行文件的

| 组件 | 版本 | 许可证 | 链接进 |
|---|---|---|---|
| Live555 | 2026.09.23 | **LGPL-3.0-or-later** | `desk2onvif.exe` |
| x264 | 依赖 FFmpeg 构建内的版本 | **GPL-2.0-or-later** | `avcodec-63.dll` |
| nghttp2 / ngtcp2 / nghttp3 | — | MIT | `libuvcpp.dll` |
| nlohmann/json | 3.11.3 | MIT | `desk2onvif.exe`、`libuvcpp.dll` |

- **Live555** — https://download.live555.com/ 。它是 **LGPL-3.0-or-later 而我们
  静态链接了它**。LGPLv3 与 GPLv3 兼容，可以并入 GPLv3 作品；LGPL 的"允许用户
  替换库"这一条，本项目通过**完整发布源码 + 提供构建脚本**（`scripts/`）满足。
- **x264** — https://code.videolan.org/videolan/x264 。GPL-2.0-or-later，是
  **本发行包整体按 GPL-3.0 分发**的原因（源码那侧仍是 MIT）。

## 前端

`web/dist/` 是 Vue 3 + Vite 构建产物。它是源码的直接产物、不含任何链接进来的
GPL 组件，所以许可证跟**源码**走：**MIT**，源码在仓库的 `web/src/`。
运行时不含任何第三方 JS 依赖 —— 打包时全部内联，
所以这个目录里没有 node_modules 之类的东西需要单独声明。

## 唯一一处对上游的修改

Live555 的 `config.mingw` 第 1 行末尾追加了 `-DNO_GETIFADDRS`。发行版里没有
这个宏，而 MinGW 没有 `<ifaddrs.h>`，缺了会直接编译失败。这是**构建配置的
一行改动，没有改任何源码**；`scripts/build_live555.ps1` 里可以看到这一行，
它是幂等的（重复应用不会叠加）。对应的补丁内容就是改后的 `config.mingw`
本身，随源码一起提供。

## 完整源码

本项目的完整源码与上述补丁、构建脚本：随本次交付的源码包 / 仓库提供。
