#!/usr/bin/env python3
"""Regenerate the code-native hdrspace PNG (fisheye mark) and macOS icon without extra packages."""
import argparse
import os
from pathlib import Path
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--work-dir", required=True, type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parent.parent
    work = args.work_dir.resolve()
    if work.is_relative_to(source):
        raise SystemExit("Use a temporary work directory outside the source tree.")
    work.mkdir(parents=True, exist_ok=True)
    assets = source / "assets/branding"
    png = assets / "hdrspace_icon_1024.png"
    env = dict(os.environ, CLANG_MODULE_CACHE_PATH=str(work / "swift-module-cache"))
    subprocess.run(["/usr/bin/swift", str(source / "tools/generate_hdrspace_icon.swift"), str(png)],
                   env=env, check=True)
    iconset = work / "hdrspace.iconset"
    iconset.mkdir(exist_ok=True)
    for logical in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            size = logical * scale
            suffix = "@2x" if scale == 2 else ""
            output = iconset / f"icon_{logical}x{logical}{suffix}.png"
            subprocess.run(["/usr/bin/sips", "-z", str(size), str(size), str(png), "--out", str(output)],
                           check=True, stdout=subprocess.DEVNULL)
    subprocess.run(["/usr/bin/iconutil", "-c", "icns", str(iconset), "-o", str(assets / "hdrspace.icns")],
                   check=True)
    print(f"Updated {png} and {assets / 'hdrspace.icns'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
