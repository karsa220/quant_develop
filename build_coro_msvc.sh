#!/usr/bin/env bash
# MSVC 本地构建脚本（本机无 clang/zig，用 VS2022 cl.exe 验证 C++20 协程）
# 用法： bash build_coro_msvc.sh
set -u
cd "$(dirname "$0")" || exit 1
export MSYS_NO_PATHCONV=1 MSYS2_ENV_CONV_EXCL='*'
MSVC="/c/Program Files/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC/14.44.35207"
SDK="/c/Program Files (x86)/Windows Kits/10"
CL="$MSVC/bin/Hostx64/x64/cl.exe"
export INCLUDE="$(cygpath -w "$MSVC/include");$(cygpath -w "$SDK/Include/10.0.26100.0/ucrt");$(cygpath -w "$SDK/Include/10.0.26100.0/um");$(cygpath -w "$SDK/Include/10.0.26100.0/shared")"
export LIB="$(cygpath -w "$MSVC/lib/x64");$(cygpath -w "$SDK/Lib/10.0.26100.0/ucrt/x64");$(cygpath -w "$SDK/Lib/10.0.26100.0/um/x64")"

SRC="src/coro/AsyncMdServer.cpp src/risk/RiskControl.cpp src/feed/CtpLikeCodec.cpp src/feed/FuturesFeed.cpp src/net/Socket.cpp src/net/IoMultiplexer.cpp"

echo "== build coro_main =="
"$CL" /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /nologo /Iinclude $SRC src/coro_main.cpp /Febuild/coro_main.exe /link ws2_32.lib
echo "== build test_coro =="
"$CL" /std:c++20 /EHsc /O2 /utf-8 /DNOMINMAX /nologo /Iinclude $SRC tests/test_coro.cpp /Febuild/test_coro.exe /link ws2_32.lib
echo "done-build"
