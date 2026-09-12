# neural-upstream

## What this fork adds

This fork combines the Stellar Blade fixes from upstream issue #3, the
community-runtime fixes in PR #4, Leonardo Capellaro's stability and frame
generation work, his v0.5.0 highlight changes, and additional
recovery fixes for the in-game controls.

### Bug fixes

- **NR interfering with frame generation:** restrict NR processing to DLSS
  upscaling and Ray Reconstruction features, bypassing frame-generation and
  unknown NGX features so their inputs are not modified.
- **GPU descriptors reused while still in flight:** keep descriptor slots owned
  by their command-list recordings until every submitting queue has finished,
  preventing later frames from overwriting descriptors the GPU still needs.
- **NR getting stuck after resize or alt-tab:** track native and wrapped command
  lists through submission, reset and destruction so cleanup can finish and NR
  can resume safely.
- **Unstable depth guides and stalled cadence:** use the game's DLSS depth
  convention instead of a changing screen sample, reset temporal history when
  that convention changes, and recover when jitter stops advancing.
- **Enabled NR remaining invisible:** repair old configurations that saved
  `EffectStrength=0`, and reset temporal history when F7 or the overlay turns NR
  back on.
- **Gameplay hitches from diagnostics:** keep GPU timestamp readback and verbose
  logging opt-in, under **Developer diagnostics** in the overlay.
- **Early NGX hook failure:** retry until a complete lifecycle/evaluate hook set
  succeeds and remove partial hooks before retrying.
- **Repeated passes compounding the image:** permanently lock NR to one network
  pass and repair old configurations that saved a higher `Passes` value.

It also includes the earlier resource-dimension and typed-view corrections,
explicit NR input/output geometry, scratch-buffer fallback, safe resource
cleanup during resolution changes and preservation of the live enabled state.

### Tested games

Leonardo reports no flicker in his fork when testing:

- GTA V
- Forza Horizon 6 — Xbox app version
- Assassin's Creed Black Flag Resynced
- Avatar: Frontiers of Pandora

For frame generation, use the **Quality** cadence described below. The current
Dawnwalker candidate and MFG behavior still need extended in-game validation.

### Thanks and credits

Thank you to **matiasLombo** for the
[original neural-upstream project](https://github.com/matiasLombo/neural-upstream),
**Devin Mesenbrink** for the community Ada-runtime fixes in
[PR #4](https://github.com/matiasLombo/neural-upstream/pull/4), and everyone who
contributed testing and bug reports through
[Stellar Blade issue #3](https://github.com/matiasLombo/neural-upstream/issues/3).

Special thanks to **Leonardo Capellaro** for the
[stability and frame-generation fixes](https://github.com/leonardocapellaro/neural-upstream).
His submission tracking, feature isolation, descriptor lifetime and depth-guide
work, plus the highlight/detail controls, are incorporated into this fork.

The experimental physical-resolution path adapts the downsample and matched
residual concept from xenmods'
[DLSSNR-Cost-Scaler v1.0.5](https://github.com/xenmods/DLSSNR-Cost-Scaler/tree/9bb03663d690b84ec00cdb55fb3a1a04dd881e02).
Its external HDR resolve, RCAS, hotkeys and lifetime scheme are not included.

See [BUILDING.md](BUILDING.md) for pinned dependencies, automated Windows DLL
builds, installation and detailed validation notes.

## About neural-upstream

DLSS 5 Neural Rendering runs at output resolution, after the upscaler. This
ReShade add-on moves it **upstream**: the network runs on the game's
render-resolution colour buffer, and its result is handed to the game's own DLSS
as the colour input.

The network is same-resolution only — it enhances, it does not upscale — so
running it on the smaller image costs proportionally less and the upscaler still
does the job it was going to do anyway.

The original project was built and tested against GTA V Enhanced and the Bright
Memory: Infinite benchmark; additional community testing is listed above.

## How it works

**Placement.** The add-on hooks `NVSDK_NGX_D3D12_EvaluateFeature`, creates a
DLSSNR feature at the game's render resolution, runs it on the colour buffer the
game was about to hand DLSS, and rebinds the result before letting the call
through.

**Experimental neural resolution scale.** `ResolutionScale` physically reduces
the encoded NR input and output dimensions from 50% to 100%. At values below
100%, one bilinear pass creates the smaller proxy, NR runs at those real
dimensions with `DLSSNR.ScalingRatio=1.0`, and one integrated matched-residual
decode adds `lowNR - lowInput` back to the full-resolution proxy before the
existing HDR/chroma/detail restore. A five-tap full-resolution depth check fades
the residual to 25% around discontinuities. Missing or incompatible depth simply
disables that guard for the frame.

The default is `ResolutionScale=1.0`. At 100% the scaler PSOs, dispatches and
small textures are not created, and the established encode → NR → decode path is
used unchanged. The old `NetScale` setting is ignored and normalized to `1.0`, so
it cannot trigger a second NGX scaling operation.

In ReShade, move **Neural resolution scale**, choose 100%/85%/75%, then press
**Apply**. Apply pauses NR in passthrough until fence-tracked GPU work finishes;
several edits during cleanup collapse to the newest requested value. Scaled mode
requires the codec and always uses Quality cadence without async snapshots or
delta reuse. The standalone build reads the same `ResolutionScale` key from
`neural-upstream.ini`; restart the game to apply it there.

**Colour.** The network expects a bounded, display-referred image; this game
hands DLSS a scene-linear HDR buffer. That buffer is normalised and rolled off
through a shoulder curve before the network sees it, and the range is restored
afterwards. The restore reads the network's contribution as a per-pixel
luminance gain and applies it to the original, which keeps the full HDR range and
leaves hue and saturation exactly as the game rendered them.

Reference white is estimated from a scene histogram. Direct copying of the
game's exposure texture is disabled because its resource state cannot be proven
safe from an NGX hook.

**Highlight and detail controls.** `Highlight curve` defaults to **Preserve
highlights**: a gradual shoulder retains more differences between bright values
in the FP16 network input, with gamut compression to keep saturated colours in
range. **Legacy** selects the previous exponential shoulder and colour handling.
This changes what the network sees; visual improvement still needs comparison
in each game.

`Detail strength` adjusts fine variations in the network's luminance gain;
`Lighting strength` adjusts its locally averaged component. Both default to
**1.00**, which uses the original gain transfer without neighbourhood filtering.
Non-default values use an edge-weighted five-tap estimate; extra detail gain is
limited to a quarter stop and the overall gain remains within 1/8 to 8. This is
a local approximation of lighting versus detail, not a semantic separation.
Both direct and reused-frame decode apply the same adjustment. Its GPU cost
has not yet been measured.

For comparison, keep cadence **Quality**, one network pass, and effect/transfer
at **1.00**. First compare the two curves with detail and lighting at **1.00**;
then try detail **1.15**, keeping lighting at **1.00**. Compare a stationary scene
and camera motion, including bright textures, foliage and skin. Changing these
controls resets NR history and cached output; allow it to settle before comparing.
Settings persist as `EncodeCurve` (0 legacy, 1 preserve highlights),
`DetailStrength` (0–2) and `LightingStrength` (0–1.5) in `[NRPreUpscale]`.

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

The build script writes `nvngx.dll.addon64`. Copy it to the game folder.

**The filename matters.** The NGX snippet gates feature creation on the calling
module's path containing `nvngx.dll`; under any other name it returns
`0xBAD00002` and nothing happens.

Needs ReShade with add-on support, and `nvngx_dlssnr.dll` in the game folder.
Everything is configured from the ReShade overlay.

`F7` is the only Neural Rendering hotkey and clears temporal history when turning
NR back on. The overlay can also toggle NR. A short debounce prevents one key
press from toggling twice. Network passes are locked to one because repeated
passes compound the effect, darken the image and can make highlights look
artificial. Old `Passes` values are repaired to `1` when loaded. `F6` and `F9`
no longer change NR state.

## Where the time goes

The add-on carries a GPU profiler (timestamps around each stage), so the cost is
measured rather than guessed. On an RTX 4070 Ti at 1280x720 render resolution:

| stage | cost |
| --- | --- |
| colour encode | 0.014 ms |
| **the network** | **3.27 ms** |
| colour decode | 0.017 ms |

The network is 99% of it, and about a third of a 100 fps frame. The colour
pipeline is small by comparison at the original transfer settings. These timings
predate the optional neighbourhood detail adjustment; measure its cost separately.

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
