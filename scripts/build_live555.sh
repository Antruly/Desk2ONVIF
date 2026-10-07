#!/bin/sh
# ---------------------------------------------------------------------------
# build_live555.sh — 为 MinGW64 构建 Live555 (LGPL-3.0-or-later)
#
# 必须在 MSYS2 的 bash 下运行：Live555 的 genMakefiles 与各 Makefile 都是
# /bin/sh 脚本，且需要 MSYS2 的 make / g++。
#
# 幂等：库和头文件都已归置好时直接返回 0。库已构建但头文件没归置全时，
# 只补归置，不重建。
#
# 用法：
#   build_live555.sh <cache-dir> <dest-dir> <work-dir> [jobs] [tarball]
#
# 产物：
#   <dest-dir>/include/*.hh,*.h      — 四个模块的 include 目录平铺在一起
#   <dest-dir>/lib/lib{liveMedia,groupsock,BasicUsageEnvironment,UsageEnvironment}.a
# ---------------------------------------------------------------------------
set -eu

CACHE_DIR=${1:?需要 cache 目录}
DEST=${2:?需要 dest 目录}
WORK=${3:?需要 work 目录}
JOBS=${4:-4}
TARBALL=${5:-}

# 固定用 MSYS2 的 mingw64 工具链，不依赖调用方 PATH 里是哪个 gcc
PATH="/mingw64/bin:/usr/bin:$PATH"
export PATH

NEEDED_LIBS="libliveMedia.a libgroupsock.a libBasicUsageEnvironment.a libUsageEnvironment.a"

# 归置头文件时**必须**包含 NetCommon.h：四个模块的 include 目录里只有它不是
# .hh 后缀（在 groupsock/include/ 下），而任何用到 groupsock 的编译单元都会
# include 它。只拷 *.hh 会得到一个"能编库、但用不了"的头文件目录 ——
# `#include "NetCommon.h"` 直接 file not found。
stage_headers() {
  src=$1
  mkdir -p "$DEST/include"
  for d in liveMedia groupsock UsageEnvironment BasicUsageEnvironment; do
    if [ -d "$src/$d/include" ]; then
      cp -f "$src/$d/include/"*.hh "$DEST/include/" 2>/dev/null || true
      cp -f "$src/$d/include/"*.h  "$DEST/include/" 2>/dev/null || true
    fi
  done
}

stage_libs() {
  src=$1
  mkdir -p "$DEST/lib"
  for l in liveMedia groupsock BasicUsageEnvironment UsageEnvironment; do
    cp -f "$src/$l/lib$l.a" "$DEST/lib/"
  done
}

# --- 1. 已经归置好了吗 ------------------------------------------------------
libs_ok=1
for l in $NEEDED_LIBS; do
  if [ ! -f "$DEST/lib/$l" ]; then libs_ok=0; fi
done
headers_ok=1
if [ ! -f "$DEST/include/NetCommon.h" ] || [ ! -f "$DEST/include/liveMedia.hh" ]; then
  headers_ok=0
fi

if [ "$libs_ok" = "1" ] && [ "$headers_ok" = "1" ]; then
  echo "[live555] 已构建，跳过：$DEST"
  exit 0
fi

# --- 2. 构建树还在吗（在的话只补归置，省一次全量编译） ----------------------
LIVE="$WORK/live"
work_ok=1
for l in liveMedia groupsock BasicUsageEnvironment UsageEnvironment; do
  if [ ! -f "$LIVE/$l/lib$l.a" ]; then work_ok=0; fi
done

if [ "$work_ok" = "1" ]; then
  echo "[live555] 复用已有构建树，只补归置：$LIVE"
  stage_headers "$LIVE"
  stage_libs "$LIVE"
  echo "[live555] 完成：$DEST"
  ls -l "$DEST/lib"
  exit 0
fi

# --- 3. 定位源码包 ---------------------------------------------------------
if [ -z "$TARBALL" ]; then
  if [ -f "$CACHE_DIR/live555-latest.tar.gz" ]; then
    TARBALL="$CACHE_DIR/live555-latest.tar.gz"
  else
    for c in "$CACHE_DIR"/live555-*.tar.gz; do
      if [ -f "$c" ]; then TARBALL="$c"; break; fi
    done
  fi
fi
if [ -z "$TARBALL" ] || [ ! -f "$TARBALL" ]; then
  echo "[live555] 找不到源码包。请先下载：" >&2
  echo "  curl -L -o '$CACHE_DIR/live555-latest.tar.gz' \\" >&2
  echo "       https://download.live555.com/live555-latest.tar.gz" >&2
  exit 1
fi
echo "[live555] 源码包：$TARBALL"

# --- 4. 解包（每次全新解，保证可重复） -------------------------------------
# 包里有一个绝对路径符号链接 live/testProgs/whiteroom.mp3 ->
# /home/ec2-user/tmp/mpeg/mp3/whiteroom.mp3（作者构建机上的路径），
# Windows 上创建不了，会让 tar 报错退出。它只是测试用的 mp3，与库无关，排除掉。
# 其余 16 个符号链接都是相对路径的 COPYING / COPYING.LESSER，不受影响。
rm -rf "$WORK"
mkdir -p "$WORK"
tar xzf "$TARBALL" -C "$WORK" --exclude='live/testProgs/whiteroom.mp3'
LIVE="$WORK/live"
if [ ! -d "$LIVE" ]; then
  echo "[live555] 解包后没有 live/ 目录：$WORK" >&2
  exit 1
fi

# --- 5. 补丁：config.mingw 第 1 行追加 -DNO_GETIFADDRS -----------------------
# 发行版的 config.mingw **没有**这个宏。缺了它，groupsock/GroupsockHelper.cpp
# 会 #include <ifaddrs.h>，而 MinGW 没有该头文件，直接编译失败。
# （源码见 GroupsockHelper.cpp:45-48，回退实现用 gethostname()，Windows 上可用）
CFG="$LIVE/config.mingw"
if [ ! -f "$CFG" ]; then
  echo "[live555] 找不到 $CFG" >&2
  exit 1
fi
if grep -q -- '-DNO_GETIFADDRS' "$CFG"; then
  echo "[live555] 补丁已存在，跳过"
else
  awk 'NR==1 { print $0 " -DNO_GETIFADDRS"; next } { print }' "$CFG" > "$CFG.new"
  mv "$CFG.new" "$CFG"
  echo "[live555] 已给 config.mingw 第 1 行追加 -DNO_GETIFADDRS"
fi

# --- 6. 构建 ---------------------------------------------------------------
cd "$LIVE"
./genMakefiles mingw
make -j"$JOBS"

# --- 7. 归置产物 -----------------------------------------------------------
stage_headers "$LIVE"
stage_libs "$LIVE"

echo "[live555] 完成：$DEST"
ls -l "$DEST/lib"
