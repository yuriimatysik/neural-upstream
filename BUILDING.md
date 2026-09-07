# Combined compatibility fixes

This fork combines [upstream issue #3](https://github.com/matiasLombo/neural-upstream/issues/3)
with [PR #4](https://github.com/matiasLombo/neural-upstream/pull/4), based on PR commit
`804fd00bf12b4927c01891bdbde533bec3e951b8`.

- Use actual Color resource dimensions for NR feature/output/codec allocation.
- Bind NR output before feature creation and track the creator's evaluate/release functions.
- Preserve PR #4's scratch-query fallback, explicit geometry and typed SRV mappings.
- Insert a UAV barrier after NR evaluation.
- Pass resize frames through before recording NR/codec work, including `Codec=0`.
- Defer cleanup until recorded command lists have actually been submitted, all their
  graphics queue fences have completed, and the exposure copy queue has completed.
  The present tick polls fences without blocking the game thread; standalone mode
  uses the same submission tracking, not an assumption that the next evaluate is a new frame.
- Clear feature pointers, ownership and temporal state after cleanup.

If a command list is discarded without submission, or a fence fails, retain the old
resources and stay in passthrough rather than free potentially live GPU resources.

## Build

GitHub: open **Actions → Build Windows DLLs**, select a successful run for your
commit, and download `neural-upstream-windows-x64` from **Artifacts**.
The workflow can also be started manually with **Run workflow**.

Local Linux cross-build (also suitable for WSL Ubuntu):

```sh
sudo apt-get update
sudo apt-get install git g++-mingw-w64-x86-64-posix
sh scripts/fetch-dependencies.sh
export CXX=x86_64-w64-mingw32-g++-posix
sh build.sh
sh proxy/build.sh
```

Windows: run the same scripts in a MinGW-w64 environment with POSIX thread support
and C++20, using `CXX=g++`. SDK/source revisions are pinned by the fetch script.
No game folder is modified by building.

## Outputs

Choose one integration:

| Integration | Files |
| --- | --- |
| ReShade | Rename `neural-upstream.addon64` to `nvngx.dll.addon64` beside the game executable. Requires ReShade with add-on support. |
| Standalone | Copy **both** `proxy/version.dll` and `proxy/nvngx.dll.nr` beside the game executable. ReShade is not required. |

The satellite name `nvngx.dll.nr` is significant to NGX. Do not rename it to
`nvngx.dll`. These builds do not contain NVIDIA's `nvngx_dlssnr.dll`, model data,
or a replacement game DLSS runtime; a working compatible NR runtime is still needed.
If a different mod already supplies `version.dll`, resolve that loader conflict
before installing the standalone build.

## GPU validation still required

Compilation and export checks do not establish game compatibility. Test:

1. Color `1708×964` with render-subrect `1708×961`; confirm NR and codec use `1708×964`.
2. Quality/Balanced/Performance changes in quick succession; inspect for device removal.
3. Repeat step 2 with `Codec=0` and `Rebind=1`; reset frames must use the game's Color.
4. Alt-tab, swapchain resize, both ReShade and standalone integration.
5. Core-created and snippet-created features; verify evaluate/release use the same owner.
6. Typeless Color/MotionVectors and exposure readback during resize.
7. Use the D3D12 debug layer/GPU validation where the game supports it; inspect
   logs for `cleanup requested` followed by `GPU submissions complete`.

Do not describe these binaries as game-tested until these checks run on the target GPU.
