# ---------------------------------------------------------------------------
# FindFFmpeg.cmake — 定位 gyan.dev 的 MinGW shared 构建（libav*.dll.a）
#
# 为什么不用社区里流传的那份 FindFFmpeg：那些是为 MSVC / 系统级安装在 /usr 下
# 写的。本项目的依赖固定在 third_party\ffmpeg，优先级正好相反 ——
#   显式给的 FFMPEG_ROOT  >  源码树里的 third_party\ffmpeg  >  系统默认路径
#
# 产出：
#   FFmpeg::avcodec FFmpeg::avformat FFmpeg::avutil
#   FFmpeg::swscale FFmpeg::swresample        （必选）
#   FFmpeg::avdevice FFmpeg::avfilter         （存在则提供）
#   FFMPEG_RUNTIME_DLLS   要随 exe 分发的运行期 DLL 全路径（含 ffmpeg 的依赖）
#
# 这里**只接受 MinGW 的导入库**（.dll.a）。同目录下那几个 .lib 是给 MSVC 的，
# 名字一样但格式不同，混进去的报错是一屏看不懂的未定义符号，所以显式排掉。
# ---------------------------------------------------------------------------

include(FindPackageHandleStandardArgs)

set(_ffmpeg_roots "")
if(FFMPEG_ROOT)
  list(APPEND _ffmpeg_roots "${FFMPEG_ROOT}")
endif()
if(DEFINED ENV{FFMPEG_ROOT})
  list(APPEND _ffmpeg_roots "$ENV{FFMPEG_ROOT}")
endif()
list(APPEND _ffmpeg_roots
  "${CMAKE_CURRENT_LIST_DIR}/../third_party/ffmpeg"
  "${CMAKE_SOURCE_DIR}/third_party/ffmpeg")

find_path(FFMPEG_INCLUDE_DIR
  NAMES libavcodec/avcodec.h
  PATHS ${_ffmpeg_roots}
  PATH_SUFFIXES include
  NO_DEFAULT_PATH)
if(NOT FFMPEG_INCLUDE_DIR)
  find_path(FFMPEG_INCLUDE_DIR NAMES libavcodec/avcodec.h)
endif()
if(FFMPEG_INCLUDE_DIR)
  get_filename_component(_ffmpeg_root "${FFMPEG_INCLUDE_DIR}" DIRECTORY)
endif()

# 每个组件一行："<名称>:<依赖1>,<依赖2>"，没有依赖就写 "-"。
#
# 这一行踩过两个 CMake 的坑，两个都不报在出错的地方：
#
#  1. **依赖之间不能用分号。** 分号是 CMake 的列表分隔符，写在这个字符串里，
#     它外面这层列表会被就地拆开 —— "avcodec:avutil;swresample" 会变成两个
#     元素 "avcodec:avutil" 和 "swresample"，后者没有冒号，解析当场越界。
#     所以内层用逗号。
#  2. **不能用空串占位成对列表。** CMake 会把空的列表元素吃掉，它后面所有
#     配对整体错位一格：症状是某个库的依赖跑到另一个库身上（swresample 声称
#     依赖 avformat），报错却指向一个看起来毫不相干的 target。
#     所以"没有依赖"写成 "-"。
#
# 依赖顺序有意义：GNU ld 对静态/导入库是单遍扫描，被依赖者必须排在依赖者后面。
set(_ffmpeg_components
  "avcodec:avutil,swresample"
  "avformat:avcodec,avutil"
  "avutil:-"
  "swscale:avutil"
  "swresample:avutil"
  "avdevice:avformat,avcodec,avutil"
  "avfilter:avformat,avcodec,avutil,swscale,swresample")

# avcodec / avformat / avutil / swscale / swresample 是硬需求：少了任何一个，
# Phase 1 的编码链路都搭不起来，配置期就报出来比链接期报一堆符号清楚。
set(_ffmpeg_required avcodec avformat avutil swscale swresample)
set(_ffmpeg_required_lib_vars "")

set(FFMPEG_FOUND_LIBS "")
foreach(_entry IN LISTS _ffmpeg_components)
  string(REPLACE ":" ";" _pair "${_entry}")
  list(GET _pair 0 _comp)
  list(GET _pair 1 _deps_raw)
  if(_deps_raw STREQUAL "-")
    set(_deps "")
  else()
    string(REPLACE "," ";" _deps "${_deps_raw}")
  endif()
  string(TOUPPER "${_comp}" _COMP)

  find_library(FFMPEG_${_COMP}_LIBRARY
    NAMES ${_comp} lib${_comp}.dll.a lib${_comp}.a
    PATHS ${_ffmpeg_roots}
    PATH_SUFFIXES lib
    NO_DEFAULT_PATH)
  if(NOT FFMPEG_${_COMP}_LIBRARY)
    find_library(FFMPEG_${_COMP}_LIBRARY NAMES ${_comp})
  endif()

  if(FFMPEG_${_COMP}_LIBRARY)
    set(_ffmpeg_link_deps "")
    foreach(_d IN LISTS _deps)
      string(TOUPPER "${_d}" _D)
      list(APPEND _ffmpeg_link_deps "FFmpeg::${_d}")
    endforeach()

    if(NOT TARGET FFmpeg::${_comp})
      add_library(FFmpeg::${_comp} UNKNOWN IMPORTED)
      set_target_properties(FFmpeg::${_comp} PROPERTIES
        IMPORTED_LOCATION "${FFMPEG_${_COMP}_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${FFMPEG_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "${_ffmpeg_link_deps}")
    endif()
    list(APPEND FFMPEG_FOUND_LIBS "FFmpeg::${_comp}")
  endif()

  if(_comp IN_LIST _ffmpeg_required)
    list(APPEND _ffmpeg_required_lib_vars "FFMPEG_${_COMP}_LIBRARY")
  endif()
endforeach()

# 需要的 target 必须真的建出来了。find_package_handle_standard_args 只查变量，
# "变量有值、target 却没建出来"是可能的，而那种情况下报错会出现在生成阶段并
# 指向一个看起来毫不相干的 target —— 在这里拦一次，话说得清楚得多。
set(_ffmpeg_missing_targets "")
foreach(_c IN LISTS _ffmpeg_required)
  if(NOT TARGET FFmpeg::${_c})
    list(APPEND _ffmpeg_missing_targets "FFmpeg::${_c}")
  endif()
endforeach()
if(_ffmpeg_missing_targets)
  message(FATAL_ERROR
    "FFmpeg 组件库没有全部建出来：${_ffmpeg_missing_targets}\n"
    "（库文件很可能没找到，检查 ${_ffmpeg_root}/lib 下的 libav*.dll.a）")
endif()

find_package_handle_standard_args(FFmpeg
  REQUIRED_VARS FFMPEG_INCLUDE_DIR ${_ffmpeg_required_lib_vars}
  REASON_FAILURE_MESSAGE
    "没找到 FFmpeg。先跑 scripts\\fetch_ffmpeg.ps1 供给依赖，或用 -DFFMPEG_ROOT=<前缀> 指定。")

# 运行期 DLL：bin\ 下那些 —— 光有导入库链接能过，跑起来照样缺 DLL。
# 注意这个目录里还有 ffmpeg.exe / ffprobe.exe，但它们不是 *.dll，不会进来；
# ffprobe 我们要单独拿来做验证，所以另外记一个 FFMPEG_FFPROBE。
set(FFMPEG_RUNTIME_DLLS "")
if(_ffmpeg_root AND EXISTS "${_ffmpeg_root}/bin")
  file(GLOB FFMPEG_RUNTIME_DLLS "${_ffmpeg_root}/bin/*.dll")
endif()
set(FFMPEG_FFPROBE "${_ffmpeg_root}/bin/ffprobe.exe")

mark_as_advanced(FFMPEG_INCLUDE_DIR FFMPEG_FFPROBE)
foreach(_c IN LISTS _ffmpeg_required)
  string(TOUPPER "${_c}" _C)
  mark_as_advanced(FFMPEG_${_C}_LIBRARY)
endforeach()
