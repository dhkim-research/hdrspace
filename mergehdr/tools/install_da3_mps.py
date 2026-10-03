#!/usr/bin/env python3
"""Install hdrspace's isolated Depth Anything 3 Apple-Silicon runtime."""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path


AWEDELON_COMMIT = "4a2a98992837a03511e76eb446cb4bc97801ff6b"
PACKAGE_URL = (
    "awesome-depth-anything-3 @ "
    f"git+https://github.com/Aedelon/awesome-depth-anything-3.git@{AWEDELON_COMMIT}"
)
MODEL_ID = "depth-anything/DA3METRIC-LARGE"


def parse_args() -> argparse.Namespace:
    default_support = Path.home() / "Library" / "Application Support" / "hdrspace"
    parser = argparse.ArgumentParser(description="Install DA3Metric-Large with the MPS-enabled runtime.")
    parser.add_argument("--runtime-root", default=str(default_support / "da3-runtime"))
    parser.add_argument("--hf-home", default=str(default_support / "da3-hf-home"))
    parser.add_argument("--helper", default=str(Path(__file__).with_name("da3_vos_helper.py")))
    parser.add_argument("--python", help="Base Python 3.10-3.13 used to create the isolated venv")
    parser.add_argument("--skip-model", action="store_true", help="Install runtime without downloading model weights")
    parser.add_argument("--force-reinstall", action="store_true")
    parser.add_argument("--helper-only", action="store_true", help="Update only da3_vos_helper.py in an existing runtime")
    return parser.parse_args()


def run(command: list[str], env: dict[str, str] | None = None) -> None:
    print("+ " + " ".join(command), flush=True)
    subprocess.run(command, check=True, env=env)


def copy_runtime_helper(source: Path, destination: Path) -> None:
    """Install the helper without failing when source already is destination."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    try:
        same_file = source.samefile(destination)
    except FileNotFoundError:
        same_file = False
    if not same_file:
        shutil.copy2(source, destination)
    destination.chmod(0o755)


def patch_lazy_export_imports(venv_python: Path) -> Path:
    """Avoid loading pycolmap/open3d and a second OpenMP runtime during inference-only use."""
    site_packages_text = subprocess.check_output(
        [str(venv_python), "-c", "import site; print(site.getsitepackages()[0])"],
        text=True,
    ).strip()
    export_init = Path(site_packages_text) / "depth_anything_3" / "utils" / "export" / "__init__.py"
    if not export_init.exists():
        raise RuntimeError(f"Depth Anything 3 export module was not found: {export_init}")
    export_init.write_text(
        '"""Lazy exporters for the hdrspace inference-only MPS runtime."""\n\n'
        'def export(prediction, export_format, export_dir, **kwargs):\n'
        '    if "-" in export_format:\n'
        '        for item in export_format.split("-"):\n'
        '            export(prediction, item, export_dir, **kwargs)\n'
        '        return\n'
        '    if export_format == "mini_npz":\n'
        '        from .npz import export_to_mini_npz as function\n'
        '    elif export_format == "npz":\n'
        '        from .npz import export_to_npz as function\n'
        '    elif export_format == "depth_vis":\n'
        '        from .depth_vis import export_to_depth_vis as function\n'
        '    elif export_format == "feat_vis":\n'
        '        from .feat_vis import export_to_feat_vis as function\n'
        '    elif export_format == "glb":\n'
        '        from .glb import export_to_glb as function\n'
        '    elif export_format == "colmap":\n'
        '        from .colmap import export_to_colmap as function\n'
        '    elif export_format == "gs_ply":\n'
        '        from .gs import export_to_gs_ply as function\n'
        '    elif export_format == "gs_video":\n'
        '        from .gs import export_to_gs_video as function\n'
        '    else:\n'
        '        raise ValueError(f"Unsupported export format: {export_format}")\n'
        '    function(prediction, export_dir, **kwargs.get(export_format, {}))\n\n'
        '__all__ = ["export"]\n',
        encoding="utf-8",
    )
    return export_init


def python_version(executable: str) -> tuple[int, int] | None:
    try:
        output = subprocess.check_output(
            [executable, "-c", "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')"],
            text=True,
        ).strip()
        major, minor = output.split(".", 1)
        return int(major), int(minor)
    except Exception:
        return None


def find_base_python(explicit: str | None) -> str:
    candidates = [explicit] if explicit else []
    candidates.extend(
        [
            "/opt/homebrew/bin/python3.12",
            "/opt/homebrew/bin/python3.13",
            "/opt/homebrew/bin/python3.11",
            "/opt/homebrew/bin/python3.10",
            "python3.12",
            "python3.13",
            "python3.11",
            "python3.10",
        ]
    )
    for candidate in candidates:
        if not candidate:
            continue
        resolved = shutil.which(candidate) if not Path(candidate).is_absolute() else candidate
        if not resolved or not Path(resolved).exists():
            continue
        version = python_version(resolved)
        if version and version[0] == 3 and 10 <= version[1] <= 13:
            return str(Path(resolved).resolve())
    raise RuntimeError("Python 3.10-3.13 is required. Homebrew Python 3.12 is recommended.")


def main() -> int:
    args = parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("This installer is for Apple Silicon macOS (arm64).")

    runtime_root = Path(args.runtime_root).expanduser()
    hf_home = Path(args.hf_home).expanduser()
    helper = Path(args.helper).expanduser().resolve()
    if not helper.exists():
        raise RuntimeError(f"DA3 VOS helper was not found: {helper}")
    if args.helper_only:
        runtime_root.mkdir(parents=True, exist_ok=True)
        installed_helper = runtime_root / "da3_vos_helper.py"
        copy_runtime_helper(helper, installed_helper)
        print(json.dumps({"helper": str(installed_helper), "source": str(helper)}, ensure_ascii=False))
        return 0
    base_python = find_base_python(args.python)
    venv_python = runtime_root / ".venv" / "bin" / "python3"

    runtime_root.mkdir(parents=True, exist_ok=True)
    hf_home.mkdir(parents=True, exist_ok=True)
    if not venv_python.exists():
        run([base_python, "-m", "venv", str(runtime_root / ".venv")])

    run([str(venv_python), "-m", "pip", "install", "--upgrade", "pip", "wheel"])
    install_command = [str(venv_python), "-m", "pip", "install"]
    if args.force_reinstall:
        install_command.append("--force-reinstall")
    install_command.append(PACKAGE_URL)
    run(install_command)
    patched_export = patch_lazy_export_imports(venv_python)
    print(f"Patched inference-only lazy exporters: {patched_export}", flush=True)

    installed_helper = runtime_root / "da3_vos_helper.py"
    copy_runtime_helper(helper, installed_helper)

    env = os.environ.copy()
    env["HF_HOME"] = str(hf_home)
    env["HF_HUB_CACHE"] = str(hf_home / "hub")
    env["PYTORCH_ENABLE_MPS_FALLBACK"] = "1"
    if not args.skip_model:
        run([str(venv_python), str(installed_helper), "download", "--model", MODEL_ID], env=env)
    check = subprocess.check_output(
        [str(venv_python), str(installed_helper), "check", "--model", MODEL_ID],
        env=env,
        text=True,
    )
    status = json.loads(check)
    status.update(
        {
            "runtime_root": str(runtime_root),
            "hf_home": str(hf_home),
            "runtime_source": f"Aedelon/awesome-depth-anything-3@{AWEDELON_COMMIT}",
            "weights_source": MODEL_ID,
        }
    )
    (runtime_root / "install-status.json").write_text(
        json.dumps(status, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )
    print(json.dumps(status, ensure_ascii=False), flush=True)
    if status.get("device") != "mps":
        raise RuntimeError("Runtime installed, but PyTorch did not select the Apple MPS device.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.CalledProcessError as exc:
        print(f"error: command failed with exit code {exc.returncode}", file=sys.stderr)
        sys.exit(exc.returncode or 1)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
