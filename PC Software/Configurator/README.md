# PAL-sign, the configurator

Windows program that goes with the test card generator. Its sibling
`../PCMDecoder` (same folder layout) turns a captured Ham PCM or Sony
PCM picture back into sound; `../PAL-sign.sln` opens both projects in
Visual Studio 2022 or newer. From the command line:

```
dotnet run --project src/PAL-sign.csproj
```

A standalone .exe (needs the .NET 8 Desktop runtime) is built by
`publish.bat`. Use that script rather than a hand written
`dotnet publish`: the options interact in a way that is easy to get wrong
on the command line.

C# on .NET 8 (LTS) with WinForms, three tab pages: Cards (every per-card
setting, including converting a photo and sending it to one of the
fourteen Custom slots, or a photo of your own onto BBC Test Card F/W's
own oval window), Slideshow, and System (everything that is not tied to
one card).

## The risk of this program, and how it is covered

This is the **second implementation** of the image conversion. The first
is in `../../Firmware/tools/convert_image.py` and is the one proven to work
on the board. Two implementations drift apart sooner or later, and then a
difference on the screen can no longer be traced back to one cause.

That is why it has been made measurable instead of hoped for:

```
python compare.py
```

That runs both implementations on the same input and lays the output side
by side byte for byte, in two trials with a different purpose:

**A. The arithmetic.** Input that is already 720x576, so nothing is
scaled. What is left is the YCbCr conversion, the flicker filter, the
chroma halving and the level limiting. That ought to be equal byte for
byte, and it is, at flicker filter 0.00, 0.50 and 1.00. If this differs,
there is a real fault.

**B. The scaling.** No equality is expected here: PIL works out its
Lanczos weights in fixed point and rounds per pass to 8 bits, the C#
version works in double. What the trial reports is the size of the
difference, so that you can tell rounding noise from something else.

That second trial paid for itself immediately. It found that
`DrawImageUnscaled` from GDI+ scales to the **physical** size and not
pixel for pixel: a JPEG of 72 dpi was enlarged by 96/72 before the tool's
own Lanczos had even started, which is not recognisable as a fault by
eye. See the explanation in `src/ImageReader.cs`.

**Before either trial**, `compare.py` checks `ImageConvert.AlgoVersion`
(read straight from the built exe, `PAL-sign.exe --algo-version`)
against `ALGO_VERSION` in `convert_image.py`, and stops right there if
they differ. A byte-for-byte pass in trial A only proves the two are
still the same algorithm if this matched first: without it, a change on
one side that happens to still pass on whatever input trial A used that
day would go unnoticed. Whoever changes the pixel arithmetic on either
side bumps both constants, in the same commit, once trial A passes
again.

## Checking the layout

```
src/bin/Debug/net8.0-windows/PAL-sign.exe --layout layout.txt
```

Builds the window off screen, lets it lay itself out, and writes down
where every element ends up. It reports two things you spot too late by
eye: elements that **overlap each other** and elements that fall
**outside their parent**. Exit code 0 means clean.

With WinForms the order of `Controls.Add` determines who takes precedence
when docking, and an element set to `Fill` that is added **first** claims
the whole client area, leaving the rest to end up on top of it. That is
why everything now sits in a `TableLayoutPanel`.

## What the settings do

| | |
|---|---|
| **To** | which of the fourteen uploadable Custom slots you overwrite |
| **Crop** | fill the picture instead of black bars |
| **Force square-pixel scaling on an exact 720x576 source** | scale it anyway, instead of passing it through as already PAL-shaped |
| **Flicker filter** | takes the interlace twitter out; see below |

### Flicker filter

Even image rows go to field 1 and odd ones to field 2. Anything that
changes sign from row to row, a roof edge, a striped shirt, a horizontal
branch, therefore sits differently in one field than in the other, and
you see that as flicker at 25 Hz.

The kernel `[1,2,1]/4` is derived and not chosen: it is exactly the
filter that sets that alternating component to zero. See `AntiTwitter()`
in `src/ImageConvert.cs` and `anti_twitter()` in `convert_image.py`.

At 1.00 the twitter is fully gone. It costs vertical sharpness, because
the twittering component is vertical detail; at 0.50 you leave half of it
standing.

## The preview

What you see is not the bare 720x576 raster but the picture **stretched
to 4:3**, because that is how it arrives on the television. It is also
worked back out of the actual 4:2:2 bytes, so you see what the chroma
halving and the level limiting make of it as well.

## During the upload

**No picture** comes out of the board while it is writing. The board does
**not** restart and the port simply stays open: the streamer on the other
core runs from RAM and keeps sending blank lines with correct sync. The
explanation is at `photoUpload()` in `../../Firmware/src/main.cpp`.

At the end the firmware reports the CRC it has read back out of the
**flash**, and the configurator compares that with its own calculation.
If it says "CRC agrees", then it really is in there correctly.

## The command line remains

`../../Firmware/tools/convert_image.py` still does the same thing and is the
reference that `compare.py` measures against. There was also a tkinter
window; that lapsed when this program arrived.
