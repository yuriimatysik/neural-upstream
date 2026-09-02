# neural-upstream

DLSS 5 Neural Rendering runs at output resolution, after the upscaler. This
ReShade add-on moves it **upstream**: the network runs on the game's
render-resolution colour buffer, and its result is handed to the game's own DLSS
as the colour input.

The network is same-resolution only — it enhances, it does not upscale — so
running it on the smaller image costs proportionally less and the upscaler still
does the job it was going to do anyway.

Built and tested against GTA V Enhanced and the Bright Memory: Infinite
benchmark.

## How it works

**Placement.** The add-on hooks `NVSDK_NGX_D3D12_EvaluateFeature`, creates a
DLSSNR feature at the game's render resolution, runs it on the colour buffer the
game was about to hand DLSS, and rebinds the result before letting the call
through.

**Colour.** The network expects a bounded, display-referred image; this game
hands DLSS a scene-linear HDR buffer. That buffer is normalised and rolled off
through a shoulder curve before the network sees it, and the range is restored
afterwards. The restore reads the network's contribution as a per-pixel
luminance gain and applies it to the original, which keeps the full HDR range and
leaves hue and saturation exactly as the game rendered them.

Reference white for that normalisation is read from the game's own exposure
buffer on a dedicated copy queue, so it tracks day and night without a fixed
value tuned by hand.

**Cadence.** The network can run less often than every frame. The choice of which
frame is anchored to the DLSS jitter rather than to a count of calls, because the
number of evaluates per frame is not something an add-on can assume, and a
miscount silently puts the work on the wrong frame.

A skipped frame does not repeat the previous image — colour from one frame against
motion from the next is what ghosting is made of. Instead the network's *effect*
is stored, followed along the motion vectors to where each pixel was, and applied
to the current frame. Where the depth says the surface underneath has changed, it
is dropped: that is a disocclusion, and the effect waiting there belongs to
whatever used to be in front.


## Running under frame generation

Use **Quality** — the network on every frame — if DLSS Frame Generation is also on.

The network costs ~4.8 ms of an 8.9 ms rendered frame. At any cadence above 1 it
runs on one frame in two or three, so the rendered interval alternates (8.9 / 13.9
ms here) and DLSS-G, which places its generated frames inside that interval,
cannot pace through a swing that size. The result is stutter and flashes that
grow with the multiplier.

It is not an image problem: with `EffectStrength=0` — identical work, no visible
effect — the artefact is unchanged. Quality costs the same in total, spread
evenly, and paces cleanly at 4x.

What remains above Quality is flicker, from reusing one frame's effect on the
next. `FINDINGS.md` records the two attempts to correct for the delta's age, both
of which made it worse.

## Building

Third-party SDKs are not vendored. Fetch them into `external/`:

| path | source |
| --- | --- |
| `external/ngx` | NVIDIA NGX / DLSS SDK |
| `external/reshade` | ReShade add-on SDK headers |
| `external/minhook` | MinHook |
| `external/imgui` | Dear ImGui (docking branch, 1.92.x) |

Then `./build.sh`. Needs a MinGW-w64 g++ with C++20.

## Installing

The build script installs to the game folder as `nvngx.dll.addon64`.

**The filename matters.** The NGX snippet gates feature creation on the calling
module's path containing `nvngx.dll`; under any other name it returns
`0xBAD00002` and nothing happens.

Needs ReShade with add-on support, and `nvngx_dlssnr.dll` in the game folder.
Everything is configured from the ReShade overlay.

## Where the time goes

The add-on carries a GPU profiler (timestamps around each stage), so the cost is
measured rather than guessed. On an RTX 4070 Ti at 1280x720 render resolution:

| stage | cost |
| --- | --- |
| colour encode | 0.014 ms |
| **the network** | **3.27 ms** |
| colour decode | 0.017 ms |

The network is 99% of it, and about a third of a 100 fps frame. The colour
pipeline is free by comparison, so there is nothing worth optimising on this
side — cadence is the only lever that moves the number.

## Status

The network's own strength parameters — intensity, local tone, local structure,
skin structure — are forwarded and exposed, but **it has not been confirmed that
the network acts on them**. A sibling parameter, `Hint.Render.Preset`, turned out
to be inert. Compare *Light* against *AI slop* and judge for yourself: if those
two look the same, the parameters do nothing.

## Findings

[FINDINGS.md](FINDINGS.md) collects what came out of this: how to rebuild the
NR runtime for another architecture, which Hopper features have no Ada
equivalent and what to put in their place, where the time actually goes, and
the optimisations that measured null or negative.

## Licence

MIT. See [LICENSE](LICENSE).

This software contains source code provided by NVIDIA Corporation.
Not affiliated with or endorsed by NVIDIA or Rockstar Games.
