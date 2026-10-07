# ---------------------------------------------------------------------------
# FindLive555.cmake — 定位我们自己构建的 Live555 静态库
#
# 产出：Live555::Live555（INTERFACE）
#       LIVE555_RUNTIME_DLLS（静态链接，恒为空，留着让部署逻辑统一）
#
# 三个必须自己处理的点，发行版的 live555.pc 一个都不管：
#
# 1. **live555.pc 漏了系统库。** Windows 上 socket 全在 ws2_32 里
#    （WSAStartup / select / recvfrom / setsockopt…），少了它就是一大片
#    未定义符号，而报错只会指向 Live555 自己的 .o，看不出是缺系统库。
#
#    同样漏掉的还有 **OpenSSL**。Live555 把 TLS 和 SRTP 都是无条件编进去的，
#    RTSPServer 会拉到 TLSState.o、SRTPCryptographicContext.o、HMAC_SHA1.o，
#    这三个文件的未定义符号是 SSL_write / EVP_* / BIO_new_socket 这一批。
#    发行版的 config.mingw 里是有的（LIBS_FOR_CONSOLE_APPLICATION 那行），
#    但 .pc 里没有，照着 .pc 链就会在链接期炸一大屏。
#
#    这里用 CMake 的 OpenSSL::SSL / OpenSSL::Crypto 而不是写 -lssl -lcrypto：
#    那两个 target 会把静态链接时需要的系统库（crypt32 / ws2_32 / bcrypt …）
#    一并带上，手写的话得自己记全。所以调用方必须**先**
#    find_package(OpenSSL)，本模块只引用。
#
#    （顺带：Desk2ONVIF 自己在 Phase 4 做 Digest 鉴权也要用 OpenSSL 取 MD5，
#    这一份链接两边共用。）
#
# 2. **四个库之间有循环引用（groupsock ↔ liveMedia）。** 对静态库来说
#    "链接顺序"就是语义：GNU ld 单遍扫描，先看到的未定义符号如果在后面才
#    遇到定义，那一遍就废了。所以不去调顺序，直接用
#    --start-group/--end-group 让链接器在组内反复扫描到不动点。
#    （CMake ≥3.24 也可以写 $<LINK_GROUP:RESCAN,...>，是同一件事。）
#
# 3. **头文件是平铺的。** 四个模块各自的 include 目录被合并进了同一个目录，
#    模块之间用 "xxx.hh" 互相引用。所以只有一个 -I，不要按模块拆开加。
# ---------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

set(_live555_roots "")
if(LIVE555_ROOT)
  list(APPEND _live555_roots "${LIVE555_ROOT}")
endif()
if(DEFINED ENV{LIVE555_ROOT})
  list(APPEND _live555_roots "$ENV{LIVE555_ROOT}")
endif()
list(APPEND _live555_roots
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/live555"
  "${CMAKE_SOURCE_DIR}/third_party/live555")

# NetCommon.h 是判据而不是 liveMedia.hh：四个模块的 include 目录里只有它
# 不是 .hh 后缀，最容易在归置时被漏掉，而它又是每个用到 groupsock 的编译单元
# 都要 include 的。用它做存在性判断，能挡住"头文件目录看起来是满的"这种情况。
find_path(LIVE555_INCLUDE_DIR
  NAMES liveMedia.hh NetCommon.h
  PATHS ${_live555_roots}
  PATH_SUFFIXES include
  NO_DEFAULT_PATH)
if(LIVE555_INCLUDE_DIR)
  get_filename_component(_live555_root "${LIVE555_INCLUDE_DIR}" DIRECTORY)
endif()

find_library(LIVE555_LIVEMEDIA_LIBRARY
  NAMES liveMedia libliveMedia.a
  PATHS ${_live555_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH)
find_library(LIVE555_GROUPSOCK_LIBRARY
  NAMES groupsock
  PATHS ${_live555_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH)
find_library(LIVE555_BASICUSAGEENV_LIBRARY
  NAMES BasicUsageEnvironment
  PATHS ${_live555_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH)
find_library(LIVE555_USAGEENV_LIBRARY
  NAMES UsageEnvironment
  PATHS ${_live555_roots} PATH_SUFFIXES lib NO_DEFAULT_PATH)

find_package_handle_standard_args(Live555
  REQUIRED_VARS
    LIVE555_INCLUDE_DIR
    LIVE555_LIVEMEDIA_LIBRARY
    LIVE555_GROUPSOCK_LIBRARY
    LIVE555_BASICUSAGEENV_LIBRARY
    LIVE555_USAGEENV_LIBRARY
  REASON_FAILURE_MESSAGE
    "没找到 Live555。先跑 scripts\\build_live555.ps1 供给依赖，或用 -DLIVE555_ROOT=<前缀> 指定。")

if(Live555_FOUND AND NOT TARGET Live555::Live555)
  # 这几项必须整体保持顺序（顺序就是语义），所以放在一个列表里按序展开。
  # 拆成 INTERFACE_LINK_OPTIONS + INTERFACE_LINK_LIBRARIES 是不行的：
  # 两类项在最终命令行上的相对位置不保证，--start-group 就可能跑到库后面去。
  # ws2_32 是 socket（live555.pc 漏了它），winmm 是 Windows 音频设备输入
  # （我们只做视频，但库是整个 .a，符号需要就得到位，缺了是链接期报错）。
  set(_live555_link_items
    -Wl,--start-group
    ${LIVE555_LIVEMEDIA_LIBRARY}
    ${LIVE555_GROUPSOCK_LIBRARY}
    ${LIVE555_BASICUSAGEENV_LIBRARY}
    ${LIVE555_USAGEENV_LIBRARY}
    -Wl,--end-group
    ws2_32
    winmm)

  # OpenSSL 必须排在 Live555 的库**后面**：liveMedia 里那几个 .o 引用了
  # SSL_* / EVP_* / BIO_*，被引用者得在后面。
  if(TARGET OpenSSL::SSL AND TARGET OpenSSL::Crypto)
    list(APPEND _live555_link_items OpenSSL::SSL OpenSSL::Crypto)
  else()
    message(FATAL_ERROR
      "Live555 需要 OpenSSL，但 OpenSSL::SSL / OpenSSL::Crypto 不存在。\n"
      "在 find_package(Live555) 之前调用 find_package(OpenSSL REQUIRED)。\n"
      "（Live555 无条件编入 TLS 与 SRTP，RTSPServer 会拉到 TLSState.o 等三个\n"
      " 目标文件，它们的未定义符号就是这批 OpenSSL 符号。）")
  endif()

  add_library(Live555::Live555 INTERFACE IMPORTED)
  set_target_properties(Live555::Live555 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${LIVE555_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${_live555_link_items}")

  # 版本串取自头文件里的宏，用来在自检里核对。
  if(EXISTS "${LIVE555_INCLUDE_DIR}/liveMedia_version.hh")
    file(STRINGS "${LIVE555_INCLUDE_DIR}/liveMedia_version.hh" _lm_ver_line
      REGEX "#define LIVEMEDIA_LIBRARY_VERSION_STRING")
    string(REGEX REPLACE ".*\"([^\"]+)\".*" "\\1" Live555_VERSION "${_lm_ver_line}")
  endif()
endif()

set(LIVE555_RUNTIME_DLLS "")

mark_as_advanced(
  LIVE555_INCLUDE_DIR
  LIVE555_LIVEMEDIA_LIBRARY
  LIVE555_GROUPSOCK_LIBRARY
  LIVE555_BASICUSAGEENV_LIBRARY
  LIVE555_USAGEENV_LIBRARY)
