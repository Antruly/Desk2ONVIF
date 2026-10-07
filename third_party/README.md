# third_party — 依赖供给

这个目录**不进版本库**（见根目录 `.gitignore`），由 `scripts/` 下的三个脚本
生成。删掉整个目录再跑一遍脚本，应该得到同样的结果。

```
third_party/
├── ffmpeg/          gyan.dev 预编译包（MinGW shared 构建）
│   ├── include/     libav*.h
│   ├── lib/         libav*.dll.a（MinGW 导入库）
│   └── bin/         av*.dll + ffmpeg.exe/ffprobe.exe（ffprobe 用于验证）
├── live555/         源码构建（2026.09.23）
│   ├── include/     四个模块的头平铺在一起（含 NetCommon.h）
│   └── lib/         libliveMedia.a 等四个静态库
├── uvcpp/           本机源码构建的 libuvcpp（含 SOAP 模块）
│   ├── include/     公开头（源码树 src/ 镜像）
│   ├── include-gen/ uvcpp_config.h（configure 期生成）
│   ├── include-ext/ nlohmann/json（webapp 的 JSON 层依赖）
│   ├── lib/         libuvcpp.dll.a
│   └── runtime/     要随 exe 分发的 DLL
├── _cache/          下载缓存（换机器可以直接拷过去离线用）
└── _build/          构建临时目录（可随时删）
```

三个脚本都是**幂等**的：已经就绪就直接返回。

| 依赖 | 脚本 | 做什么 |
|---|---|---|
| FFmpeg | `scripts\fetch_ffmpeg.ps1` | 下载 + 解压 gyan.dev 的 `ffmpeg-*-full_build-shared.7z` |
| Live555 | `scripts\build_live555.ps1` | 解包 + 打一行补丁 + `genMakefiles mingw` + `make` |
| libuvcpp | `scripts\fetch_uvcpp.ps1` | **只搬运不构建**（见下） |
| OpenSSL | 不需要脚本 | 直接用 MSYS2 的 `mingw-w64-x86_64-openssl` |

```powershell
.\scripts\fetch_ffmpeg.ps1
.\scripts\build_live555.ps1
.\scripts\fetch_uvcpp.ps1
```

## OpenSSL

**也来自 MSYS2**（`mingw-w64-x86_64-openssl`，这里是 3.5.1），不单独供给。
两处要用它：

- **Live555 的 TLS 与 SRTP 是无条件编进去的**，`RTSPServer` 会拉到
  `TLSState.o`、`SRTPCryptographicContext.o`、`HMAC_SHA1.o` 三个目标文件，
  它们的未定义符号是 `SSL_*` / `EVP_*` / `BIO_*` 这一批。发行版的
  `live555.pc` 里**没有**这一项（`config.mingw` 的
  `LIBS_FOR_CONSOLE_APPLICATION` 里有，`.pc` 里没有），照着 `.pc` 链就会在
  链接期炸一屏未定义符号。
- Phase 4 的 HTTP Digest 鉴权要取 MD5。libuvcpp 不导出任何 MD5/base64，
  走 OpenSSL 的 EVP 接口最省事。

`CMakeLists.txt` 里显式 `set(OPENSSL_USE_STATIC_LIBS FALSE)`：用 `.dll.a`
（shared）而不是 `libcrypto.a`。理由是静态那份会额外拖进一串系统库，而且会和
`libuvcpp.dll` 里已经静态链进去的那一份在同一个进程里并存 —— 两边各归各的
反而干净。代价只是多两个运行期 DLL：`libssl-3-x64.dll`、`libcrypto-3-x64.dll`。

---

## FFmpeg

用 gyan.dev 的 `full_build` **shared** 包。选它有两个理由：

- 它是 **MinGW 构建**，带 `.dll.a` 导入库，MinGW 的 g++ 能直接链接。
  同包里那几个 `.lib` 是给 MSVC 的，名字一样格式不同，混用会得到一屏
  看不懂的未定义符号 —— `cmake/FindFFmpeg.cmake` 里显式只认 `.dll.a`。
- 只有 **full_build** 才带 libx264。essentials 包不带，链接能过，
  运行到 `avcodec_find_encoder_by_name("libx264")` 才返回空 ——
  自检程序里专门有一项查这个。

包留在 `_cache/`，下次换机器可以直接拷过去，脚本会优先用缓存不再下载。

## Live555

**必须源码构建**，用 MSYS2 的 bash 跑（`genMakefiles` 和各 Makefile 都是
`/bin/sh` 脚本，`mingw32-make` 不行）。脚本自己会去找 `C:\msys64\usr\bin\bash.exe`。

发行版需要两处处理：

1. **`config.mingw` 第 1 行末尾追加 ` -DNO_GETIFADDRS`。**
   发行版里**没有**这个宏。缺了它，`groupsock/GroupsockHelper.cpp` 会
   `#include <ifaddrs.h>`，而 MinGW 没有该头文件，编译直接失败。
   （回退实现走 `gethostname()`，Windows 上可用。）

2. **归置头文件时必须带上 `NetCommon.h`。**
   四个模块的 include 目录里只有它**不是** `.hh` 后缀（在
   `groupsock/include/` 下），而每个用到 groupsock 的编译单元都要 include 它。
   漏了它得到的是"库编好了、但用不了"的头文件目录：
   `#include "NetCommon.h"` 直接 file not found。
   `scripts/build_live555.sh` 里同时拷 `*.hh` 和 `*.h`。

包里有 17 个符号链接，其中 16 个是相对路径的 `COPYING`/`COPYING.LESSER`
（无害），只有 `live/testProgs/whiteroom.mp3` 指向作者构建机上的绝对路径，
Windows 上创建不了且会让 tar 报错退出 —— 脚本用
`--exclude='live/testProgs/whiteroom.mp3'` 排掉。它只是测试用的 mp3。

链接方式见 `cmake/FindLive555.cmake`：`-lws2_32` 和 OpenSSL 都必须自己补
（发行版的 `live555.pc` 两样都漏），四个库之间是循环引用所以用
`-Wl,--start-group ... -Wl,--end-group` 包起来。

## libuvcpp — 唯一一个要自己编的

**所有预编译包里 `UVCPP_WSDL_ENABLE` 都是 0**，也就是头文件在、`uvcpp_soap_*`
的符号不在。ONVIF 的 SOAP 信封层就靠它，所以必须从源码编全量版本。

源码：`D:\public\libuvcpp\libuvcpp`（可用 `-Source` 或环境变量
`UVCPP_SOURCE` 改）。构建产物在 `build-mingw64-full/`。

```powershell
# 在 libuvcpp 源码树里
cmake -S . -B build-mingw64-full -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe `
  -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe `
  -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe `
  -DUVCPP_BUILD_SHARED=ON -DUVCPP_BUILD_STATIC=OFF `
  -DUVCPP_BUILD_NET=ON -DUVCPP_BUILD_WEB=ON -DUVCPP_BUILD_WEBAPP=ON `
  -DUVCPP_ENABLE_WSDL=ON -DUVCPP_ENABLE_OPENSSL=ON -DUVCPP_ENABLE_ZLIB=ON `
  -DUVCPP_ENABLE_NGHTTP2=ON -DUVCPP_ENABLE_QUIC=ON -DUVCPP_ENABLE_HTTP3=ON `
  -DUVCPP_ENABLE_CAPI=ON `
  -DUVCPP_BUILD_TESTS=OFF -DUVCPP_BUILD_EXAMPLES=OFF `
  -DUVCPP_BUILD_LIBUV_FROM_SOURCE=ON -DUVCPP_USE_SYSTEM_LIBUV=ON `
  -DOPENSSL_ROOT_DIR=C:/msys64/mingw64 -DOPENSSL_USE_STATIC_LIBS=ON `
  -DFETCHCONTENT_SOURCE_DIR_LIBUV=<src>/_local_deps/libuv `
  -DFETCHCONTENT_SOURCE_DIR_LLHTTP=<src>/_local_deps/llhttp `
  -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=<src>/_local_deps/json-3113 `
  -DFETCHCONTENT_SOURCE_DIR_PUGIXML=<src>/_local_deps/pugixml `
  -DFETCHCONTENT_SOURCE_DIR_NGHTTP2=<src>/_local_deps/nghttp2 `
  -DFETCHCONTENT_SOURCE_DIR_NGTCP2=<src>/_local_deps/ngtcp2 `
  -DFETCHCONTENT_SOURCE_DIR_NGHTTP3=<src>/_local_deps/nghttp3
cmake --build build-mingw64-full -j
```

`FETCHCONTENT_SOURCE_DIR_*` 全部指向源码树里的 `_local_deps/`，所以构建
**完全离线**。`UVCPP_ENABLE_WSDL=ON` 要求 `UVCPP_BUILD_WEBAPP=ON`。

`fetch_uvcpp.ps1` 之后做的是搬运和校验，不重新编库：

- `src/` 下的头 → `include/`
- `build-mingw64-full/include/uvcpp/uvcpp_config.h` → `include-gen/`
- `_local_deps/json-3113/include/nlohmann` → `include-ext/`
  （`uvcpp_web_json.h` 里 `#include <nlohmann/json.hpp>` 是暴露面，
  任何碰 webapp 的编译单元都看得见它，不能指望系统里刚好有）
- `libuvcpp.dll.a` → `lib/`
- 六个运行期 DLL → `runtime/`

`runtime/` 里那份清单不是猜的，是 `objdump -p libuvcpp.dll` 的 import 表
读出来对过的：`libuvcpp.dll` 自己，加它依赖的 `libuv.dll`、`libllhttp.dll`、
`zlib1__.dll`（这三个是 libuvcpp 构建时从源码编的，只在它自己的构建树里，
msys64 的 `mingw64\bin` 下**没有**），加 GCC 运行时的
`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`。
OpenSSL 和 nghttp2/ngtcp2/nghttp3 都是静态链进去的，不需要额外 DLL。

> `zlib1__.dll` 那个双下划线不是笔误 —— 它就是 libuvcpp 的构建产物名，
> 导入表里要的就是这个名字，改成 `zlib1.dll` 反而找不到。

消费方式见 `cmake/FindLibuvcpp.cmake`：三个 `-I`（`include`、`include-gen`、
`include-ext`），定义 `UVCPP_IMPORTS` 和 `UVCPP_C_SHARED` 拿到
`__declspec(dllimport)`。**`UVCPP_*_ENABLE` 那些开关绝对不能自己定义** ——
`uvcpp_config.h` 里有 `#error` 守卫。

---

## 许可证

| 依赖 | 许可证 | 与本项目（GPL-3.0）的关系 |
|---|---|---|
| FFmpeg（gyan.dev full_build） | GPL-3.0（因为带了 libx264） | 兼容 |
| libx264 | GPL-2.0-or-later | 是项目整体取 GPL-3.0 的原因 |
| Live555 | LGPL-3.0-or-later | 与 GPLv3 兼容；静态链接可并入 GPLv3 作品 |
| libuvcpp | MIT | 兼容 |
| nlohmann/json | MIT | 兼容 |

第三方组件的完整许可证文本随各组件源码/包分发，本目录不再重复。
