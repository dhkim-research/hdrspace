#!/usr/bin/env python3
"""Assemble assets/licenses: one text file per third-party component in hdrspace.app.

Texts come from the vendored sources that are compiled into the app, from the
mergehdr sources next to hdrspace, from assets/license-notices (verbatim texts that
exist nowhere else as files) and, for the runtime libraries copied unchanged from
the baseline app, from the matching Homebrew kegs on the build machine. Run it
again when a dependency changes:

    python3 tools/collect_hdrspace_licenses.py
"""
import argparse
from pathlib import Path
import re
import shutil
import tempfile

DEPS = "vendor/tev/dependencies"
GUI = "hdrspace (main executable, statically linked)"
CLI = "mergehdr command-line tools (Contents/MacOS/mergehdr, mergehdrcore)"
FW = "Contents/Frameworks (runtime library, copied unchanged from the baseline app)"
RES = "Contents/Resources"

PYRTOOLS_MIT = """MIT License

Copyright (c) 2015 LabForComputationalVision

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
"""

EVALGLARE_NOTE = """Required notices (Evalglare Software License, conditions 3 and 6):

  This product includes the evalglare software, developed at Fraunhofer ISE and
  EPFL by J. Wienold.

  This software uses a modified version of the source code of evalglare.

The glare analysis in hdrspace and mergehdr is a C++ re-implementation of
evalglare by Dong Hyun Kim. The Radiance build in Contents/Resources/radiance
also contains the original evalglare program.
"""

HDRMERGE_NOTE = """mergehdr's default merge path is based on the merging code of hdrmerge by
Wenzel Jakob (https://github.com/wjakob/hdrmerge), which is distributed under the
GNU General Public License, version 3. The license text follows.
"""

MERGEHDR_NOTE = """mergehdr (by Dong Hyun Kim) re-implements linearhdr and pylinearhdr by
Stephen Wasilewski (EPFL, https://github.com/stephanwaz/linearhdr) in C++. Like
linearhdr it is distributed under the GNU Lesser General Public License v3 and the
GNU General Public License v3; both texts follow. dht_demosaic_linearhdr.cpp keeps
its original notices (Anton Petrusevich, LibRaw; Stephen Wasilewski, EPFL).
"""

# (name, version, license, used in, [sources]); a source is one of
#   "path"                  file relative to the hdrspace source tree
#   "mergehdr:path"         file relative to the sibling mergehdr source tree
#   "brew:formula/file"     file in the Homebrew keg /opt/homebrew/opt/formula
#   "tail:path:marker"      text of a file from the line containing marker
#   "head:path:N"           first N lines of a file
#   "text:KEY"              text kept in this script
#   "as:LABEL:source"       any of the above, labelled as a generic license text
COMPONENTS = [
    ("hdrspace and mergehdr", "this build", "GPL-3.0 (mergehdr: LGPL-3.0 / GPL-3.0)", GUI + "; " + CLI,
     ["text:MERGEHDR_NOTE", "mergehdr:COPYING.LESSER", "mergehdr:COPYING"]),
    ("tev", "vendored, modified", "GPL-3.0", GUI, ["vendor/tev/LICENSE.txt"]),
    ("hdrmerge", "merge code in mergehdr", "GPL-3.0", CLI,
     ["text:HDRMERGE_NOTE", "as:GNU General Public License v3.0 (text):vendor/tev/LICENSE.txt"]),
    ("evalglare", "glare analysis (C++ re-implementation); original in Contents/Resources/radiance",
     "Evalglare Software License 3.0x", GUI + "; " + CLI + "; " + RES + "/radiance",
     ["text:EVALGLARE_NOTE", "assets/license-notices/evalglare-license.txt"]),
    ("Radiance", "Contents/Resources/radiance", "Radiance Software License 2.0", RES + "/radiance",
     ["assets/license-notices/radiance-license.txt"]),
    ("HDR-VDP 3", "3.0.7 (View Visibility port and spectral data)", "BSD-3-Clause", GUI + "; " + CLI + "; " + RES + "/hdrvdp3",
     ["assets/license-notices/hdrvdp3-license.txt", "assets/hdrvdp3-data/README.txt"]),
    ("matlabPyrTools (convolve.c, edges.c)", "vendored in vendor/hdrvdp-mex", "MIT", GUI + "; " + CLI,
     ["head:vendor/hdrvdp-mex/convolve.c:19", "text:PYRTOOLS_MIT"]),
    ("NLopt SLSQP", "vendored in mergehdr/third_party/nlopt_slsqp", "BSD-3-Clause", CLI,
     ["mergehdr:third_party/nlopt_slsqp/COPYRIGHT"]),
    ("NanoGUI", "vendored", "BSD-3-Clause", GUI, [f"{DEPS}/nanogui/LICENSE.txt"]),
    ("NanoVG", "vendored", "Zlib", GUI, [f"{DEPS}/nanogui/ext/nanovg/LICENSE.txt"]),
    ("MetalNanoVG", "vendored", "MIT", GUI, [f"{DEPS}/nanogui/ext/nanovg_metal/LICENSE"]),
    ("GLFW", "vendored", "Zlib", GUI, [f"{DEPS}/nanogui/ext/glfw/LICENSE.md"]),
    ("nativefiledialog-extended", "vendored", "Zlib", GUI, [f"{DEPS}/nanogui/ext/nativefiledialog-extended/LICENSE"]),
    ("clip", "vendored", "MIT", GUI, [f"{DEPS}/clip/LICENSE.txt"]),
    ("OpenEXR", "vendored; Frameworks 3.4.9", "BSD-3-Clause", GUI + "; " + CLI + "; " + FW, [f"{DEPS}/openexr/LICENSE.md"]),
    ("Imath", "vendored; Frameworks 3.2.2", "BSD-3-Clause", GUI + "; " + CLI + "; " + FW, [f"{DEPS}/Imath/LICENSE.md"]),
    ("LibRaw", "vendored; Frameworks libraw.24", "LGPL-2.1 or CDDL-1.0", GUI + "; " + CLI + "; " + FW,
     [f"{DEPS}/LibRaw/COPYRIGHT", f"{DEPS}/LibRaw/LICENSE.LGPL", f"{DEPS}/LibRaw/LICENSE.CDDL"]),
    ("Little-CMS", "vendored; Frameworks liblcms2.2", "MIT", GUI + "; " + FW, [f"{DEPS}/Little-CMS/LICENSE"]),
    ("libjpeg-turbo", "vendored; Frameworks libjpeg.8.3.2", "IJG AND BSD-3-Clause AND Zlib", GUI + "; " + CLI + "; " + FW,
     [f"{DEPS}/libjpeg-turbo/LICENSE.md", f"{DEPS}/libjpeg-turbo/README.ijg"]),
    ("libpng", "vendored", "libpng-2.0", GUI, [f"{DEPS}/libpng/LICENSE.md"]),
    ("zlib", "vendored", "Zlib", GUI, [f"{DEPS}/zlib/LICENSE"]),
    ("LibTIFF", "vendored", "libtiff", GUI, [f"{DEPS}/libtiff/LICENSE.md"]),
    ("libdeflate", "vendored; Frameworks libdeflate.0", "MIT", GUI + "; " + FW, [f"{DEPS}/libdeflate/COPYING"]),
    ("libwebp", "vendored", "BSD-3-Clause (with patent grant)", GUI, [f"{DEPS}/libwebp/COPYING", f"{DEPS}/libwebp/PATENTS"]),
    ("OpenJPEG", "vendored", "BSD-2-Clause", GUI, [f"{DEPS}/openjpeg/LICENSE"]),
    ("libjxl", "vendored", "BSD-3-Clause (with patent grant)", GUI, [f"{DEPS}/libjxl/LICENSE", f"{DEPS}/libjxl/PATENTS"]),
    ("Brotli", "vendored with libjxl; Frameworks 1.2.0", "MIT", GUI + "; " + FW, [f"{DEPS}/libjxl/third_party/brotli/LICENSE"]),
    ("Highway", "vendored with libjxl", "Apache-2.0 or BSD-3-Clause", GUI,
     [f"{DEPS}/libjxl/third_party/highway/LICENSE", f"{DEPS}/libjxl/third_party/highway/LICENSE-BSD3"]),
    ("libexif", "vendored", "LGPL-2.1", GUI, [f"{DEPS}/libexif/COPYING"]),
    ("Expat", "vendored", "MIT", GUI, [f"{DEPS}/libexpat/COPYING"]),
    ("XMP Toolkit SDK", "vendored", "BSD-3-Clause", GUI, [f"{DEPS}/xmp/LICENSE"]),
    ("args", "vendored", "MIT", GUI, [f"{DEPS}/args/LICENSE"]),
    ("qoi", "vendored", "MIT", GUI, [f"{DEPS}/qoi/LICENSE"]),
    ("small_vector", "vendored", "MIT", GUI, [f"{DEPS}/small_vector/LICENSE"]),
    ("stb_image and stb_image_write", "vendored", "MIT or Unlicense", GUI,
     [f"tail:{DEPS}/stb/stb_image_write.h:This software is available under 2 licenses"]),
    ("tinylogger", "vendored", "see text", GUI, [f"{DEPS}/tinylogger/LICENSE.md"]),
    ("concurrentqueue", "vendored", "BSD-2-Clause or BSL-1.0", GUI, [f"{DEPS}/concurrentqueue/LICENSE.md"]),
    ("UTF8-CPP", "vendored", "BSL-1.0", GUI, [f"{DEPS}/utfcpp/LICENSE"]),
    ("Eigen", "header-only (Homebrew)", "MPL-2.0", GUI + "; " + CLI, ["brew:eigen/COPYING.README", "brew:eigen/COPYING.MPL2"]),
    ("Roboto font", "embedded by NanoGUI", "Apache-2.0", GUI,
     [f"as:Apache License 2.0 (text):{DEPS}/libjxl/third_party/highway/LICENSE"]),
    ("Font Awesome Free 5 and Inconsolata fonts", "embedded by NanoGUI", "OFL-1.1", GUI,
     [f"{DEPS}/nanogui/ext/nanovg/example/LICENSE_OFL.txt"]),
    ("Boost (atomic, container, filesystem, program_options)", "Frameworks", "BSL-1.0", CLI + "; " + FW,
     [f"as:Boost Software License 1.0 (text):{DEPS}/utfcpp/LICENSE"]),
    ("Exiv2", "Frameworks 0.28.7", "GPL-2.0-or-later", CLI + "; " + FW, ["brew:exiv2/COPYING"]),
    ("FFTW", "Frameworks 3.3.10", "GPL-2.0-or-later", GUI + "; " + CLI + "; " + FW, ["brew:fftw/COPYRIGHT", "brew:fftw/COPYING"]),
    ("GCC OpenMP runtime (libgomp)", "Frameworks", "GPL-3.0 WITH GCC-exception-3.1", FW,
     ["brew:gcc/COPYING.RUNTIME", "brew:gcc/COPYING"]),
    ("GNU gettext runtime (libintl)", "Frameworks libintl.8", "LGPL-2.1-or-later", FW,
     [f"as:GNU Lesser General Public License v2.1 (text):{DEPS}/libexif/COPYING"]),
    ("inih and INIReader", "Frameworks", "BSD-3-Clause", FW, ["brew:inih/LICENSE.txt"]),
    ("LLVM OpenMP runtime (libomp)", "Frameworks", "Apache-2.0 WITH LLVM-exception", GUI + "; " + CLI + "; " + FW,
     ["brew:libomp/LICENSE.TXT"]),
    ("OpenJPH", "Frameworks 0.27.0", "BSD-2-Clause", FW, ["brew:openjph/LICENSE"]),
]
TEXTS = {"PYRTOOLS_MIT": PYRTOOLS_MIT, "EVALGLARE_NOTE": EVALGLARE_NOTE, "HDRMERGE_NOTE": HDRMERGE_NOTE,
         "MERGEHDR_NOTE": MERGEHDR_NOTE}
BREW = Path("/opt/homebrew/opt")


def slug(name: str) -> str:
    return re.sub(r"[^a-z0-9]+", "-", name.lower()).strip("-") + ".txt"


def resolve(source: str, root: Path) -> tuple[str, str]:
    """Return (label, text); labels never contain absolute user paths."""
    if source.startswith("as:"):
        _, label, inner = source.split(":", 2)
        return label, resolve(inner, root)[1]
    if source.startswith("text:"):
        return "hdrspace", TEXTS[source[5:]]
    if source.startswith("brew:"):
        formula, name = source[5:].split("/", 1)
        keg = (BREW / formula).resolve(strict=True)
        return f"Homebrew {formula} {keg.name}: {name}", (keg / name).read_text(encoding="utf-8", errors="replace")
    if source.startswith("mergehdr:"):
        path = source[len("mergehdr:"):]
        return f"mergehdr/{path}", (root.parent / "mergehdr" / path).read_text(encoding="utf-8", errors="replace")
    if source.startswith(("tail:", "head:")):
        kind, path, arg = source.split(":", 2)
        lines = (root / path).read_text(encoding="utf-8", errors="replace").splitlines()
        if kind == "head":
            return f"{path} (first {arg} lines)", "\n".join(lines[: int(arg)]) + "\n"
        start = next(i for i, line in enumerate(lines) if arg in line)
        body = [line for line in lines[start:] if line.strip() != "*/"]
        return f"{path} (license section)", "\n".join(body) + "\n"
    return source, (root / source).read_text(encoding="utf-8", errors="replace")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, help="Output folder (default: assets/licenses)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    out = (args.out or root / "assets/licenses").resolve()
    rows = []
    with tempfile.TemporaryDirectory(prefix=".licenses-", dir=out.parent) as scratch:
        stage = Path(scratch) / "licenses"
        stage.mkdir()
        for name, version, licence, used, sources in COMPONENTS:
            parts = []
            for source in sources:
                label, text = resolve(source, root)
                parts.append(f"--- {label} ---\n\n{text.rstrip()}\n")
            header = (f"{name}\nVersion: {version}\nLicense: {licence}\nUsed in hdrspace.app: {used}\n"
                      + "=" * 78 + "\n\n")
            file_name = slug(name)
            (stage / file_name).write_text(header + "\n".join(parts), encoding="utf-8")
            rows.append((name, version, licence, file_name))
        width = max(len(row[0]) for row in rows)
        index = [
            "hdrspace - third-party licenses",
            "================================",
            "",
            "hdrspace (c) 2026 Dong Hyun Kim is free software under the GNU General Public",
            "License v3. It contains or builds on the components listed below; each file",
            "in this folder holds the license text(s) of one component, taken from the",
            "sources that were compiled into the app, from the mergehdr sources, from",
            "assets/license-notices or, for the runtime libraries in Contents/Frameworks,",
            "from the matching Homebrew packages on the build machine.",
            "",
            "Required acknowledgements:",
            "  This product includes the evalglare software, developed at Fraunhofer ISE",
            "  and EPFL by J. Wienold.",
            "  This software uses a modified version of the source code of evalglare.",
            "  This product includes Radiance software (http://radsite.lbl.gov/) developed",
            "  by the Lawrence Berkeley National Laboratory (http://www.lbl.gov/).",
            "",
            "Copyleft components: hdrspace, tev and hdrmerge (GPL-3.0), mergehdr",
            "(LGPL-3.0 / GPL-3.0), Exiv2 and FFTW (GPL-2.0-or-later), LibRaw, libexif and",
            "libintl (LGPL-2.1), libgomp (GPL-3.0 with the GCC runtime exception).",
            "",
            "Not included: the AI models (SAM 3, Depth Anything 3) and the SpectralDB",
            "material list are downloaded on request and keep their own licenses.",
            "",
            "Regenerate with: python3 tools/collect_hdrspace_licenses.py",
            "",
        ]
        index += [f"{name.ljust(width)}  {licence}  [{version}]  -> {file_name}" for name, version, licence, file_name in rows]
        (stage / "README.txt").write_text("\n".join(index) + "\n", encoding="utf-8")
        if out.exists():
            shutil.rmtree(out)
        shutil.move(str(stage), str(out))
    print(f"Wrote {len(rows)} license files and README.txt to {out.name}/")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
