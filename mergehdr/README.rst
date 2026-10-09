========
mergehdr
========

``mergehdr`` is a C++ command-line tool for turning a RAW exposure bracket
into an HDR image while keeping exposure scaling and camera color conversion
in the same pipeline.

What it does
------------

- loads a RAW bracket directly from camera files
- computes effective exposure from shutter, aperture, ISO, and calibration terms
- merges the sequence into HDR radiance values
- converts from camera space into ``rad``, ``srgb``, ``xyz``, or ``raw``
- can reproject an equisolid fisheye image into an equiangular view
- writes Radiance HDR output with reproducible header metadata
- includes native calibration tools for shutter, aperture, and color fitting

Run modes
---------

``mergehdr`` has two practical run modes:

- default mode
  fast merge with AHD demosaicing
- ``--bloom-prevent``
  heavier merge weighting with DHT demosaicing for highlight protection

Typical usage::

   mergehdr -profile R5m2 run "*.CR3" > out.hdr
   mergehdr -profile R5m2 run --bloom-prevent "*.CR3" > out.hdr
   mergehdr run --colorspace xyz --no-fisheye "*.CR3" > out.hdr

Profiles
--------

``profiles/`` holds ready-made presets for the setup mergehdr was tested with:

- camera: Canon EOS R5 Mark II
- lens: Canon EF 8-15mm f/4L Fisheye USM, on an EF-RF mount adapter
- filters: Kolari Clear, plus an ND3 (ND 3.0) filter for bright skies

``R5m2`` is the preset for this camera and lens. ``R5m2ND3`` is the same setup
with the ND3 filter: it adds ``nd = 3.0`` and has its own colour matrix. For
other equipment, make your own preset with the calibration tools below.

Lens projection
---------------

``convertprojection`` converts a merged fisheye HDR between projections. A
measured lens curve can be given as four coefficients of
``rho=C1*phi+C2*phi^2+C3*phi^3+C4*phi^4``, with ``phi`` in radians::

   mergehdr convertprojection --equisolid --equidistant \
     -p C1 C2 C3 C4 -o corrected.hdr uncorrected.hdr

Source and target radii default to half the square input width. Use
``--source-radius`` and ``--target-radius`` only when the fisheye disk or the
90-degree target radius differs from that.

Vignetting correction
---------------------

``-vfile``/``--vfile`` accepts the same angle-table format as
``pylinearhdr``: either ``angle factor`` or ``angle red green blue``. The
table is linearly interpolated (and linearly extrapolated beyond its endpoint)
at 180-degree equiangular pixel-center angles, then multiplied into the
destination RGB image after any fisheye or calibrated lens projection::

   mergehdr -profile R5m2 run \
     -vfile /path/to/F-22_vignetting.txt \
     -o corrected.hdr "*.CR3"

The output header preserves ``VIEW`` and records ``VIGNETTING_CORRECTION``.
The final HDR must be square. A ``vfile`` key may also be placed in the
``[globals]`` section of a mergehdr config/profile.

Calibration tools
-----------------

``mergehdr`` also provides native calibration commands that stay outside the
normal merge hot path:

- ``mergehdr shutter``
  estimates ``shutterc`` from RAW samples
- ``mergehdr aperture``
  estimates ``fo`` aperture correction pairs
- ``mergehdr colorcalibrate``
  fits candidate ``XYZCAM`` matrices from HDR or TSV reference/test data
- ``mergehdr chartcells``
  detects a cropped 6x4 chart grid and writes cell rectangles

Experimental image VOS (SAM3 + DA3Metric)
-----------------------------------------

``bin/mergehdr-image-vos`` estimates radial metric depth from an HDR view and
combines it with separate building and greenery masks. It is independent of
``view_visibility``: the latter compares visible contrast between two HDR
images, while image VOS estimates scene volume from one segmented view.
Radiance ``VIEW=`` metadata is read automatically: ``-vta`` images use an
equidistant solid-angle map and five perspective DA3 tiles, while ``-vtv``
images use an exact perspective solid-angle map and direct DA3 inference.

On Apple Silicon, install the isolated MPS runtime and the official
``depth-anything/DA3METRIC-LARGE`` weights once::

   bin/mergehdr-image-vos install

Then run the existing hdrspace SAM3 segmentation and DA3/VOS analysis in one
command::

   bin/mergehdr-image-vos auto \
     --input view.hdr \
     --output-dir view_image_vos

When a matching exterior HDR is available, paired mode analyzes both images,
angularly reprojects the exterior depth and masks into the interior VIEW, and
reports building/greenery depth and VOS differences::

   mergehdr view-vos-calculation \
     --interior room_view.hdr \
     --exterior exterior_view.hdr \
     --window-distance 1.5 \
     --window-mask window.png \
     --output paired_image_vos

``--window-mask`` is optional. When supplied, white pixels define the glazing
view included in building/greenery VOS and black pixels exclude walls, frames,
and other interior occlusions; PNG and HDR masks are supported. When it is
omitted, the full valid Radiance ``VIEW`` is used, preserving the existing
behavior. If ``--window-distance`` is omitted, the command asks for y/n
confirmation before using a DA3 image-based estimate.

``pair_comparison.json`` preserves both unmodified DA3 results and adds an
exterior reference corrected from the glazing plane to the interior observer.
``pair_depth_comparison.png`` places the internal DA3 result beside the aligned
exterior reference. Radiance ``VIEW`` direction/up metadata is used when it is
available; otherwise paired mode assumes that the two view centres and
orientations are already registered. The distance correction assumes that the
exterior camera was located at the glazing plane.

Alternatively, analyze a 180-degree equidistant image using reviewed masks
exported by SAM3::

   bin/mergehdr-image-vos analyze \
     --input view.hdr \
     --building-mask building.png \
     --greenery-mask greenery.png \
     --output-dir view_image_vos

For hdrspace candidate folders, replace the mask arguments with
``--building-candidates`` and ``--greenery-candidates``. The helper uses five
overlapping rectilinear tiles rather than sending the distorted fisheye image
directly to DA3. ``image_vos.hdr`` packs building VOS, greenery VOS, and radial
depth in its R, G, and B channels; ``result.json`` contains the totals and
model/runtime provenance.

The implemented per-pixel calculation is
``dV = solid_angle_sr * max(radial_depth_m^3 - glazing_depth_m^3, 0) / 3``.
``result.json`` reports ``V_built`` and ``V_greenery`` as the primary results.
View satisfaction is not calculated by default because the regression
coefficients have not yet been established. Once coefficients are calibrated,
provide all three optional satisfaction arguments to evaluate
``a log10(1 + V_built) + b log10(1 + V_greenery) + c``. Depth is clipped to
100 m by default because VOS is cubic in distance. Monocular metric depth is
an estimate, so results should be calibrated against known distances
before they are used as measured physical data.

For an image without Radiance projection metadata, provide the geometry
explicitly, for example ``--projection perspective --horizontal-fov 100``;
the vertical FOV is derived from the image aspect ratio. Explicit
``--horizontal-fov`` and ``--vertical-fov`` values override header values.

Repository layout
-----------------

- ``mergehdr_cli.cpp``
  user-facing CLI
- ``main.cpp``
  core entry point
- ``input.cpp``
  RAW loading and metadata collection
- ``hdr.cpp``
  merge, demosaic, color, and reprojection stages
- ``output.cpp``
  HDR and EXR writers
- ``camera_detect.cpp``
  native camera-matrix detection from the first RAW file
- ``profiles/``
  saved camera and workflow profiles
- ``bin/mergehdr``
  small launcher for the local build
- ``bin/mergehdr-image-vos``
  Apple-Silicon SAM3/DA3Metric image-VOS launcher
- ``tools/da3_vos_helper.py``
  fisheye tiling, metric-depth stitching, and VOS calculation
- ``Eigen/``
  bundled header-only dependency

Build
-----

Requirements:

- CMake
- OpenEXR
- Exiv2
- LibRaw
- JPEG
- Boost filesystem and program_options
- Eigen
- OpenMP support is used when available
- no separate optimizer package is required for ``colorcalibrate``;
  the needed SLSQP sources are bundled in ``third_party/nlopt_slsqp/``

Build locally::

   cmake -S . -B build-mergehdr -DCMAKE_BUILD_TYPE=Release
   cmake --build build-mergehdr -j

The local launcher in ``bin/mergehdr`` expects the binary to live in
``build-mergehdr/``.

Notes
-----

- ``--xyzcam`` can be passed explicitly.
- If ``--xyzcam`` is omitted, ``mergehdr`` reads the matrix from the first RAW
  file natively through LibRaw.
- Build directories are local artifacts and are ignored by git.
- RAW black correction and full-scale white are decoded for every exposure.
  Before merging, each frame is normalized to a common black/white encoding,
  so mixed ISO values or RAW bit depths do not hide clipped samples.
  ``--blacklevel`` sets that common baseline after LibRaw's per-frame black
  correction; it is not subtracted a second time. Profile white/saturation
  settings then operate on the common encoding. Camera photometric calibration
  must be checked when changing the RAW normalization workflow.

Acknowledgements
----------------

This project draws on ideas and source work from:

- ``linearhdr`` by Stephen Wasilewski
  https://github.com/stephanwaz/linearhdr
- ``hdrmerge`` by Wenzel Jakob
  https://github.com/wjakob/hdrmerge
- ``LibRaw`` by LibRaw LLC
  https://www.libraw.org/
- ``NLopt`` by Steven G. Johnson and collaborators
  https://nlopt.readthedocs.io/

License notes
-------------

- This repository keeps the GPL and LGPL license texts in ``COPYING`` and
  ``COPYING.LESSER``.
- ``dht_demosaic_linearhdr.cpp`` carries its own copyright and license notice
  in the file header and should be kept with that notice intact.
- ``third_party/nlopt_slsqp/`` keeps the original NLopt copyright headers and
  README/COPYRIGHT files for the vendored SLSQP implementation.
- ``THIRD_PARTY_NOTICES.rst`` summarizes the bundled and referenced external
  components used in this repository.
