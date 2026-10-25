# Asset attribution

The console icons in this directory (`system_icons/`, excluding
`AMIGA.png`, `ATOMISWAVE.png`, `NAOMI.png`, `PC98.png`, `PICO8.png`, `POKEMINI.png`, and the UI badge glyph `_tools.png`)
are derived from public-domain video game hardware photography by **Evan Amos**.

  Author:  Evan Amos
  Source:  Vanamo Online Game Museum / Wikimedia Commons
           https://commons.wikimedia.org/wiki/User:Evan-Amos
  Licence: Public Domain (released to the public domain by the author)

These are the Cover Flow theme's own console art — distinct from the libretro
Systematic pack credited in `res/system_icons/LICENSE-ASSETS.md`. The source
photographs were background-removed (transparent alpha) and renamed from their
console names to Jawaka short codes during import. Source-size PNG masters are
kept in `artwork/Jawaka-Coverflow/system_icons`; the 384px copies in this runtime
directory are the versions packaged for the launcher. The original source
photographs are kept in the `umrk-assets` repository under `systems/icons`.
Alias codes that share a console's artwork (e.g. NES/FC, SNES/SFC,
GEN/GENESIS/MD) are local copies of the same source image.

`ATOMISWAVE.png`, `NAOMI.png` and `PC98.png` are AI-generated renders (OpenAI
ChatGPT). They replace earlier flat-vector rasterizations that did not match the
photographic style of the rest of this directory. Each was background-removed to
transparent alpha and squared to 1200x1200 on the framing the photographs above
use. Purely AI-generated images carry no copyright of their own, and no rights
are claimed in them.

`PC98.png` was composed after a reference photograph, recorded here for
provenance only — a CC0 source carries no obligation forward:

  Author:  Darklanlan
  Source:  https://commons.wikimedia.org/wiki/File:PC-9801_computer_set.jpg
  Licence: CC0 1.0 (https://creativecommons.org/publicdomain/zero/1.0/)

`ATOMISWAVE.png` and `NAOMI.png` were not composed after a specific source
image. Console and cabinet trade dress remains the property of the respective
rights holders and appears here to identify the platform, as it does in the
photographs above.

`AMIGA.png` is an AI-generated render (OpenAI ChatGPT), background-removed to
transparent alpha and squared to 1200x1200 on the framing the photographs above
use. Purely AI-generated images carry no copyright of their own, and no rights
are claimed in the render itself. It was composed after the photograph below and
follows its composition closely enough to be treated as a derivative work, so it
is distributed under that photograph's terms:

  Author:  Bill Bertram (Pixel8)
  Source:  https://commons.wikimedia.org/wiki/File:Amiga500_system.jpg
  Licence: CC BY-SA 2.5 (https://creativecommons.org/licenses/by-sa/2.5/)

Console trade dress remains the property of the respective rights holders and
appears here to identify the platform, as it does in the photographs above.

`POKEMINI.png` is a Google Gemini render of a clear-blue Pokémon mini, supplied
by the user on October 25, 2026. Reference-image details were not supplied.
The original JPEG and the keyed full-resolution PNG are kept in umrk-assets
under `Systems/ICONS/` (revision `f8dc20b7f07576069d09b67411d33ba6fa4e372e`), with the keying recipe in
that directory's `LICENSE-ASSETS.md`. The 1200x1200 master in
`artwork/Jawaka-Coverflow/system_icons` trims the keyed render to its alpha and
scales it with Pillow Lanczos filtering on premultiplied alpha to 92% of the
canvas height, the Game Boy framing; the 384x384 runtime copy is that master
downscaled the same way. Google marks Gemini output with an invisible SynthID
watermark. Purely AI-generated images carry no copyright of their own, and no
rights are claimed in the render. Nintendo's Pokémon mini trade dress and the
Nintendo and Pokémon marks remain their owners' and identify the platform.

`_tools.png` is an original Leaf UI badge, not derived from the above.

`PICO8.png` is a ChatGPT-generated illustration supplied by the user on
September 12, 2026. It depicts a fictional PICO-8 console and controller;
reference-image details were not supplied. The original 1349x1166 RGBA PNG
is archived unchanged in `umrk-assets/Systems/ICONS/PICO8.png`. This 384x384
transparent runtime copy contains the complete image, scaled and centered
without cropping or stretching. Its longest image dimension occupies 70% of
the canvas to match the visual weight of NES and PlayStation in Games.
PICO-8-pak uses a larger framing of the same illustration for its Apps entry.
No exclusive rights are claimed in the generated illustration. PICO-8 names
and marks identify the platform; this is not official Lexaloffle artwork.
