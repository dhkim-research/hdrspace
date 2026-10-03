#!/usr/bin/env python3

import argparse
import os
import shutil
import subprocess
from pathlib import Path


SYSTEM_PREFIXES = ("/System/Library/", "/usr/lib/")


def run(*cmd: str, check: bool = True) -> str:
    result = subprocess.run(cmd, check=check, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return result.stdout


def list_deps(binary: Path) -> list[str]:
    out = run("otool", "-L", str(binary))
    refs: list[str] = []
    for line in out.splitlines()[1:]:
        stripped = line.strip()
        if not stripped:
            continue
        refs.append(stripped.split(" (compatibility version", 1)[0])
    return refs


def list_rpaths(binary: Path) -> list[str]:
    out = run("otool", "-l", str(binary))
    rpaths: list[str] = []
    lines = out.splitlines()
    for index, line in enumerate(lines):
        if "cmd LC_RPATH" not in line:
            continue
        for probe in range(index + 1, min(index + 8, len(lines))):
            candidate = lines[probe].strip()
            if candidate.startswith("path "):
                rpaths.append(candidate.split("path ", 1)[1].split(" (offset", 1)[0].strip())
                break
    return rpaths


def binary_archs(path: Path) -> set[str]:
    out = run("lipo", "-archs", str(path), check=False).strip()
    if not out:
        return set()
    return set(out.split())


def ensure_arch(path: Path, required_arch: str | None) -> None:
    if not required_arch:
        return
    archs = binary_archs(path)
    if required_arch not in archs:
        arch_label = ", ".join(sorted(archs)) if archs else "unknown"
        raise RuntimeError(f"{path} is {arch_label}, expected {required_arch}")


def expand_special(current_file: Path, app_macos_dir: Path, ref: str) -> list[Path]:
    if ref.startswith("@loader_path/"):
        suffix = ref[len("@loader_path/") :]
        return [(current_file.parent / suffix).resolve()]
    if ref.startswith("@executable_path/"):
        suffix = ref[len("@executable_path/") :]
        return [(app_macos_dir / suffix).resolve()]
    if ref.startswith("@rpath/"):
        suffix = ref[len("@rpath/") :]
        matches: list[Path] = []
        for rpath in list_rpaths(current_file):
            for root in expand_special(current_file, app_macos_dir, rpath):
                candidate = (root / suffix).resolve()
                if candidate.exists():
                    matches.append(candidate)
        return matches
    path = Path(ref)
    if path.exists():
        return [path.resolve()]
    return []


def should_bundle(dep: Path, app_bundle: Path) -> bool:
    dep_str = str(dep)
    if any(dep_str.startswith(prefix) for prefix in SYSTEM_PREFIXES):
        return False
    try:
        dep.relative_to(app_bundle)
        return False
    except ValueError:
        return dep.exists()


def install_name(path: Path) -> str | None:
    out = run("otool", "-D", str(path), check=False)
    lines = [line.strip() for line in out.splitlines() if line.strip()]
    if len(lines) >= 2:
        return lines[1]
    return None


def maybe_change(binary: Path, old_ref: str, new_ref: str) -> None:
    refs = list_deps(binary)
    if old_ref in refs:
        subprocess.run(["install_name_tool", "-change", old_ref, new_ref, str(binary)], check=True)


def bundle_tools(app: Path, tools: list[Path], profiles_dir: Path | None, required_arch: str | None) -> None:
    contents = app / "Contents"
    macos_dir = contents / "MacOS"
    frameworks_dir = contents / "Frameworks"
    frameworks_dir.mkdir(parents=True, exist_ok=True)

    if profiles_dir and profiles_dir.exists():
        bundled_profiles = contents / "profiles"
        if bundled_profiles.exists():
            shutil.rmtree(bundled_profiles)
        shutil.copytree(profiles_dir, bundled_profiles)
        ds_store = bundled_profiles / ".DS_Store"
        if ds_store.exists():
            ds_store.unlink()

    original_for_copy: dict[Path, Path] = {}
    bundled_ref_for_source: dict[Path, str] = {}
    queue: list[Path] = []
    tools_in_bundle: list[Path] = []

    def bundled_ref_for(dest: Path) -> str:
        return f"@executable_path/../Frameworks/{dest.name}"

    def bundled_id_for(dest: Path) -> str:
        return f"@rpath/{dest.name}"

    def ensure_bundled_library(source: Path) -> Path:
        source = source.resolve()
        ensure_arch(source, required_arch)
        dest = frameworks_dir / source.name
        if not dest.exists():
            shutil.copy2(source, dest)
            os.chmod(dest, 0o755)
        original_for_copy.setdefault(dest, source)
        bundled_ref_for_source[source] = bundled_ref_for(dest)
        if dest not in queue:
            queue.append(dest)
        return dest

    for tool in tools:
        tool = tool.resolve()
        ensure_arch(tool, required_arch)
        dest = macos_dir / tool.name
        shutil.copy2(tool, dest)
        os.chmod(dest, 0o755)
        original_for_copy[dest] = tool
        tools_in_bundle.append(dest)
        if dest not in queue:
            queue.append(dest)

    processed: set[Path] = set()
    while queue:
        copied = queue.pop(0)
        if copied in processed:
            continue
        processed.add(copied)

        source_for_resolution = original_for_copy[copied]
        for ref in list_deps(copied):
            if any(ref.startswith(prefix) for prefix in SYSTEM_PREFIXES):
                continue
            resolved_candidates = expand_special(source_for_resolution, macos_dir, ref)
            if not resolved_candidates:
                continue
            resolved = resolved_candidates[0]
            if should_bundle(resolved, app):
                ensure_bundled_library(resolved)

    all_targets = list(dict.fromkeys(tools_in_bundle + sorted(original_for_copy)))
    for copied in all_targets:
        source_for_resolution = original_for_copy[copied]
        for ref in list_deps(copied):
            if any(ref.startswith(prefix) for prefix in SYSTEM_PREFIXES):
                continue
            resolved_candidates = expand_special(source_for_resolution, macos_dir, ref)
            if not resolved_candidates:
                continue
            resolved = resolved_candidates[0]
            new_ref = bundled_ref_for_source.get(resolved.resolve())
            if new_ref:
                maybe_change(copied, ref, new_ref)

    for bundled, source in sorted(original_for_copy.items()):
        if bundled.parent != frameworks_dir:
            continue
        target_id = bundled_id_for(bundled)
        current_id = install_name(bundled)
        if current_id != target_id:
            subprocess.run(["install_name_tool", "-id", target_id, str(bundled)], check=True)
        source_id = install_name(source)
        if source_id and source_id != target_id:
            maybe_change(bundled, source_id, target_id)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", required=True, type=Path)
    parser.add_argument("--tool", action="append", default=[], type=Path)
    parser.add_argument("--profiles", type=Path)
    parser.add_argument("--require-arch", default="")
    args = parser.parse_args()

    if not args.app.exists():
        raise SystemExit(f"App bundle not found: {args.app}")
    if not args.tool:
        raise SystemExit("At least one --tool is required.")

    bundle_tools(
        args.app.resolve(),
        [tool.resolve() for tool in args.tool],
        args.profiles.resolve() if args.profiles else None,
        args.require_arch.strip() or None,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
