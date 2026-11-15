# Windows executable icons

## Setup

`setup-icon.png` was adapted from the user's supplied `1788857854985.jpg`
using the built-in ImageGen tool. The original file was left unchanged.
The generated artwork is stored in the project, not referenced from a temporary
or per-user generated-images directory.

Final prompt:

> Use case: precise-object-edit / icon adaptation.
> Edit target: the attached KOKAKU RECOMP artwork featuring Motoko, a blue moon and a night city. Recompose this actual artwork into ONE square Windows setup application icon, 1024x1024, with straight square corners and a crisp very thin icy-blue outline.
> Preserve Motoko's recognizable face, purple bob haircut, black tactical clothing and the source's anime cel art style. Retain the luminous blue moon and dark navy night as simplified supporting backdrop. Crop/reframe so her face and upper shoulders are large and legible at 32px, positioned centrally; remove the long body and pistol from the framing. Keep her expression and face faithful to the input. The head should fill most of the square, without clipping hair. Remove the original large KOKAKU RECOMP lettering entirely because it will be unreadable in a desktop icon. Add only a compact dark navy rectangular badge across the bottom with the exact white text "SETUP" in large block sans capitals, modest enough not to cover the face. No additional text, no mockup, no multiple variants, no exterior drop shadow, no rounded corners. Palette dark navy, light icy blue, black, restrained purple and natural face colors. Deliver a finished square raster icon asset, not a presentation sheet.

## Game

`game-icon.png` is an unchanged copy of the user's `GAMEICON.png` (RGBA,
511 x 512). ICO encoding pads one transparent column to make a square before
downsampling. No background replacement, recoloring or AI regeneration.

Both `setup.ico` and `game.ico` include 16, 20, 24, 32, 40, 48, 64, 96, 128,
and 256 pixel representations. Source alpha is retained.
