hdrspace - third-party licenses
================================

hdrspace (c) 2026 Dong Hyun Kim is free software under the GNU General Public
License v3. It contains or builds on the components listed below; each file
in this folder holds the license text(s) of one component, taken from the
sources that were compiled into the app, from the mergehdr sources, from
assets/license-notices or, for the runtime libraries in Contents/Frameworks,
from the matching Homebrew packages on the build machine.

Required acknowledgements:
  This product includes the evalglare software, developed at Fraunhofer ISE
  and EPFL by J. Wienold.
  This software uses a modified version of the source code of evalglare.
  This product includes Radiance software (http://radsite.lbl.gov/) developed
  by the Lawrence Berkeley National Laboratory (http://www.lbl.gov/).

Copyleft components: hdrspace, tev and hdrmerge (GPL-3.0), mergehdr
(LGPL-3.0 / GPL-3.0), Exiv2 and FFTW (GPL-2.0-or-later), LibRaw, libexif and
libintl (LGPL-2.1), libgomp (GPL-3.0 with the GCC runtime exception).

Not included: the AI models (SAM 3, Depth Anything 3) and the SpectralDB
material list are downloaded on request and keep their own licenses.

Regenerate with: python3 tools/collect_hdrspace_licenses.py

hdrspace and mergehdr                                   GPL-3.0 (mergehdr: LGPL-3.0 / GPL-3.0)  [this build]  -> hdrspace-and-mergehdr.txt
tev                                                     GPL-3.0  [vendored, modified]  -> tev.txt
hdrmerge                                                GPL-3.0  [merge code in mergehdr]  -> hdrmerge.txt
evalglare                                               Evalglare Software License 3.0x  [glare analysis (C++ re-implementation); original in Contents/Resources/radiance]  -> evalglare.txt
Radiance                                                Radiance Software License 2.0  [Contents/Resources/radiance]  -> radiance.txt
HDR-VDP 3                                               BSD-3-Clause  [3.0.7 (View Visibility port and spectral data)]  -> hdr-vdp-3.txt
matlabPyrTools (convolve.c, edges.c)                    MIT  [vendored in vendor/hdrvdp-mex]  -> matlabpyrtools-convolve-c-edges-c.txt
NLopt SLSQP                                             BSD-3-Clause  [vendored in mergehdr/third_party/nlopt_slsqp]  -> nlopt-slsqp.txt
NanoGUI                                                 BSD-3-Clause  [vendored]  -> nanogui.txt
NanoVG                                                  Zlib  [vendored]  -> nanovg.txt
MetalNanoVG                                             MIT  [vendored]  -> metalnanovg.txt
GLFW                                                    Zlib  [vendored]  -> glfw.txt
nativefiledialog-extended                               Zlib  [vendored]  -> nativefiledialog-extended.txt
clip                                                    MIT  [vendored]  -> clip.txt
OpenEXR                                                 BSD-3-Clause  [vendored; Frameworks 3.4.9]  -> openexr.txt
Imath                                                   BSD-3-Clause  [vendored; Frameworks 3.2.2]  -> imath.txt
LibRaw                                                  LGPL-2.1 or CDDL-1.0  [vendored; Frameworks libraw.24]  -> libraw.txt
Little-CMS                                              MIT  [vendored; Frameworks liblcms2.2]  -> little-cms.txt
libjpeg-turbo                                           IJG AND BSD-3-Clause AND Zlib  [vendored; Frameworks libjpeg.8.3.2]  -> libjpeg-turbo.txt
libpng                                                  libpng-2.0  [vendored]  -> libpng.txt
zlib                                                    Zlib  [vendored]  -> zlib.txt
LibTIFF                                                 libtiff  [vendored]  -> libtiff.txt
libdeflate                                              MIT  [vendored; Frameworks libdeflate.0]  -> libdeflate.txt
libwebp                                                 BSD-3-Clause (with patent grant)  [vendored]  -> libwebp.txt
OpenJPEG                                                BSD-2-Clause  [vendored]  -> openjpeg.txt
libjxl                                                  BSD-3-Clause (with patent grant)  [vendored]  -> libjxl.txt
Brotli                                                  MIT  [vendored with libjxl; Frameworks 1.2.0]  -> brotli.txt
Highway                                                 Apache-2.0 or BSD-3-Clause  [vendored with libjxl]  -> highway.txt
libexif                                                 LGPL-2.1  [vendored]  -> libexif.txt
Expat                                                   MIT  [vendored]  -> expat.txt
XMP Toolkit SDK                                         BSD-3-Clause  [vendored]  -> xmp-toolkit-sdk.txt
args                                                    MIT  [vendored]  -> args.txt
qoi                                                     MIT  [vendored]  -> qoi.txt
small_vector                                            MIT  [vendored]  -> small-vector.txt
stb_image and stb_image_write                           MIT or Unlicense  [vendored]  -> stb-image-and-stb-image-write.txt
tinylogger                                              see text  [vendored]  -> tinylogger.txt
concurrentqueue                                         BSD-2-Clause or BSL-1.0  [vendored]  -> concurrentqueue.txt
UTF8-CPP                                                BSL-1.0  [vendored]  -> utf8-cpp.txt
Eigen                                                   MPL-2.0  [header-only (Homebrew)]  -> eigen.txt
Roboto font                                             Apache-2.0  [embedded by NanoGUI]  -> roboto-font.txt
Font Awesome Free 5 and Inconsolata fonts               OFL-1.1  [embedded by NanoGUI]  -> font-awesome-free-5-and-inconsolata-fonts.txt
Boost (atomic, container, filesystem, program_options)  BSL-1.0  [Frameworks]  -> boost-atomic-container-filesystem-program-options.txt
Exiv2                                                   GPL-2.0-or-later  [Frameworks 0.28.7]  -> exiv2.txt
FFTW                                                    GPL-2.0-or-later  [Frameworks 3.3.10]  -> fftw.txt
GCC OpenMP runtime (libgomp)                            GPL-3.0 WITH GCC-exception-3.1  [Frameworks]  -> gcc-openmp-runtime-libgomp.txt
GNU gettext runtime (libintl)                           LGPL-2.1-or-later  [Frameworks libintl.8]  -> gnu-gettext-runtime-libintl.txt
inih and INIReader                                      BSD-3-Clause  [Frameworks]  -> inih-and-inireader.txt
LLVM OpenMP runtime (libomp)                            Apache-2.0 WITH LLVM-exception  [Frameworks]  -> llvm-openmp-runtime-libomp.txt
OpenJPH                                                 BSD-2-Clause  [Frameworks 0.27.0]  -> openjph.txt
