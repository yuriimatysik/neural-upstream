#!/bin/sh
set -eu
cd "$(dirname "$0")/.."

# Only source and headers are needed. Do not download NVIDIA runtime binaries.
fetch() {
    name=$1 url=$2 revision=$3
    target="external/$name"
    if [ -d "$target/.git" ]; then
        actual=$(git -C "$target" rev-parse HEAD)
        [ "$actual" = "$revision" ] || {
            echo "$target is at $actual; expected $revision. Preserve local changes and switch it explicitly." >&2
            exit 1
        }
        return
    fi
    git init "$target"
    git -C "$target" remote add origin "$url"
    git -C "$target" sparse-checkout init --no-cone
    git -C "$target" sparse-checkout set --no-cone '/include/' '/src/' '/*.h' '/*LICENSE*' '/*license*'
    git -C "$target" fetch --depth 1 --filter=blob:none origin "$revision"
    git -C "$target" checkout --detach FETCH_HEAD
}

fetch ngx https://github.com/NVIDIA/DLSS.git a291cc7d2cc642a51566f3dfd5376f635cd1b284
fetch minhook https://github.com/TsudaKageyu/minhook.git c3fcafdc10146beb5919319d0683e44e3c30d537
fetch reshade https://github.com/crosire/reshade.git 358c345ca2fe64f86e67c694f8379c356627adcb
fetch imgui https://github.com/ocornut/imgui.git 3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c
