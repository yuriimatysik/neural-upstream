#!/bin/sh
# Two binaries, and which code lives in which one is the whole design.
#
#   nvngx.dll.nr  the add-on, entire. Every NGX call leaves from here, because the
#                 DLSSNR snippet tests the *calling module's* path for "nvngx.dll"
#                 and refuses anything else. Splitting the calls out into a thin
#                 forwarder was tried and does not work: Init_Ext is accepted but
#                 CreateFeature never returns. Under ReShade one module does all of
#                 it -- nvngx.dll.addon64 -- and this mirrors that exactly.
#
#   version.dll   bootstrap only. It forwards version.dll's exports, captures the
#                 D3D12 device and direct queue, loads the add-on and hands them
#                 over. It makes no NGX calls at all.
set -e
cd "$(dirname "$0")/.."

g++ -shared -std=c++20 -O2 -DNDEBUG -w -DNR_STANDALONE \
  -I proxy/shim -I proxy -I external/ngx/include -I external/minhook/include \
  -o proxy/nvngx.dll.nr src/addon.cpp proxy/shim.impl.cpp proxy/satellite.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -static -static-libgcc -static-libstdc++ \
  -ld3d12 -ld3dcompiler -ldxgi -lpsapi -lkernel32

g++ -shared -std=c++20 -O2 -DNDEBUG -w -I external/minhook/include \
  -o proxy/version.dll proxy/nrproxy.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup \
  -ld3d12 -lkernel32

echo "built: nvngx.dll.nr $(ls -la proxy/nvngx.dll.nr | awk '{print $5}') bytes  (the add-on)"
echo "       version.dll  $(ls -la proxy/version.dll | awk '{print $5}') bytes  (bootstrap)"
