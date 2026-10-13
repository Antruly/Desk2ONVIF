# ---------------------------------------------------------------------------
# Desk2ONVIF — 把 Windows 桌面模拟成一台 ONVIF Profile S 摄像机
#
# 链路：DXGI 桌面复制 → FFmpeg/x264 编码 → Live555 RTSP 推流
#       + libuvcpp 提供 HTTP/WebSocket/ONVIF SOAP 与 WS-Discovery
#
# 工具链是硬约束，不是偏好：third_party 下的产物全是 GCC 格式
# （FFmpeg 的 libav*.dll.a、Live555 的 lib*.a、libuvcpp.dll.a），
# MSVC 一个都吃不下。所以这里当场拦掉，而不是让它在链接期报一堆看不懂的错。
# ---------------------------------------------------------------------------
cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# 工具链 PATH 自查 —— 必须在 project() 之前
#
# MSYS2 的 gcc.exe 自己能起来（它在 mingw64\bin 里，DLL 走"同目录优先"），
# 但它调起来的 cc1.exe 在 lib\gcc\...\15.1.0\ 下，那里没有运行库 DLL，
# 只能靠 PATH 找到。PATH 里没有 mingw64\bin 时，cc1 会因为从别处（比如
# 某个自带 zlib1.dll / libwinpthread-1.dll 的目录）加载到版本不对的 DLL
# 而以 STATUS_ENTRYPOINT_NOT_FOUND 退出。
#
# 这个失败**完全静默**：cc1 不打印任何东西，只返回退出码，CMake 那边表现为
# "The C compiler ... is not able to compile a simple test program"，
# ninja 那边只有一行 "FAILED: [code=1]"。现场没有任何线索指向 PATH。
# 所以在这里判一次，把话说清楚 —— 这比让使用者去猜便宜太多。
# ---------------------------------------------------------------------------
if(WIN32 AND CMAKE_CXX_COMPILER MATCHES "msys64/mingw64/bin")
  get_filename_component(_mingw_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
  file(TO_NATIVE_PATH "${_mingw_bin}" _mingw_bin_native)
  # 先落到普通变量里再遍历：foreach(... IN LISTS ENV{PATH}) 在这里不展开，
  # 直接用 ENV{} 会得到一个空列表，判据就永远为"没找到"。
  set(_path_raw "$ENV{PATH}")
  string(REPLACE "\\" "/" _path_raw "${_path_raw}")
  string(TOLOWER "${_mingw_bin}" _mingw_bin_lc)
  string(REPLACE "\\" "/" _mingw_bin_lc "${_mingw_bin_lc}")
  set(_path_has_mingw FALSE)
  set(_msys_paths "")
  foreach(_p IN LISTS _path_raw)
    if(NOT _p)
      continue()
    endif()
    set(_p_lc "${_p}")
    string(TOLOWER "${_p_lc}" _p_lc)
    string(REPLACE "\\" "/" _p_lc "${_p_lc}")
    string(REGEX REPLACE "/+$" "" _p_lc "${_p_lc}")
    if(_p_lc STREQUAL _mingw_bin_lc)
      set(_path_has_mingw TRUE)
    endif()
    # 挑出可能造成 DLL 撞名的那些：名字里带 msys / mingw / zlib / ffmpeg 的
    if(_p_lc MATCHES "(msys|mingw|zlib|ffmpeg)")
      list(APPEND _msys_paths "${_p}")
    endif()
  endforeach()
  if(NOT _msys_paths)
    set(_msys_paths "(没有看起来相关的项)")
  else()
    string(REPLACE ";" "\n       " _msys_paths "${_msys_paths}")
  endif()
  if(NOT _path_has_mingw)
    message(FATAL_ERROR
      "PATH 里没有 ${_mingw_bin_native}，MSYS2 的编译器无法工作。\n\n"
      "原因：gcc.exe 会去调 cc1.exe，而 cc1.exe 不在 mingw64\\bin 下，它需要的\n"
      "运行库 DLL（zlib1.dll / libwinpthread-1.dll / libgmp-10.dll …）只能从 PATH\n"
      "里找。PATH 里没有这个目录时，cc1 会从别处加载到版本不对的同名 DLL，然后以\n"
      "STATUS_ENTRYPOINT_NOT_FOUND 静默退出 —— 报错里不会出现任何相关字样。\n\n"
      "现在 PATH 里的相关项：${_msys_paths}\n\n"
      "三种解决办法，任选一种：\n"
      "  1. 用项目自带的脚本（推荐）：\n"
      "       .\\scripts\\build.ps1\n"
      "  2. 用 MSYS2 的 MinGW64 shell 跑 cmake（开始菜单里的\n"
      "     \"MSYS2 MINGW64\"，或 C:\\msys64\\mingw64.exe）。\n"
      "  3. 手工在当前会话里前置 PATH：\n"
      "       $env:PATH = 'C:\\msys64\\mingw64\\bin;' + $env:PATH\n")
  endif()
endif()

# RC 这一门是为了把图标和版本信息编进 exe 本身（resources\desk2onvif.rc）。
# 不加的话任务栏和资源管理器显示的是 Windows 默认图标 —— 安装包和快捷方式
# 各自有图标，唯独跑起来的程序没有，而这三处互不相干。
project(Desk2ONVIF
  VERSION 0.1.9
  DESCRIPTION "Windows desktop screen as an ONVIF Profile S IP camera"
  LANGUAGES C CXX RC)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(NOT MINGW)
  message(FATAL_ERROR
    "Desk2ONVIF 只支持 MinGW64 工具链（当前编译器 ID: ${CMAKE_CXX_COMPILER_ID}）。\n"
    "third_party 下的依赖都是 MinGW 构建的，MSVC 无法链接。配置示例：\n"
    "  cmake -S . -B build -G Ninja \\\n"
    "    -DCMAKE_C_COMPILER=C:/msys64/mingw64/bin/gcc.exe \\\n"
    "    -DCMAKE_CXX_COMPILER=C:/msys64/mingw64/bin/g++.exe \\\n"
    "    -DCMAKE_MAKE_PROGRAM=C:/msys64/mingw64/bin/ninja.exe")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)

# windres 默认按"当前代码页"解释 .rc 里的字节，而我们的 .rc 存的是 UTF-8
# （版本块里有中文）。不说清楚的话那些中文会变成乱码 —— 而且**不报错**，
# 要等有人打开文件属性才看得出来。所以要显式告诉它。
set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} --codepage=65001")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "构建类型" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_compile_options(-Wall -Wextra)
add_compile_definitions(
  WIN32_LEAN_AND_MEAN
  NOMINMAX
  _WIN32_WINNT=0x0A00)   # Windows 10 —— DXGI 桌面复制要 8 以上，这里直接钉 10

# ---------------------------------------------------------------------------
# 依赖
#
# 三个都不走 FetchContent：FFmpeg 是预编译包、Live555 要打补丁后自己编、
# libuvcpp 必须全模块从源码编（预编译包一律不含 SOAP）。供给脚本在 scripts\，
# 说明在 third_party\README.md。
# ---------------------------------------------------------------------------
# OpenSSL 必须最先找：Live555 的 TLS/SRTP 目标文件无条件引用 SSL_*/EVP_*/BIO_*，
# FindLive555 会把它加进链接接口。Phase 4 的 Digest 鉴权取 MD5 也用同一份。
# 显式要 shared：静态 libcrypto.a 会额外拖进一串系统库，而且会和 libuvcpp.dll
# 里已经静态链进去的那一份在同一个进程里并存 —— 用 .dll.a 时两边各归各的，
# 反而干净。
set(OPENSSL_USE_STATIC_LIBS FALSE)
find_package(OpenSSL REQUIRED)

find_package(FFmpeg REQUIRED)
find_package(Live555 REQUIRED)
find_package(Libuvcpp REQUIRED)

# OpenSSL 的运行期 DLL（libssl-3-x64.dll / libcrypto-3-x64.dll）。
# 从导入库所在目录推出 prefix，再去 bin\ 下找 —— FindOpenSSL 不给这个信息。
set(OPENSSL_RUNTIME_DLLS "")
if(OPENSSL_CRYPTO_LIBRARY)
  get_filename_component(_ossl_libdir "${OPENSSL_CRYPTO_LIBRARY}" DIRECTORY)
  get_filename_component(_ossl_prefix "${_ossl_libdir}" DIRECTORY)
  if(EXISTS "${_ossl_prefix}/bin")
    file(GLOB OPENSSL_RUNTIME_DLLS
      "${_ossl_prefix}/bin/libssl-*.dll"
      "${_ossl_prefix}/bin/libcrypto-*.dll")
  endif()
endif()

# 一个接口目标把三套依赖收在一起。后面的模块（capture/encoder/rtsp/onvif/web）
# 都只链它，不各自去记 FFmpeg 的库顺序、Live555 的 --start-group。
add_library(desk2onvif_deps INTERFACE)
target_link_libraries(desk2onvif_deps INTERFACE
  FFmpeg::avcodec
  FFmpeg::avformat
  FFmpeg::avutil
  FFmpeg::swscale
  FFmpeg::swresample
  Live555::Live555
  Libuvcpp::Libuvcpp)

# ---------------------------------------------------------------------------
# 核心库
#
# 分成库 + 可执行文件而不是一坨 add_executable：后期的 tests\ 要直接链这些
# 模块做冒烟测试，而 main.cpp 里那套命令行解析不该被链进去。
# ---------------------------------------------------------------------------
add_library(desk2onvif_core STATIC
  src/common/log.cpp
  src/common/audit_log.cpp
  src/common/text.cpp
  src/common/config.cpp
  src/common/adaptive.cpp
  src/common/device_state.cpp
  src/common/ntp_client.cpp
  src/common/ntp_syncer.cpp
  src/common/firewall.cpp
  src/capture/display_info.cpp
  src/capture/desktop_capture.cpp
  src/capture/cursor_overlay.cpp
  src/capture/osd_overlay.cpp
  src/capture/display_source.cpp
  src/capture/frame_source.cpp
  src/capture/webcam_capture.cpp
  src/encoder/x264_encoder.cpp
  src/encoder/mp4_writer.cpp
  src/encoder/mjpeg_encoder.cpp
  src/encoder/fmp4_muxer.cpp
  src/encoder/aac_encoder.cpp
  src/rtsp/nalu.cpp
  src/rtsp/h264_live_source.cpp
  src/rtsp/aac_live_source.cpp
  src/rtsp/rtsp_server.cpp
  src/onvif/digest.cpp
  src/onvif/net_identity.cpp
  src/onvif/wsd.cpp
  src/onvif/onvif_service.cpp
  src/web/web_server.cpp
  src/web/web_session.cpp
  src/service/service_host.cpp
  src/audio/audio_device.cpp
  src/audio/wasapi_capture.cpp
  src/audio/audio_mixer.cpp
  src/app/audio_track.cpp
  src/app/channel_plan.cpp
  src/app/capture_session.cpp)

target_include_directories(desk2onvif_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(desk2onvif_core PUBLIC desk2onvif_deps)

# 版本号编进代码里（`src/common/version.h` 的 `d2o::kVersion`）。
#
# **PUBLIC，不是 PRIVATE**：`tests/` 链的是 desk2onvif_core，而"版本号在
# `/api/status` 与 `/ws/stats` 两条路上都在"这类断言要拿它做对账。
# 另外 `resources/desk2onvif.rc.in` 用的是同一个 `PROJECT_VERSION`，
# 所以 exe 属性和程序自己报的数**不可能**各写一份 —— 那正是这一轮要修的病。
target_compile_definitions(desk2onvif_core PUBLIC D2O_VERSION="${PROJECT_VERSION}")
# d3d11/dxgi 是采集的；iphlpapi 是 GetAdaptersAddresses（net_identity 枚举网卡）——
# 它在 iphlpapi.dll 里，不在默认的系统库集合里，缺了就是一句
# `undefined reference to 'GetAdaptersAddresses'`，看不出该链什么。
#
# gdi32 是 OSD 水印的：它用 CreateDIBSection + TextOutW 把文字栅格成掩码。
# **必须是 PUBLIC** —— tests\ 里的 osd_check 直接链 desk2onvif_core 并调
# OsdOverlay，PRIVATE 的话它那边会报一串 `undefined reference to 'CreateDIBSection'`。
#
# 服务形态那四个（advapi32 / wtsapi32 / userenv / shell32）：
#   advapi32 —— SCM 全套（OpenSCManager/CreateService/StartService/QueryService*）
#                + RegisterServiceCtrlHandlerEx + DuplicateTokenEx + OpenProcessToken
#   wtsapi32 —— WTSGetActiveConsoleSessionId / WTSQueryUserToken
#   userenv  —— CreateEnvironmentBlock / DestroyEnvironmentBlock
#   shell32  —— ShellExecuteExW（--service install 的自我提权）
# MinGW 的系统库都不在默认集合里，缺哪个就是一句 undefined reference，
# 而报错只给函数名不给库名（同 iphlpapi 那条）。
#
# 音频那两组：
#   ole32  —— CoInitializeEx / CoCreateInstance / CoTaskMemFree / PropVariantClear。
#             **不需要 uuid**：MMDevice 那几个 GUID 走 `__uuidof`（mingw 的
#             `__CRT_UUID_DECL` 模板模拟），是编译期常量、不引用任何符号 ——
#             已实测只链 ole32 就能枚举出端点。手抄 GUID 字节是"抄错一个就
#             静默拿不到设备"的那类缺陷，能不用就不用。
#   mmdevapi / avrt —— wasapi_capture 用的（IMMDevice 的 Activate 不需要符号，
#             但 `AvSetMmThreadCharacteristics` 在 avrt 里）。
#
# 摄像头那一组（第五轮 Phase AE，Media Foundation）：
#   mfcore     —— `MFEnumDeviceSources` / `MFCreateDeviceSource`。**实测这两个
#                 只在这个库里**（libmfplat.a 里一个都没有），而报错只给函数名
#                 不给库名 —— 和上面 iphlpapi 那条是同一类坑。
#   mfplat     —— `MFStartup` / `MFCreateAttributes` / `MFCreateMediaType` /
#                 `MFGetAttributeSize` / `MFGetAttributeRatio`。
#   mfreadwrite—— `MFCreateSourceReaderFromMediaSource`。
#   mfuuid     —— Media Foundation 那几个 CLSID/接口 IID 的符号。**要它**：
#                 `__uuidof` 对 MF 的接口不成立（那些是 `__declspec(uuid)` 声明的
#                 MIDL 接口，mingw 的头里没有 `__CRT_UUID_DECL`），所以
#                 `MFCreateSourceReaderFromMediaSource` 那些 `REFIID` 参数是
#                 **真实的符号引用**，不像 MMDevice 那样能白拿。
#   ole32 已经在上面 —— MF 的 `MFStartup` 不碰 COM，但 `MFEnumDeviceSources`
#                 内部要初始化设备枚举器，实测少了它不一定当场报错，不额外加。
target_link_libraries(desk2onvif_core PUBLIC d3d11 dxgi iphlpapi gdi32
  advapi32 wtsapi32 userenv shell32 ole32 mmdevapi avrt
  mfcore mfplat mfreadwrite mfuuid)

# ---------------------------------------------------------------------------
# 可执行文件
#
# Phase 0 的自检仍然在里面（--selfcheck），Phase 1 起多了 --list-displays
# 和 --record。三者共用同一个 exe 是刻意的：录制依赖的东西和自检依赖的完全
# 一样，分成两个 exe 反而会让"DLL 部署对不对"这件事有两套答案。
# ---------------------------------------------------------------------------
# 资源脚本由**模板**生成，好让版本号只有 `project(... VERSION x.y.z)` 一个来源。
#
# 生成到构建目录是有代价的：windres 解析 `.rc` 引用的文件时按 **`.rc` 自己的
# 目录**找（陷阱 55），所以模板里那个图标路径必须是绝对的 —— 写相对的话它会
# 去构建目录里找一个不存在的 `.ico`。理由写在 `resources/desk2onvif.rc.in`
# 的文件头里。
#
# `@ONLY` 是必须的：模板里有 `@PROJECT_VERSION@` 这种占位符，而 CMake 默认还会
# 顺手展开 `${...}` —— 万一将来模板里出现一个 `${}`（比如注释里举例），
# 那是**静默**改掉文件内容。
configure_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/resources/desk2onvif.rc.in"
  "${CMAKE_CURRENT_BINARY_DIR}/desk2onvif.rc"
  @ONLY)

add_executable(desk2onvif
  src/main.cpp
  "${CMAKE_CURRENT_BINARY_DIR}/desk2onvif.rc")   # 图标 + 版本信息，见模板的说明
target_link_libraries(desk2onvif PRIVATE desk2onvif_core)

# ---------------------------------------------------------------------------
# 运行期 DLL 部署
#
# 用 file(GENERATE) 生成一个脚本再 include，而不是把 DLL 列表当 -D 参数传给
# cmake -P：DLL 列表里的分号会参与 CMake 的列表展开，COMMAND 的参数边界会被
# 吃掉，结果是第二个 DLL 被当成第二条命令。生成脚本没有这个问题。
# ---------------------------------------------------------------------------
set(_runtime_dlls
  ${FFMPEG_RUNTIME_DLLS}
  ${LIVE555_RUNTIME_DLLS}
  ${LIBUVCPP_RUNTIME_DLLS}
  ${OPENSSL_RUNTIME_DLLS})
list(REMOVE_DUPLICATES _runtime_dlls)
list(LENGTH _runtime_dlls _runtime_dll_count)

file(GENERATE
  OUTPUT "${CMAKE_BINARY_DIR}/deploy_runtime_$<CONFIG>.cmake"
  CONTENT
"set(DEST \"$<TARGET_FILE_DIR:desk2onvif>\")
set(FILES \"${_runtime_dlls}\")
include(\"${CMAKE_CURRENT_SOURCE_DIR}/cmake/DeployRuntimeDeps.cmake\")
")

add_custom_command(TARGET desk2onvif POST_BUILD
  COMMAND ${CMAKE_COMMAND} -P "${CMAKE_BINARY_DIR}/deploy_runtime_$<CONFIG>.cmake"
  COMMENT "部署运行期 DLL（${_runtime_dll_count} 个）"
  VERBATIM)

# ---------------------------------------------------------------------------
# 构建摘要 —— 把"到底链上了哪一份依赖"直接打出来。
# 值这个打印：libuvcpp 的 SOAP 模块在预编译包里是关着的，链错了也编得过。
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "Desk2ONVIF ${PROJECT_VERSION} — ${CMAKE_BUILD_TYPE}")
message(STATUS "  编译器      : ${CMAKE_CXX_COMPILER} (${CMAKE_CXX_COMPILER_VERSION})")
message(STATUS "  FFmpeg      : ${FFMPEG_INCLUDE_DIR}")
message(STATUS "  OpenSSL     : ${OPENSSL_VERSION} — ${OPENSSL_CRYPTO_LIBRARY}")
message(STATUS "  Live555     : ${Live555_VERSION} — ${LIVE555_INCLUDE_DIR}")
message(STATUS "  libuvcpp    : ${Libuvcpp_VERSION} — ${LIBUVCPP_INCLUDE_DIR}")
message(STATUS "  运行期 DLL  : ${_runtime_dll_count} 个 → ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
