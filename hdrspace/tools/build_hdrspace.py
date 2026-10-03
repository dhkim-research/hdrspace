#!/usr/bin/env python3
"""Configure/build hdrspace in a separate workspace and stage its verified app."""
import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HOMEBREW = Path("/opt/homebrew")

# The rebuilt helpers load the libraries already shipped in the baseline payload
# (Contents/Frameworks). They are compiled against the headers of exactly those
# releases: the installed Homebrew keg whose machine code matches the shipped dylib
# is used, so inline header code (Imath half conversion, OpenEXR, LibRaw structures)
# is the same as in the validated baseline helpers.
#   (formula, shipped dylib in Frameworks, library inside the keg)
PINNED_LIBRARIES = (
    ("openexr", "libOpenEXR-3_*.dylib", "libOpenEXR.dylib"),
    ("imath", "libImath-*.dylib", "libImath.dylib"),
    ("jpeg-turbo", "libjpeg.*.dylib", "libjpeg.dylib"),
    ("little-cms2", "liblcms2.*.dylib", "liblcms2.dylib"),
    ("libomp", "libomp.dylib", "libomp.dylib"),
    ("exiv2", "libexiv2.*.dylib", "libexiv2.dylib"),
    ("boost", "libboost_filesystem.dylib", "libboost_filesystem.dylib"),
    ("fftw", "libfftw3.3.dylib", "libfftw3.dylib"),
    ("libraw", "libraw.*.dylib", "libraw.dylib"),
)
# Shipped LibRaw 0.22.0 (libraw.24); Homebrew has moved to an incompatible ABI, so the
# matching public headers are kept in the source tree.
VENDORED_LIBRAW = Path("vendor/libraw-0.22.0")


def text_digest(library: Path) -> str:
    """SHA-256 of the __TEXT,__text section: equal for the same build of a library
    even after its install names or signature were rewritten for bundling."""
    with tempfile.TemporaryDirectory() as scratch:
        section = Path(scratch) / "text.bin"
        subprocess.run(["segedit", str(library), "-extract", "__TEXT", "__text", str(section)],
                       check=True, capture_output=True)
        return hashlib.sha256(section.read_bytes()).hexdigest()


def shipped_library(frameworks: Path, pattern: str) -> Path:
    found = sorted(path for path in frameworks.glob(pattern) if not path.is_symlink())
    if len(found) != 1:
        raise SystemExit(f"Expected one {pattern} in {frameworks}, found {[p.name for p in found]}")
    return found[0]


def matching_keg(formula: str, keg_library: str, shipped: Path) -> Path | None:
    cellar = HOMEBREW / "Cellar" / formula
    if not cellar.is_dir():
        return None
    wanted = text_digest(shipped)
    for keg in sorted(cellar.iterdir(), reverse=True):
        candidate = keg / "lib" / keg_library
        if candidate.exists() and text_digest(candidate) == wanted:
            return keg
    return None


def libraw_pkgconfig(source: Path, work: Path, shipped: Path) -> tuple[Path, str]:
    """pkg-config entry that compiles against the vendored headers and links the
    shipped library itself (the packager later points it at Contents/Frameworks)."""
    headers = source / VENDORED_LIBRAW / "include"
    version_h = (headers / "libraw/libraw_version.h").read_text()
    field = lambda name: re.search(rf"#define {name} (\d+)", version_h).group(1)
    soname = re.fullmatch(r"libraw\.(\d+)\.dylib", shipped.name)
    if not soname or soname.group(1) != field("LIBRAW_SHLIB_CURRENT"):
        raise SystemExit(f"{VENDORED_LIBRAW} does not match the shipped {shipped.name}.")
    version = ".".join(field(f"LIBRAW_{part}_VERSION") for part in ("MAJOR", "MINOR", "PATCH"))
    root = work / "mergehdr-deps" / "libraw"
    lib = root / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    link = lib / "libraw.dylib"
    if link.is_symlink() or link.exists():
        link.unlink()
    link.symlink_to(shipped)
    pkgconfig = root / "pkgconfig"
    pkgconfig.mkdir(exist_ok=True)
    (pkgconfig / "libraw.pc").write_text(
        f"prefix={headers.parent}\n"
        "includedir=${prefix}/include\n"
        f"libdir={lib}\n\n"
        "Name: libraw\n"
        f"Description: LibRaw {version} headers for the shipped {shipped.name}\n"
        "Requires: lcms2\n"
        f"Version: {version}\n"
        "Libs: -L${libdir} -lraw -lstdc++ -Xpreprocessor -fopenmp\n"
        "Libs.private: -ljpeg\n"
        "Cflags: -I${includedir}/libraw -I${includedir}\n")
    return pkgconfig, f"LibRaw {version} headers from {VENDORED_LIBRAW}, linked to {shipped.name}"


def pinned_dependencies(source: Path, work: Path, baseline_app: Path) -> tuple[list[Path], list[Path]]:
    frameworks = baseline_app / "Contents/Frameworks"
    prefixes: list[Path] = []
    pkgconfig: list[Path] = []
    for formula, pattern, keg_library in PINNED_LIBRARIES:
        shipped = shipped_library(frameworks, pattern)
        keg = matching_keg(formula, keg_library, shipped)
        if keg:
            prefixes.append(keg)
            if (keg / "lib/pkgconfig").is_dir():
                pkgconfig.append(keg / "lib/pkgconfig")
            print(f"mergehdr dependency {formula}: Homebrew {keg.name} (matches {shipped.name})")
        elif formula == "libraw" and (source / VENDORED_LIBRAW).is_dir():
            directory, note = libraw_pkgconfig(source, work, shipped)
            pkgconfig.insert(0, directory)
            print(f"mergehdr dependency {formula}: {note}")
        else:
            raise SystemExit(
                f"No installed Homebrew {formula} matches the shipped {shipped.name}. Install that "
                "release, or refresh the Frameworks payload and validate the helpers again.")
    eigen = (HOMEBREW / "opt/eigen/include/eigen3/Eigen/Version")
    if eigen.exists():
        parts = dict(re.findall(r"#define EIGEN_(\w+)_VERSION (\d+)", eigen.read_text()))
        print("mergehdr dependency eigen (header-only): "
              + ".".join(parts.get(key, "?") for key in ("MAJOR", "MINOR", "PATCH")))
    return prefixes, pkgconfig


def build_mergehdr(source: Path, work: Path, baseline_app: Path, cmake: str, ninja: str,
                   jobs: int, configure_only: bool) -> Path:
    """Build the mergehdr/mergehdrcore helpers that hdrspace ships.

    Dependencies are pinned to the releases shipped in the baseline Frameworks (see
    PINNED_LIBRARIES); the packager then binds the helpers to those dylibs. An active
    conda environment must not shadow the Homebrew packages, so its prefix is ignored
    and pkg-config reads only the pinned kegs and Homebrew."""
    mergehdr = source.parent / "mergehdr"
    build = work / "mergehdr-build"
    prefixes, pkgconfig_dirs = pinned_dependencies(source, work, baseline_app)
    env = dict(os.environ)
    pkgconfig = ":".join([str(path) for path in pkgconfig_dirs]
                         + [f"{HOMEBREW}/lib/pkgconfig", f"{HOMEBREW}/share/pkgconfig"])
    env["PKG_CONFIG_PATH"] = pkgconfig
    env["PKG_CONFIG_LIBDIR"] = pkgconfig
    configure = [
        cmake, "-S", str(mergehdr), "-B", str(build), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=26.0",
        "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_MAKE_PROGRAM=" + ninja,
        "-DCMAKE_PREFIX_PATH=" + ";".join([str(path) for path in prefixes] + [str(HOMEBREW)]),
        f"-DMERGEHDR_HOMEBREW_PREFIX={HOMEBREW}",
        f"-DEigen3_DIR={HOMEBREW}/opt/eigen/share/eigen3/cmake",
        "-DMERGEHDR_MEX_DIR=" + str(source / "vendor/hdrvdp-mex"),
    ]
    if (HOMEBREW / "bin/pkg-config").exists():
        configure.append(f"-DPKG_CONFIG_EXECUTABLE={HOMEBREW}/bin/pkg-config")
    if env.get("CONDA_PREFIX"):
        configure.append("-DCMAKE_IGNORE_PREFIX_PATH=" + env["CONDA_PREFIX"])
    subprocess.run(configure, check=True, env=env)
    if not configure_only:
        subprocess.run([cmake, "--build", str(build), "--target", "mergehdr", "mergehdrcore",
                        "--parallel", str(jobs)], check=True, env=env)
    return build


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", type=Path, required=True)
    # The validated baseline app bundle (runtime payload), kept next to the source tree.
    parser.add_argument("--baseline-app", type=Path,
                        default=Path(__file__).resolve().parents[2] / "hdrspace-baseline" / "payload")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--keep-baseline-helpers", action="store_true",
                        help="Ship the baseline mergehdr/mergehdrcore instead of rebuilding them from ../mergehdr")
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    work = args.work_dir.resolve()
    if work.is_relative_to(source) or "Applications" in work.parts or work == Path("/"):
        raise SystemExit("Choose a temporary build workspace outside source and Applications directories.")
    if args.jobs < 1:
        raise SystemExit("--jobs must be positive.")
    cmake = shutil.which("cmake")
    ninja = shutil.which("ninja")
    if not cmake or not ninja:
        raise SystemExit("Existing CMake and Ninja executables are required; no packages are installed automatically.")
    configure = [
        cmake, "-S", str(source), "-B", str(work / "build"), "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_OSX_ARCHITECTURES=arm64",
        "-DCMAKE_OSX_DEPLOYMENT_TARGET=13.3",
        "-DCMAKE_C_COMPILER=/usr/bin/clang", "-DCMAKE_CXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_OBJC_COMPILER=/usr/bin/clang", "-DCMAKE_OBJCXX_COMPILER=/usr/bin/clang++",
        "-DCMAKE_MAKE_PROGRAM=" + ninja,
        "-DEigen3_DIR=/opt/homebrew/opt/eigen/share/eigen3/cmake",
        "-DHDRSPACE_BASELINE_APP=" + str(args.baseline_app.resolve()),
        "-DHDRSPACE_STAGE_DIR=" + str(work / "app"),
        "-DHDRSPACE_MERGEHDR_SOURCE_DIR=" + str(source.parent / "mergehdr"),
        "-DHDRVDP_MEX_DIR=" + str(source / "vendor/hdrvdp-mex"),
        "-DHDRSPACE_HDRVDP3_DATA_DIR=" + str(source / "assets/hdrvdp3-data"),
        "-DHDRSPACE_ENABLE_OPENMP=ON",
    ]
    if not args.keep_baseline_helpers:
        helpers = build_mergehdr(source, work, args.baseline_app.resolve(), cmake, ninja, args.jobs,
                                 args.configure_only)
        configure.append("-DHDRSPACE_MERGEHDR_BUILD_DIR=" + str(helpers))
    else:
        configure.append("-DHDRSPACE_MERGEHDR_BUILD_DIR=")
    subprocess.run(configure, check=True)
    if not args.configure_only:
        subprocess.run([cmake, "--build", str(work / "build"), "--target", "hdrspace", "--parallel", str(args.jobs)], check=True)
        print(work / "app/hdrspace.app")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
