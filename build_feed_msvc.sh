#!/usr/bin/env bash
# 本机 MSVC 编译 harness（无 clang/zig 时验证 CTP/飞马 feed 接入）。
# 真实封装 RealCtpMdApi/RealFemasMdApi 在未定义 HAS_CTP/HAS_FEMAS 时不参与编译。
set -e
export MSYS_NO_PATHCONV=1 MSYS2_ENV_CONV_EXCL='*'
MSVC="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
SDK="/c/Program Files (x86)/Windows Kits/10"
CL="$MSVC/bin/Hostx64/x64/cl.exe"
export INCLUDE="$(cygpath -w "$MSVC/include");$(cygpath -w "$SDK/Include/10.0.26100.0/shared");$(cygpath -w "$SDK/Include/10.0.26100.0/um");$(cygpath -w "$SDK/Include/10.0.26100.0/ucrt")"
export LIB="$(cygpath -w "$MSVC/lib/x64");$(cygpath -w "$SDK/Lib/10.0.26100.0/um/x64");$(cygpath -w "$SDK/Lib/10.0.26100.0/ucrt/x64")"

SRC="src/feed/MarketFeed.cpp src/feed/FuturesFeed.cpp src/feed/CtpLikeCodec.cpp src/risk/RiskControl.cpp \
     src/feed/ctp/SimCtpMdApi.cpp src/feed/femas/SimFemasMdApi.cpp \
     src/feed/CtpMdFeed.cpp src/feed/FemasMdFeed.cpp"
# 真实封装仅在定义宏时编译（需 vendor SDK）
if [ -n "$HAS_CTP" ];   then SRC="$SRC src/feed/ctp/RealCtpMdApi.cpp";   fi
if [ -n "$HAS_FEMAS" ]; then SRC="$SRC src/feed/femas/RealFemasMdApi.cpp"; fi

echo "== build feed_demo =="
"$CL" /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /nologo /Iinclude $SRC src/feed_demo.cpp /Febuild/feed_demo.exe /link ws2_32.lib
echo "== build test_feed =="
"$CL" /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /nologo /Iinclude $SRC tests/test_feed.cpp /Febuild/test_feed.exe /link ws2_32.lib
echo "done-build"
