# ---------------------------------------------------------------------------
# FindLibuvcpp.cmake — 定位本机源码构建的 libuvcpp（MinGW64，全模块）
#
# 产出：Libuvcpp::Libuvcpp（INTERFACE）
#       LIBUVCPP_RUNTIME_DLLS   要随 exe 分发的 DLL 全路径
#
# 三处与"正常"的 find module 不一样，都是有原因的：
#
# 1. **头文件有两个根。** `src\` 下是手写的公开头（`uvcpp.h`、`handle\...`），
#    而 `uvcpp_config.h` 是 configure 期生成到构建目录里的。`uvcpp.h` 里
#    `#include <uvcpp/uvcpp_config.h>` 和 `#include "handle/uvcpp_handle.h"`
#    两种写法并存，所以两个 -I 都得给。少一个的症状是"找不到 uvcpp_config.h"，
#    而那个文件确实存在 —— 只是在构建目录里。
#
# 2. **还要一个 nlohmann/json 的 -I。** `webapp/uvcpp_web_json.h` 直接
#    `#include <nlohmann/json.hpp>`，是暴露面：任何碰 webapp 的编译单元都得
#    看得见它。第三方的头不进 `include\`，单独在 `include-ext\`。
#
# 3. **必须定义 UVCPP_IMPORTS 和 UVCPP_C_SHARED。** 这两个宏决定
#    `UVCPP_API` / `UVCPP_C_API` 展开成 `__declspec(dllimport)`。不定义也能编过
#    （宏退化成空），但那是"调用方声明与导入库对不上"的经典坑，症状很难认。
#    反过来，`UVCPP_*_ENABLE` 那些开关**绝对不能自己定义** ——
#    `uvcpp_config.h` 里带了 `#error` 守卫，它要的是"这份库里到底有什么"，
#    不是"你可能想要什么"。
# ---------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

set(_uvcpp_roots "")
if(LIBUVCPP_ROOT)
  list(APPEND _uvcpp_roots "${LIBUVCPP_ROOT}")
endif()
if(DEFINED ENV{LIBUVCPP_ROOT})
  list(APPEND _uvcpp_roots "$ENV{LIBUVCPP_ROOT}")
endif()
list(APPEND _uvcpp_roots
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/uvcpp"
  "${CMAKE_SOURCE_DIR}/third_party/uvcpp")

find_path(LIBUVCPP_INCLUDE_DIR
  NAMES uvcpp.h
  PATHS ${_uvcpp_roots}
  PATH_SUFFIXES include
  NO_DEFAULT_PATH)

find_path(LIBUVCPP_GENERATED_INCLUDE_DIR
  NAMES uvcpp/uvcpp_config.h
  PATHS ${_uvcpp_roots}
  PATH_SUFFIXES include-gen
  NO_DEFAULT_PATH)

find_path(LIBUVCPP_EXT_INCLUDE_DIR
  NAMES nlohmann/json.hpp
  PATHS ${_uvcpp_roots}
  PATH_SUFFIXES include-ext
  NO_DEFAULT_PATH)

find_library(LIBUVCPP_LIBRARY
  NAMES uvcpp libuvcpp.dll.a
  PATHS ${_uvcpp_roots}
  PATH_SUFFIXES lib
  NO_DEFAULT_PATH)

# libuv 的导入库。libuvcpp 的公开头里有直接调用 uv_* 的**内联**函数
# （`uvcpp_udp::recv_stop()` 就是 `uv_udp_recv_stop(...)`，`uvcpp_handle::close()`
# 就是 `uv_close(...)`），所以任何用到这些内联函数的编译单元都得自己链上 libuv。
# 少了的症状是一串 `undefined reference to 'uv_udp_recv_stop'`，而报错指在
# **我们自己的** .cpp 上 —— 看着像漏了头文件，其实是缺一个导入库。
#
# 只在"有 libuvcpp 但没 libuv"时才真正报错：版本必须和 runtime\libuv.dll 同源，
# 所以这里只找 third_party\uvcpp\lib，不去系统里捡一个（系统那份是 MSYS2 自己
# 编的 libuv，和我们的 DLL 版本不一定对得上，缺符号要拖到运行期才炸）。
find_library(LIBUVCPP_UV_LIBRARY
  NAMES libuv.dll.a uv
  PATHS ${_uvcpp_roots}
  PATH_SUFFIXES lib
  NO_DEFAULT_PATH)

find_package_handle_standard_args(Libuvcpp
  REQUIRED_VARS
    LIBUVCPP_INCLUDE_DIR
    LIBUVCPP_GENERATED_INCLUDE_DIR
    LIBUVCPP_EXT_INCLUDE_DIR
    LIBUVCPP_LIBRARY
    LIBUVCPP_UV_LIBRARY
  REASON_FAILURE_MESSAGE
    "没找到 libuvcpp。先跑 scripts\\fetch_uvcpp.ps1 供给依赖（它只搬运，库要先按 third_party\\README.md 自己编），或用 -DLIBUVCPP_ROOT=<前缀> 指定。")

if(Libuvcpp_FOUND AND NOT TARGET Libuvcpp::Libuvcpp)
  add_library(Libuvcpp::Libuvcpp INTERFACE IMPORTED)
  set_target_properties(Libuvcpp::Libuvcpp PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES
      "${LIBUVCPP_INCLUDE_DIR};${LIBUVCPP_GENERATED_INCLUDE_DIR};${LIBUVCPP_EXT_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "${LIBUVCPP_LIBRARY};${LIBUVCPP_UV_LIBRARY}"
    INTERFACE_COMPILE_DEFINITIONS "UVCPP_IMPORTS;UVCPP_C_SHARED")

  # 版本从 uvcpp_version.h 里读，自检拿它和 uvcpp_c_version_string() 对。
  # 注意这个头在**镜像过来的源码树**里（include/uvcpp/），不在生成目录里 ——
  # include-gen/ 下只有 configure 期生成的 uvcpp_config.h 那一个文件。
  if(EXISTS "${LIBUVCPP_INCLUDE_DIR}/uvcpp/uvcpp_version.h")
    file(STRINGS "${LIBUVCPP_INCLUDE_DIR}/uvcpp/uvcpp_version.h" _uv_ver
      REGEX "#define UVCPP_VERSION_(MAJOR|MINOR|PATCH) ")
    set(_maj 0)
    set(_min 0)
    set(_pat 0)
    foreach(_l IN LISTS _uv_ver)
      if(_l MATCHES "UVCPP_VERSION_MAJOR ([0-9]+)")
        set(_maj "${CMAKE_MATCH_1}")
      elseif(_l MATCHES "UVCPP_VERSION_MINOR ([0-9]+)")
        set(_min "${CMAKE_MATCH_1}")
      elseif(_l MATCHES "UVCPP_VERSION_PATCH ([0-9]+)")
        set(_pat "${CMAKE_MATCH_1}")
      endif()
    endforeach()
    set(Libuvcpp_VERSION "${_maj}.${_min}.${_pat}")
  endif()
endif()

set(LIBUVCPP_RUNTIME_DLLS "")
if(_uvcpp_roots)
  foreach(_r IN LISTS _uvcpp_roots)
    if(EXISTS "${_r}/runtime")
      file(GLOB LIBUVCPP_RUNTIME_DLLS "${_r}/runtime/*.dll")
      break()
    endif()
  endforeach()
endif()

mark_as_advanced(
  LIBUVCPP_INCLUDE_DIR
  LIBUVCPP_GENERATED_INCLUDE_DIR
  LIBUVCPP_EXT_INCLUDE_DIR
  LIBUVCPP_LIBRARY
  LIBUVCPP_UV_LIBRARY)
