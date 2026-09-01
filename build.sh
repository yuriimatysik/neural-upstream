#!/bin/sh
set -e
cd "$(dirname "$0")"
g++ -shared -std=c++20 -O2 -DNDEBUG -w \
  -I external/reshade/include -I external/ngx/include \
  -I external/minhook/include -I external/imgui \
  -o neural-upstream.addon64 src/addon.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -ld3d12 -ldxgi -static -static-libgcc -static-libstdc++
echo "built: $(ls -la neural-upstream.addon64 | awk '{print $5}') bytes"
# Installing is deliberate, never a side effect of building: the target folder is
# usually a game someone is playing, and quietly swapping the addon underneath a
# working setup is how a good build becomes a broken one.
#   install:  cp neural-upstream.addon64 "<game>/nvngx.dll.addon64"
# The filename is load-bearing -- the NGX snippet gate checks the caller module
# path for "nvngx.dll".
