# hdrspace

hdrspace is a macOS app for HDR photography where the pixel values are meant to be
measurements. It turns a bracket of camera RAW files into a Radiance HDR image in absolute
units (luminance in cd/m², colour in CIE XYZ or Radiance / linear sRGB primaries), and has
tools to analyse that image: glare, visibility, tone mapping and a few perceptual maps.

I use it for daylighting and visual comfort research. Ordinary raw converters add tone
curves and white balance, so their output can't be used for luminance. hdrspace keeps the
whole chain linear and writes every step into the file header, so each result can be traced
back to the RAW files and settings that made it.

The app is for Apple Silicon Macs (developed and tested on macOS 26). The `mergehdr`
command-line tools also run on 64-bit Windows; see [mergehdr/README.rst](mergehdr/README.rst#windows).

**New here?** The [usage guide](docs/usage.md) walks through the app with screenshots.

![hdrspace viewer](docs/images/01-viewer.jpg)

## Camera RGB to calibrated colour and luminance

The engine is `mergehdr` ([`mergehdr/`](mergehdr)), a C++ re-implementation of Stephen
Wasilewski's linearhdr / pylinearhdr method.

1. **RAW decoding** with LibRaw: per-frame black level, no tone curve, no white balance.
2. **Exposure** of each frame from shutter, aperture and ISO, with optional fitted
   corrections for the real shutter times and apertures of your camera
   (`mergehdr shutter`, `mergehdr aperture`). Mixed-aperture brackets work.
3. **Merging** with linearhdr weighting and DHT demosaicing, or a faster hdrmerge-style
   path with AHD demosaicing.
4. **Colour**: camera RGB → XYZ with the camera matrix from the RAW file, or one fitted to a
   photographed colour chart (`mergehdr colorcalibrate`, `mergehdr chartcells`). Output in
   Radiance RGB, linear sRGB or XYZ, with the primaries stored in the header.
5. **Geometry**: fisheye reprojection (equisolid → equidistant, or a measured lens curve),
   vignetting correction from an angle table, and a Radiance `VIEW` line.
6. **Sun**: `shadowband` combines shadowband and ND-filter captures so the solar disc is not
   clipped.

Pixel values are stored in Radiance units (W·sr⁻¹·m⁻², i.e. cd/m² ÷ 179) whatever the output
colour space, and the luminance weights for that space are written to the header as
`LuminanceRGB` (the Y row of its RGB → XYZ matrix). Luminance is therefore
`L = 179 × (w_R R + w_G G + w_B B) / EXPOSURE`, where `EXPOSURE` is the product of any
`EXPOSURE=` lines in the header (1 if there are none):

| Output | w_R, w_G, w_B | Luminance |
|---|---|---|
| Radiance RGB | 0.265, 0.670, 0.065 | `179 × (0.265 R + 0.670 G + 0.065 B)` |
| linear sRGB (Rec. 709) | 0.2126, 0.7152, 0.0722 | `179 × (0.2126 R + 0.7152 G + 0.0722 B)` |
| XYZ | 0, 1, 0 | `179 × Y` |

Every tool in hdrspace reads files this way (Glare, View Visibility, Perceptual maps and the
viewer's luminance read-out). The one exception is an XYZ file that already holds cd/m², for
example one written by another program: mark it as *XYZ (cd/m²)* in the viewer, or choose
`--input-color xyz-cdm2` on the command line, and the ×179 is left out.

The absolute scale comes from the exposure model, so check a new camera and lens against a
luminance meter before relying on absolute values.

## Tools in the app

- **Viewer**: pixel values in the file's colour space or converted (Rad, sRGB, XYZ,
  luminance), false colour, histograms, region statistics. Files without colour information
  are shown as *Unknown* until you say what they are.
- **Merge HDR / Calibrate**: the pipeline above, plus colour, shutter and aperture
  calibration.
- **Glare**: evalglare 3.06 ported to C++ (DGP, DGI, UGR, glare sources, bands, zones).
- **View Visibility**: how much of a view's detectable contrast is kept in a second image
  (e.g. with blinds), using the visibility stages of HDR-VDP 3.
- **Perceptual maps**: cone and rod response, local adaptation, detectable contrast and
  equivalent luminance, with pixels per degree read from the `VIEW` header.
- **Tonemapping**: pfstools operators and Radiance `pcond` (Ward).
- **Crop & Projection, Blur, Create View** (Radiance rendering), and **View Volume /
  AI Segmentation** (SAM 3 and Depth Anything 3, downloaded only on request).

## Based on, and what changed

| Based on | Licence | What hdrspace changed |
|---|---|---|
| [tev](https://github.com/Tom94/tev) (Thomas Müller) | GPL-3.0 | Kept the viewer; added the tool rail, inspectors, analysis panels, colour-space handling, crop/projection and perceptual maps. |
| [linearhdr / pylinearhdr](https://github.com/stephanwaz/linearhdr) (Stephen Wasilewski) | LGPL-3.0 / GPL-3.0 | Re-implemented in C++; added calibration, projection and vignetting tools. |
| [hdrmerge](https://github.com/wjakob/hdrmerge) (Wenzel Jakob) | GPL-3.0 | Used for the fast merge path. |
| evalglare (J. Wienold, Fraunhofer ISE / EPFL) | Evalglare License 3.0 | Ported to C++; angles in degrees; equivalent-luminance and local-only modes added. This software uses a modified version of the source code of evalglare. |
| [HDR-VDP 3](https://sourceforge.net/projects/hdrvdp/) (Rafał Mantiuk) | BSD-3-Clause | Visibility and detectable-contrast stages ported from MATLAB to C++. |
| [Radiance](https://www.radiance-online.org/) (LBNL) | Radiance License 2.0 | Command-line tools bundled for `pcond` and rendering. |
| [pfstools](http://pfstools.sourceforge.net/) | GPL / LGPL | Called as external programs for tone mapping. |
| NanoGUI (Wenzel Jakob), NanoVG (Mikko Mononen) | BSD-3-Clause / zlib | Restyled dialogs, menus and tooltips. |

Known differences from the originals, all intentional: the glare output adds a `dgm`
column, uses the header's `LuminanceRGB`, divides `av_lum_pos2` once, and computes
`r_contrast` (an uninitialised value in the original). Very dark pixels (a few tens of DN)
are slightly more precise than in pylinearhdr because there is no 16-bit TIFF step.

Licence texts for all bundled components are in `hdrspace/assets/licenses`.

## Building

```
hdrspace/   the app (C++, NanoGUI, vendored tev)
mergehdr/   the HDR engine and command-line tool
```

Requirements: Xcode command-line tools, CMake, Ninja, and Homebrew OpenEXR, Imath,
jpeg-turbo, little-cms2, Exiv2, Boost, FFTW, Eigen and libomp. pfstools is optional.

Packaging also needs a runtime payload (Radiance binaries, pinned dynamic libraries,
HDR-VDP data) that is not in this repository. Place it at `hdrspace-baseline/payload` next
to these folders, then:

```
cd hdrspace
python3 tools/build_hdrspace.py --work-dir /tmp/hdrspace-build
```

The engine alone builds with CMake:

```
cd mergehdr
cmake -S . -B build-mergehdr -DCMAKE_BUILD_TYPE=Release
cmake --build build-mergehdr -j
```

## Licence

GPL-3.0 (see `LICENSE`). `mergehdr` keeps its GPL and LGPL texts in `mergehdr/COPYING` and
`mergehdr/COPYING.LESSER`.

Dong Hyun Kim, 2026
