neural-upstream
===============

Runs DLSS 5 Neural Rendering at render resolution, before the game's own DLSS
upscales, instead of at output resolution afterwards.

Requires: RTX card with DLSS 5 Neural Rendering, ReShade with add-on support,
and nvngx_dlssnr.dll present in the game folder.


INSTALL
-------
Copy  nvngx.dll.addon64  into the game folder, next to the game executable.

Do not rename it. The NGX snippet only accepts feature creation from a module
whose path contains "nvngx.dll"; under any other name it fails with 0xBAD00002
and nothing happens.

Everything is configured from the ReShade overlay, under "DLSS5 NR Pre-Upscale".


SETTINGS
--------
How often it runs    Quality runs the network every frame -- best image.
                     Balanced and Performance skip frames and reconstruct them,
                     which costs some trailing and shimmer on moving edges.

How transformative   How far the network may reinterpret the image. Reference is
                     what it produces on its own; the lower settings pull back
                     its micro-detail first, which is where invented texture
                     comes from. NOTE: these reach the network but it has not
                     been confirmed that it acts on them -- compare Light against
                     Reference and see for yourself.

Effect strength      How much of the result is kept. Dilutes everything evenly.

Exposure / Auto      Derives reference white from the game's own exposure, so it
                     follows day and night. Leave it on.


NOTICES
-------
This software contains source code provided by NVIDIA Corporation.

Not affiliated with or endorsed by NVIDIA or Rockstar Games. Provided as-is.
