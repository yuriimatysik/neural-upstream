#!/bin/sh
set -e
cd "$(dirname "$0")"
GAME="/c/Program Files (x86)/Steam/steamapps/common/Grand Theft Auto V Enhanced"
g++ -shared -std=c++20 -O2 -DNDEBUG -w \
  -I external/reshade/include -I external/ngx/include \
  -I external/minhook/include -I external/imgui \
  -o neural-upstream.addon64 src/addon.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
# the NGX snippet gate checks the caller module name: this filename is load-bearing
cp neural-upstream.addon64 "$GAME/nvngx.dll.addon64"
echo "built + installed: $(ls -la neural-upstream.addon64 | awk '{print $5}') bytes"
