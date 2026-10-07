# ---------------------------------------------------------------------------
# DeployRuntimeDeps.cmake — 把运行期 DLL 复制到可执行文件旁边
#
# 不是给 -P 单独跑的，由 CMakeLists.txt 通过 file(GENERATE) 生成的脚本
# include 进来。调用前必须已设好：
#   DEST   —— 目标目录（可执行文件所在目录）
#   FILES  —— DLL 全路径列表
#
# 为什么这件事值得一个脚本，而不是直接 file(COPY)：
#   1. 缺 DLL 的失败方式是最没有上下文的那种 —— 双击后一个
#      0xc0000135，或者从命令行跑时一句"应用程序无法正常启动"。
#      链接期完全看不出来，因为导入库是齐的。所以**源文件缺失是硬错误**，
#      在这里就说清楚是哪一个。
#   2. 反过来，**目标被占用不该是硬错误**：开发时程序正开着，Windows 会锁住
#      已加载的 DLL，复制失败。这种时候只该提醒一句，不该让整个构建挂掉。
#   3. ONLY_IF_DIFFERENT 让每次构建不用重写十几 MB。
# ---------------------------------------------------------------------------

if(NOT DEFINED DEST)
  message(FATAL_ERROR "DeployRuntimeDeps.cmake: 没给 DEST")
endif()

# --- 1. 源文件必须都在（供给脚本的问题，早报早好） -------------------------
set(_missing "")
foreach(_f IN LISTS FILES)
  if(NOT EXISTS "${_f}")
    list(APPEND _missing "${_f}")
  endif()
endforeach()
if(_missing)
  string(REPLACE ";" "\n    " _missing_txt "${_missing}")
  message(FATAL_ERROR
    "运行期 DLL 的源文件不存在，exe 会在启动时直接退出（0xc0000135）：\n    ${_missing_txt}\n"
    "重新跑 scripts 下的供给脚本：fetch_ffmpeg.ps1 / build_live555.ps1 / fetch_uvcpp.ps1")
endif()

file(MAKE_DIRECTORY "${DEST}")

# --- 2. 复制 ---------------------------------------------------------------
#
# 走 cmake -E copy_if_different 而不是 file(COPY_FILE)：后者**没有**
# RESULT_VARIABLE，目标被占用时它会直接终止整个脚本，于是"程序正开着"这件事
# 会表现成一次构建失败。这里要的是"能拿到失败、并且不当回事"。
# （-E 的失败只体现在退出码上，所以拿 execute_process 的 RESULT_VARIABLE。）
set(_locked "")
set(_copied 0)
foreach(_f IN LISTS FILES)
  get_filename_component(_name "${_f}" NAME)
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_f}" "${DEST}/${_name}"
    RESULT_VARIABLE _r
    OUTPUT_QUIET
    ERROR_QUIET)
  if(_r)
    list(APPEND _locked "${_name}")
  else()
    math(EXPR _copied "${_copied} + 1")
  endif()
endforeach()

if(_locked)
  string(REPLACE ";" ", " _locked_txt "${_locked}")
  message(WARNING
    "有 DLL 没能更新（多半是程序正在运行、文件被锁住）：${_locked_txt}\n"
    "关掉正在运行的 desk2onvif 再构建一次即可。源文件都在，不是供给问题。")
endif()

message(STATUS "运行期 DLL 已就位：${DEST}（本次更新 ${_copied} 个）")
