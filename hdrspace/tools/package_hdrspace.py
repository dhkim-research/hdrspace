#!/usr/bin/env python3
"""Stage hdrspace on the hash-verified baseline runtime payload.

Everything from the baseline app is kept byte for byte except these deliberate,
reported changes: the main executable, the mergehdr/mergehdrcore helpers when a
rebuilt pair is supplied (--mergehdr-dir), duplicate backup files that are
byte-identical to a file they shadow (DUPLICATE_BACKUPS), the baseline's own
icon and branding images (unused by hdrspace) and the two DA3 Python helpers, which
are taken from mergehdr/tools (AI_HELPERS). License texts are added under
Contents/Resources/licenses."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import plistlib
import re
import shutil
import subprocess
import tempfile


APP_NAME = "hdrspace"
BUNDLE_ID = "com.hdrspace.app"
SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")
HELPERS = ("mergehdr", "mergehdrcore")
# A leftover copy that is byte-identical to the file it backs up; dropped from the
# bundle only after the hashes are confirmed equal.
# The baseline's own icon and branding images; hdrspace ships its own icon and mark.
REQUIRED_BASELINE_PAYLOAD = ("Contents/Frameworks", "Contents/Resources/radiance/bin",
                             "Contents/Resources/hdrvdp3/data", "Contents/Resources/ai")
# Python helpers kept in sync with mergehdr/tools (same code, hdrspace paths and names).
AI_HELPERS = ("da3_vos_helper.py", "install_da3_mps.py")
DUPLICATE_BACKUPS = {
    "Contents/Resources/radiance/bin/rtrace.pre_progress_20260327": "Contents/Resources/radiance/bin/rtrace",
}


def run(*args: str) -> str:
    return subprocess.run(args, check=True, text=True, capture_output=True).stdout


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def inventory(root: Path, excluded: tuple[str, ...] = ()) -> dict[str, str]:
    result = {}
    for path in sorted(root.rglob("*")):
        relative = path.relative_to(root).as_posix()
        if any(relative == item or relative.startswith(item + "/") for item in excluded):
            continue
        if path.is_symlink():
            result[relative] = "symlink:" + os.readlink(path)
        elif path.is_file():
            result[relative] = digest(path)
    return result


def ensure_preserved(app: Path, expected: dict[str, str]) -> None:
    actual = inventory(app)
    mismatches = [name for name, value in expected.items() if actual.get(name) != value]
    if mismatches:
        raise RuntimeError("Baseline payload changed: " + ", ".join(mismatches[:8]))


def dependencies(executable: Path) -> list[str]:
    return [line.strip().split(" (compatibility version", 1)[0]
            for line in run("/usr/bin/otool", "-L", str(executable)).splitlines()[1:]
            if line.strip()]


def fix_main_dependencies(executable: Path, frameworks: Path) -> None:
    for dependency in dependencies(executable):
        if dependency.startswith(SYSTEM_PREFIXES):
            continue
        library = frameworks / Path(dependency).name
        if not library.is_file():
            raise RuntimeError(f"New GUI requires a library absent from the baseline: {dependency}")
        replacement = "@executable_path/../Frameworks/" + library.name
        if dependency != replacement:
            run("/usr/bin/install_name_tool", "-change", dependency, replacement, str(executable))
    for dependency in dependencies(executable):
        if not dependency.startswith(SYSTEM_PREFIXES + ("@executable_path/../Frameworks/",)):
            raise RuntimeError(f"Unresolved main executable dependency: {dependency}")


def library_family(name: str) -> str:
    """libOpenEXR-3_4.33.dylib and libOpenEXR-3_4.33.3.4.9.dylib are one family."""
    match = re.match(r"^(lib[^.]+)", name)
    return match.group(1) if match else name


def rpaths(binary: Path) -> list[str]:
    lines = run("/usr/bin/otool", "-l", str(binary)).splitlines()
    found = []
    for index, line in enumerate(lines):
        if "cmd LC_RPATH" in line:
            for probe in lines[index + 1:index + 6]:
                probe = probe.strip()
                if probe.startswith("path "):
                    found.append(probe.split("path ", 1)[1].split(" (offset", 1)[0].strip())
                    break
    return found


def linked_versions(binary: Path) -> dict[str, str]:
    """Each dependency of a binary with the compatibility version it was linked against.
    For a dylib the first entry is the library itself (its LC_ID_DYLIB)."""
    found = {}
    for line in run("/usr/bin/otool", "-L", str(binary)).splitlines()[1:]:
        match = re.match(r"\s*(\S.*?) \(compatibility version ([0-9.]+), current version", line)
        if match:
            found.setdefault(match.group(1), match.group(2))
    return found


def install_rebuilt_helper(source: Path, target: Path, frameworks: Path) -> dict[str, str]:
    """Copy a rebuilt helper and bind it to the baseline Frameworks dylibs.

    The helper must have been compiled against headers of the releases shipped in
    the baseline payload (tools/build_hdrspace.py pins them); a dependency whose
    compatibility version differs from the shipped library is refused rather than
    silently rebound. Build-machine rpaths are removed, the binary is ad-hoc signed
    again after its load commands change, and it must start with its bundled
    libraries."""
    require_arm64(source)
    shutil.copy2(source, target)
    os.chmod(target, 0o755)
    available = {library_family(item.name): item for item in frameworks.glob("*.dylib")}
    mapping = {}
    for dependency, compatibility in linked_versions(target).items():
        if dependency.startswith(SYSTEM_PREFIXES):
            continue
        family = library_family(Path(dependency).name)
        if family not in available:
            raise RuntimeError(f"Rebuilt {target.name} needs a library absent from the baseline: {dependency}")
        shipped = available[family]
        shipped_compatibility = next(iter(linked_versions(shipped).values()), "")
        if compatibility != shipped_compatibility:
            raise RuntimeError(
                f"Rebuilt {target.name} was linked against {dependency} (compatibility {compatibility}), "
                f"but the shipped {shipped.name} is {shipped_compatibility}; build it against that release.")
        replacement = "@executable_path/../Frameworks/" + shipped.name
        if dependency != replacement:
            run("/usr/bin/install_name_tool", "-change", dependency, replacement, str(target))
        mapping[dependency] = replacement
    for path in rpaths(target):
        run("/usr/bin/install_name_tool", "-delete_rpath", path, str(target))
    for dependency in dependencies(target):
        if not dependency.startswith(SYSTEM_PREFIXES + ("@executable_path/../Frameworks/",)):
            raise RuntimeError(f"Unresolved {target.name} dependency: {dependency}")
    run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none", str(target))
    started = subprocess.run([str(target), "--help"], capture_output=True, text=True, timeout=120)
    if started.returncode < 0 or "dyld" in started.stderr:
        raise RuntimeError(f"Rebuilt {target.name} does not start: {started.stderr.strip()[:400]}")
    return mapping


def require_arm64(path: Path) -> None:
    if "arm64" not in run("/usr/bin/lipo", "-archs", str(path)).split():
        raise RuntimeError(f"Expected an Apple Silicon binary: {path}")


def assert_staging_path(stage: Path, baseline: Path, built: Path) -> None:
    if stage == Path("/") or "Applications" in stage.parts:
        raise RuntimeError("Packaging only stages an app; choose a temporary directory outside Applications.")
    for protected in (baseline, built):
        if stage == protected or stage.is_relative_to(protected) or protected.is_relative_to(stage):
            raise RuntimeError(f"Staging directory overlaps a protected app: {protected}")
    if any(part.endswith(".app") for part in stage.parts):
        raise RuntimeError("--stage-dir must be a directory outside any existing app bundle.")


def package(args: argparse.Namespace) -> Path:
    baseline = args.baseline_app.resolve(strict=True)
    built = args.built_app.resolve(strict=True)
    stage = args.stage_dir.resolve()
    assert_staging_path(stage, baseline, built)
    baseline_info = plistlib.loads((baseline / "Contents/Info.plist").read_bytes())
    built_info = plistlib.loads((built / "Contents/Info.plist").read_bytes())
    missing = [item for item in REQUIRED_BASELINE_PAYLOAD if not (baseline / item).is_dir()]
    if missing or baseline_info.get("CFBundleIdentifier") == BUNDLE_ID:
        raise RuntimeError("Expected the validated baseline app payload (missing: "
                           + ", ".join(missing) + ").")
    if built_info.get("CFBundleIdentifier") != BUNDLE_ID:
        raise RuntimeError("The build artifact does not have the hdrspace bundle identifier.")
    old_executable_name = baseline_info["CFBundleExecutable"]
    executable_name = built_info["CFBundleExecutable"]
    if executable_name != APP_NAME:
        raise RuntimeError("Expected the newly built executable to be named hdrspace.")
    executable_source = built / "Contents/MacOS" / executable_name
    require_arm64(executable_source)
    for name in HELPERS:
        require_arm64(baseline / "Contents/MacOS" / name)
    rebuilt_dir = args.mergehdr_dir.resolve(strict=True) if args.mergehdr_dir else None
    licenses_dir = args.licenses.resolve(strict=True)
    if not (licenses_dir / "README.txt").is_file():
        raise RuntimeError("The license folder is incomplete; run tools/collect_hdrspace_licenses.py first.")

    # Snapshot the complete source app as well as the payload that must survive
    # replacement of its launcher, top-level signature and bundle identity.
    baseline_before = inventory(baseline)
    removed = []
    for backup, original in DUPLICATE_BACKUPS.items():
        backup_path = baseline / backup
        if backup_path.exists():
            if digest(backup_path) != digest(baseline / original):
                raise RuntimeError(f"{backup} differs from {original}; it is not a duplicate, keep it.")
            removed.append(backup)
    resources = baseline / "Contents/Resources"
    old_branding = sorted(
        [path.relative_to(baseline).as_posix() for path in resources.glob("*.icns") if path.is_file()]
        + [path.relative_to(baseline).as_posix() for path in (resources / "branding").rglob("*") if path.is_file()])
    ai_tools = args.ai_tools_dir.resolve(strict=True)
    for name in AI_HELPERS:
        if not (ai_tools / name).is_file():
            raise RuntimeError(f"Missing AI helper {ai_tools / name}")
    exclusions = ["Contents/Info.plist", "Contents/_CodeSignature",
                  "Contents/MacOS/" + old_executable_name] + removed + old_branding + [
                  "Contents/Resources/ai/" + name for name in AI_HELPERS]
    if rebuilt_dir:
        exclusions += ["Contents/MacOS/" + name for name in HELPERS]
    protected = inventory(baseline, tuple(exclusions))
    local_data = inventory(args.hdrvdp_data.resolve(strict=True))
    baseline_data = inventory(baseline / "Contents/Resources/hdrvdp3/data")
    if local_data != baseline_data:
        raise RuntimeError("The local HDR-VDP data snapshot differs from the validated baseline.")

    stage.mkdir(parents=True, exist_ok=True)
    final_app = stage / (APP_NAME + ".app")
    if final_app.exists():
        previous_info = plistlib.loads((final_app / "Contents/Info.plist").read_bytes())
        if previous_info.get("CFBundleIdentifier") != BUNDLE_ID:
            raise RuntimeError("Refusing to replace a staged app with an unrelated identity.")

    # Assemble in a sibling staging folder. An interrupted or failed build keeps
    # the previous staged app available and never writes into the baseline app.
    with tempfile.TemporaryDirectory(prefix=".hdrspace-package-", dir=stage) as scratch:
        app = Path(scratch) / (APP_NAME + ".app")
        shutil.copytree(baseline, app, symlinks=True, copy_function=shutil.copy2)
        old_executable = app / "Contents/MacOS" / old_executable_name
        old_executable.unlink()
        executable = app / "Contents/MacOS" / APP_NAME
        shutil.copy2(executable_source, executable)
        os.chmod(executable, 0o755)
        fix_main_dependencies(executable, app / "Contents/Frameworks")
        helper_report = {}
        if rebuilt_dir:
            for name in HELPERS:
                target = app / "Contents/MacOS" / name
                helper_report[name] = {
                    "dependencies": install_rebuilt_helper(rebuilt_dir / name, target, app / "Contents/Frameworks"),
                    "sha256": digest(target),
                }
        for backup in removed:
            (app / backup).unlink()
        for relative in old_branding:
            (app / relative).unlink()
        ai_report = {}
        for name in AI_HELPERS:
            target = app / "Contents/Resources/ai" / name
            shutil.copy2(ai_tools / name, target)
            ai_report[name] = digest(target)
        licenses_target = app / "Contents/Resources/licenses"
        if licenses_target.exists():
            shutil.rmtree(licenses_target)
        shutil.copytree(licenses_dir, licenses_target)

        info = dict(baseline_info)
        info.update({
            "CFBundleExecutable": APP_NAME,
            "CFBundleName": APP_NAME,
            "CFBundleDisplayName": APP_NAME,
            "CFBundleIdentifier": BUNDLE_ID,
            "CFBundleIconFile": "hdrspace.icns",
            "CFBundleVersion": built_info["CFBundleVersion"],
            "CFBundleShortVersionString": built_info["CFBundleShortVersionString"],
            "LSMinimumSystemVersion": "13.3",
            # Offered under Finder "Open With" for HDR images; never claims to be the default app.
            "CFBundleDocumentTypes": [
                {
                    "CFBundleTypeName": "HDR image",
                    "CFBundleTypeRole": "Viewer",
                    "LSHandlerRank": "Alternate",
                    "LSItemContentTypes": ["public.radiance", "com.ilm.openexr-image"],
                },
                {
                    "CFBundleTypeName": "HDR image file",
                    "CFBundleTypeRole": "Viewer",
                    "LSHandlerRank": "Alternate",
                    "CFBundleTypeExtensions": ["hdr", "pic", "exr", "pfm"],
                },
            ],
        })
        (app / "Contents/Info.plist").write_bytes(plistlib.dumps(info, sort_keys=True))
        resources = app / "Contents/Resources"
        shutil.copy2(args.icon, resources / "hdrspace.icns")
        shutil.copy2(args.brand_png, resources / "branding/hdrspace_icon_1024.png")
        shutil.copy2(args.brand_svg, resources / "branding/hdrspace.svg")
        ensure_preserved(app, protected)

        signature = app / "Contents/_CodeSignature"
        if signature.exists():
            shutil.rmtree(signature)
        # Do not use --deep here: re-signing nested tools/libraries would change
        # their bytes and invalidate the deliberate runtime-payload preservation.
        run("/usr/bin/xattr", "-cr", str(app))
        run("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none",
            "--identifier", BUNDLE_ID, str(app))
        run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
        ensure_preserved(app, protected)
        if inventory(baseline) != baseline_before:
            raise RuntimeError("The baseline app changed during packaging; do not deploy this build.")

        report = {
            "app": str(final_app),
            "bundle_identifier": BUNDLE_ID,
            "baseline_app": str(baseline),
            "protected_file_count": len(protected),
            "protected_files": protected,
            "baseline_unchanged": True,
            "helper_and_resource_bytes_preserved": not rebuilt_dir,
            "rebuilt_helpers": helper_report,
            "removed_duplicate_backups": removed,
            "removed_baseline_branding": old_branding,
            "ai_helpers_from_mergehdr_tools": ai_report,
            "license_files": sorted(item.name for item in licenses_target.iterdir()),
            "codesign_verify_deep_strict": "passed",
            "main_executable_sha256": digest(executable),
        }
        if final_app.exists():
            shutil.rmtree(final_app)
        shutil.move(str(app), str(final_app))
        (stage / "hdrspace-package-verification.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n")
    return final_app


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--built-app", required=True, type=Path)
    parser.add_argument("--baseline-app", required=True, type=Path)
    parser.add_argument("--stage-dir", required=True, type=Path)
    parser.add_argument("--icon", required=True, type=Path)
    parser.add_argument("--brand-png", required=True, type=Path)
    parser.add_argument("--brand-svg", required=True, type=Path)
    parser.add_argument("--hdrvdp-data", required=True, type=Path)
    parser.add_argument("--licenses", required=True, type=Path)
    parser.add_argument("--ai-tools-dir", type=Path,
                        default=Path(__file__).resolve().parents[2] / "mergehdr" / "tools",
                        help="Folder with da3_vos_helper.py and install_da3_mps.py to ship")
    parser.add_argument("--mergehdr-dir", type=Path,
                        help="Folder with rebuilt mergehdr and mergehdrcore to ship instead of the baseline pair")
    args = parser.parse_args()
    app = package(args)
    kept = "libraries, resources and profiles" if args.mergehdr_dir else "helpers, libraries, resources and profiles"
    print(f"Staged {app}; baseline {kept} verified unchanged.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
