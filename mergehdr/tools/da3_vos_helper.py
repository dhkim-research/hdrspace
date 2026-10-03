#!/usr/bin/env python3
"""Image-based VOS estimation for 180-degree equidistant HDR views.

The depth network receives rectilinear tiles, never the fisheye image directly.
Segmentation is intentionally kept separate: pass masks exported by SAM3 or the
candidate directories produced by hdrspace's MLX SAM3 helper.
"""

from __future__ import annotations

import argparse
import contextlib
import csv
import io
import json
import math
import os
import re
import subprocess
import struct
import sys
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image, ImageDraw, ImageFont


MODEL_ID = "depth-anything/DA3METRIC-LARGE"
METRIC_FOCAL_NORMALIZER = 300.0
DEFAULT_TILE_FOV_DEG = 105.0


def add_satisfaction_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--satisfaction-building-coefficient",
        type=float,
        help="Optional coefficient a in a*log10(1+V_built) + b*log10(1+V_greenery) + c",
    )
    parser.add_argument(
        "--satisfaction-greenery-coefficient",
        type=float,
        help="Optional coefficient b in a*log10(1+V_built) + b*log10(1+V_greenery) + c",
    )
    parser.add_argument(
        "--satisfaction-intercept",
        type=float,
        help="Optional intercept c; all three satisfaction coefficients must be supplied together",
    )


def add_projection_arguments(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--projection",
        choices=("auto", "equidistant", "perspective"),
        default="auto",
        help="Read Radiance VIEW metadata by default; use an explicit value only when metadata is absent",
    )
    parser.add_argument(
        "--fov",
        type=float,
        help="Legacy shortcut that sets both horizontal and vertical FOV in degrees",
    )
    parser.add_argument("--horizontal-fov", type=float, help="Override horizontal FOV in degrees")
    parser.add_argument("--vertical-fov", type=float, help="Override vertical FOV in degrees")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Estimate building and greenery VOS from a segmented 180-degree view."
    )
    sub = parser.add_subparsers(dest="command", required=True)

    check = sub.add_parser("check", help="Report the selected PyTorch device and model cache state")
    check.add_argument("--model", default=MODEL_ID)

    download = sub.add_parser("download", help="Download the metric model without running inference")
    download.add_argument("--model", default=MODEL_ID)

    analyze = sub.add_parser("analyze", help="Run tiled metric depth and calculate category VOS")
    analyze.add_argument("--input", required=True, help="Tone-mapped RGB preview or HDR/EXR input")
    analyze.add_argument(
        "--original-input",
        help="Original HDR used to read projection metadata when --input is a generated preview",
    )
    analyze.add_argument("--output-dir", required=True)
    analyze.add_argument("--building-mask", action="append", default=[])
    analyze.add_argument("--greenery-mask", action="append", default=[])
    analyze.add_argument("--building-candidates", action="append", default=[])
    analyze.add_argument("--greenery-candidates", action="append", default=[])
    analyze.add_argument("--model", default=MODEL_ID)
    analyze.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    add_projection_arguments(analyze)
    analyze.add_argument("--tile-size", type=int, default=504)
    analyze.add_argument("--tile-fov", type=float, default=DEFAULT_TILE_FOV_DEG)
    analyze.add_argument("--min-depth", type=float, default=0.10)
    analyze.add_argument("--max-depth", type=float, default=100.0)
    analyze.add_argument(
        "--window-distance",
        type=float,
        default=0.0,
        help="Perpendicular observer-to-glazing distance in metres; subtracts indoor VOS",
    )
    analyze.add_argument(
        "--window-mask",
        help="Optional white-window/black-occlusion mask (PNG or HDR) in the analyzed view",
    )
    analyze.add_argument("--output-max-side", type=int, default=2048, help="Limit output maps to control fisheye stitching memory; 0 keeps full size")
    analyze.add_argument("--include-instances", action="store_true", help="Union instance masks with semantic masks")
    analyze.add_argument(
        "--candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="semantic-union",
        help="Choose semantic masks, or only confident SAM instances with semantic fallback",
    )
    analyze.add_argument(
        "--building-candidate-policy",
        choices=("semantic-union", "confident-instances"),
    )
    analyze.add_argument(
        "--greenery-candidate-policy",
        choices=("semantic-union", "confident-instances"),
    )
    analyze.add_argument("--instance-min-score", type=float, default=0.55)
    analyze.add_argument("--dry-run-depth", type=float, help="Skip DA3 and use a constant radial depth (tests only)")
    add_satisfaction_arguments(analyze)

    auto = sub.add_parser("auto", help="Run the existing hdrspace SAM3 runtime, then DA3 and VOS")
    auto.add_argument("--input", required=True, help="HDR/EXR or display-ready view")
    auto.add_argument(
        "--clear-reference",
        help="Unoccluded image from the same camera, used for shades or Venetian blinds",
    )
    auto.add_argument("--output-dir", required=True)
    auto.add_argument("--sam-runtime-root", default=str(Path.home() / "Library" / "Application Support" / "hdrspace" / "mlx-sam3-runtime"))
    auto.add_argument("--sam-hf-home", default=str(Path.home() / "Library" / "Application Support" / "hdrspace" / "mlx-sam3-hf-home"))
    auto.add_argument("--building-prompt", default="exterior building; outdoor building facade")
    auto.add_argument("--greenery-prompt", default="tree foliage; outdoor tree; green leaves")
    auto.add_argument("--sam-confidence", type=float, default=0.5)
    auto.add_argument("--preview-max-side", type=int, default=1400)
    auto.add_argument("--model", default=MODEL_ID)
    auto.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    add_projection_arguments(auto)
    auto.add_argument("--tile-size", type=int, default=504)
    auto.add_argument("--tile-fov", type=float, default=DEFAULT_TILE_FOV_DEG)
    auto.add_argument("--min-depth", type=float, default=0.10)
    auto.add_argument("--max-depth", type=float, default=100.0)
    auto.add_argument(
        "--window-distance",
        type=float,
        help="Perpendicular observer-to-glazing distance; inferred from names such as view_1m when omitted",
    )
    auto.add_argument(
        "--window-mask",
        help="Optional white-window/black-occlusion mask (PNG or HDR) in the interior view",
    )
    auto.add_argument("--output-max-side", type=int, default=2048)
    auto.add_argument("--include-instances", action="store_true")
    auto.add_argument(
        "--candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="confident-instances",
    )
    auto.add_argument(
        "--building-candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="confident-instances",
    )
    auto.add_argument(
        "--greenery-candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="semantic-union",
    )
    auto.add_argument("--instance-min-score", type=float, default=0.55)
    auto.add_argument("--dry-run-depth", type=float, help=argparse.SUPPRESS)
    add_satisfaction_arguments(auto)

    pair = sub.add_parser(
        "pair",
        help="Analyze interior/exterior HDRs together and report paired depth/VOS error",
    )
    pair.add_argument("--internal-input", required=True, help="View rendered or photographed from inside")
    pair.add_argument("--external-input", required=True, help="Matching unobstructed exterior reference view")
    pair.add_argument("--output-dir", required=True)
    pair.add_argument("--sam-runtime-root", default=str(Path.home() / "Library" / "Application Support" / "hdrspace" / "mlx-sam3-runtime"))
    pair.add_argument("--sam-hf-home", default=str(Path.home() / "Library" / "Application Support" / "hdrspace" / "mlx-sam3-hf-home"))
    pair.add_argument("--building-prompt", default="exterior building; outdoor building facade")
    pair.add_argument("--greenery-prompt", default="tree foliage; outdoor tree; green leaves")
    pair.add_argument("--sam-confidence", type=float, default=0.5)
    pair.add_argument("--preview-max-side", type=int, default=1400)
    pair.add_argument("--model", default=MODEL_ID)
    pair.add_argument("--device", choices=("auto", "mps", "cpu"), default="auto")
    add_projection_arguments(pair)
    pair.add_argument("--tile-size", type=int, default=504)
    pair.add_argument("--tile-fov", type=float, default=DEFAULT_TILE_FOV_DEG)
    pair.add_argument("--min-depth", type=float, default=0.10)
    pair.add_argument("--max-depth", type=float, default=100.0)
    pair.add_argument(
        "--window-distance",
        type=float,
        help="Interior observer-to-glazing distance in metres; inferred from the interior filename when omitted",
    )
    pair.add_argument(
        "--window-mask",
        help="Optional white-window/black-occlusion mask (PNG or HDR) registered to the interior view",
    )
    pair.add_argument(
        "--estimate-window-distance",
        action="store_true",
        help="Estimate observer-to-window distance from the paired/interior DA3 results",
    )
    pair.add_argument("--output-max-side", type=int, default=2048)
    pair.add_argument("--include-instances", action="store_true")
    pair.add_argument(
        "--candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="confident-instances",
    )
    pair.add_argument(
        "--building-candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="confident-instances",
    )
    pair.add_argument(
        "--greenery-candidate-policy",
        choices=("semantic-union", "confident-instances"),
        default="semantic-union",
    )
    pair.add_argument("--instance-min-score", type=float, default=0.55)
    pair.add_argument("--dry-run-depth", type=float, help=argparse.SUPPRESS)
    add_satisfaction_arguments(pair)
    return parser.parse_args()


def select_device(requested: str = "auto") -> str:
    import torch

    if requested == "mps":
        if not torch.backends.mps.is_available():
            raise RuntimeError("MPS was requested but is not available in this PyTorch runtime.")
        return "mps"
    if requested == "cpu":
        return "cpu"
    return "mps" if torch.backends.mps.is_available() else "cpu"


def model_cache_path(model_id: str) -> Path:
    cache_root = Path(os.environ.get("HF_HUB_CACHE", Path(os.environ.get("HF_HOME", Path.home() / ".cache" / "huggingface")) / "hub"))
    return cache_root / ("models--" + model_id.replace("/", "--"))


def command_check(args: argparse.Namespace) -> int:
    import platform
    import torch

    result = {
        "architecture": platform.machine(),
        "device": select_device("auto"),
        "mps_built": bool(torch.backends.mps.is_built()),
        "mps_available": bool(torch.backends.mps.is_available()),
        "model": args.model,
        "model_cached": model_cache_path(args.model).exists(),
        "torch": torch.__version__,
    }
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


def command_download(args: argparse.Namespace) -> int:
    from huggingface_hub import snapshot_download

    path = snapshot_download(
        repo_id=args.model,
        allow_patterns=("*.json", "*.safetensors", "*.yaml", "*.yml"),
    )
    print(json.dumps({"model": args.model, "path": path}, ensure_ascii=False))
    return 0


def srgb_encode(linear: np.ndarray) -> np.ndarray:
    linear = np.clip(linear, 0.0, None)
    return np.where(
        linear <= 0.0031308,
        12.92 * linear,
        1.055 * np.power(linear, 1.0 / 2.4) - 0.055,
    )


def tonemap_hdr(rgb: np.ndarray) -> np.ndarray:
    rgb = np.nan_to_num(np.asarray(rgb, dtype=np.float32), nan=0.0, posinf=0.0, neginf=0.0)
    rgb = np.clip(rgb, 0.0, None)
    luminance = 0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]
    positive = luminance[luminance > 1.0e-8]
    if positive.size == 0:
        return np.zeros((*rgb.shape[:2], 3), dtype=np.uint8)
    reference = float(np.percentile(positive, 90.0))
    scaled = rgb * (0.8 / max(reference, 1.0e-8))
    mapped = scaled / (1.0 + scaled)
    return np.clip(np.rint(srgb_encode(mapped) * 255.0), 0, 255).astype(np.uint8)


def load_display_rgb(path: Path) -> np.ndarray:
    try:
        with Image.open(path) as image:
            return np.asarray(image.convert("RGB"), dtype=np.uint8)
    except Exception:
        pass

    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2

    raw = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if raw is None:
        raise RuntimeError(f"Could not read input image: {path}")
    if raw.ndim == 2:
        raw = np.repeat(raw[..., None], 3, axis=2)
    if raw.shape[2] >= 3:
        raw = raw[..., :3][..., ::-1]
    if raw.dtype == np.uint8:
        return raw.copy()
    if np.issubdtype(raw.dtype, np.integer):
        maximum = float(np.iinfo(raw.dtype).max)
        return np.clip(np.rint(raw.astype(np.float32) * (255.0 / maximum)), 0, 255).astype(np.uint8)
    return tonemap_hdr(raw)


def radiance_header_text(path: Path) -> str | None:
    try:
        with path.open("rb") as handle:
            prefix = handle.read(131072)
    except OSError:
        return None
    if not prefix.startswith((b"#?RADIANCE", b"#?RGBE")):
        return None
    header_end = prefix.find(b"\n\n")
    if header_end < 0:
        header_end = prefix.find(b"\r\n\r\n")
    if header_end >= 0:
        prefix = prefix[:header_end]
    return prefix.decode("latin-1", errors="replace")


def parse_radiance_view(path: Path) -> dict[str, object] | None:
    header = radiance_header_text(path)
    if not header:
        return None
    view_lines = [line.strip() for line in header.splitlines() if line.strip().upper().startswith("VIEW=")]
    search_text = view_lines[-1] if view_lines else header
    projection_match = re.search(r"(?:^|\s)-(vta|vtv)(?=\s|$)", search_text, flags=re.IGNORECASE)
    horizontal_match = re.search(r"(?:^|\s)-vh\s+([-+0-9.eE]+)", search_text, flags=re.IGNORECASE)
    vertical_match = re.search(r"(?:^|\s)-vv\s+([-+0-9.eE]+)", search_text, flags=re.IGNORECASE)
    if not projection_match and not horizontal_match and not vertical_match:
        return None
    projection_token = projection_match.group(1).casefold() if projection_match else None
    projection = {"vta": "equidistant", "vtv": "perspective"}.get(projection_token)
    return {
        "projection": projection,
        "horizontal_fov_degrees": float(horizontal_match.group(1)) if horizontal_match else None,
        "vertical_fov_degrees": float(vertical_match.group(1)) if vertical_match else None,
        "radiance_view_type": f"-{projection_token}" if projection_token else None,
        "source": "Radiance VIEW header" if view_lines else "Radiance command header",
        "view": search_text.removeprefix("VIEW=").strip() if view_lines else None,
    }


def resolve_projection(
    args: argparse.Namespace,
    metadata_path: Path,
    image_width: int,
    image_height: int,
) -> tuple[str, float, float, dict[str, object] | None]:
    metadata = parse_radiance_view(metadata_path)
    projection = args.projection
    if projection == "auto":
        projection = str(metadata.get("projection")) if metadata and metadata.get("projection") else ""
        if not projection:
            raise ValueError(
                "Projection metadata was not found. Supply --projection and either --fov or horizontal/vertical FOV."
            )

    horizontal = args.horizontal_fov if args.horizontal_fov is not None else args.fov
    vertical = args.vertical_fov if args.vertical_fov is not None else args.fov
    if horizontal is None and metadata:
        horizontal = metadata.get("horizontal_fov_degrees")
    if vertical is None and metadata:
        vertical = metadata.get("vertical_fov_degrees")

    if projection == "perspective":
        if horizontal is None and vertical is not None:
            vertical_radians = math.radians(float(vertical))
            horizontal = math.degrees(
                2.0 * math.atan((image_width / image_height) * math.tan(vertical_radians * 0.5))
            )
        if vertical is None and horizontal is not None:
            horizontal_radians = math.radians(float(horizontal))
            vertical = math.degrees(
                2.0 * math.atan((image_height / image_width) * math.tan(horizontal_radians * 0.5))
            )
    else:
        if horizontal is None:
            horizontal = vertical
        if vertical is None:
            vertical = horizontal

    if horizontal is None or vertical is None:
        raise ValueError("Both horizontal and vertical FOV could not be resolved from metadata or arguments.")
    horizontal = float(horizontal)
    vertical = float(vertical)
    if not (0.0 < horizontal < 180.0 + (1.0e-9 if projection == "equidistant" else 0.0)):
        raise ValueError("Horizontal FOV must be positive and no greater than 180 degrees.")
    if not (0.0 < vertical < 180.0 + (1.0e-9 if projection == "equidistant" else 0.0)):
        raise ValueError("Vertical FOV must be positive and no greater than 180 degrees.")
    if projection == "perspective" and (horizontal >= 180.0 or vertical >= 180.0):
        raise ValueError("Perspective FOV must be less than 180 degrees.")
    return projection, horizontal, vertical, metadata


def resize_mask(mask: np.ndarray, size: tuple[int, int]) -> np.ndarray:
    image = Image.fromarray((np.asarray(mask) > 0).astype(np.uint8) * 255, mode="L")
    if image.size != size:
        image = image.resize(size, Image.Resampling.NEAREST)
    return np.asarray(image) > 127


def load_mask(path: Path) -> np.ndarray:
    try:
        with Image.open(path) as image:
            return np.asarray(image.convert("L")) > 127
    except Exception:
        pass

    os.environ.setdefault("OPENCV_IO_ENABLE_OPENEXR", "1")
    import cv2

    raw = cv2.imread(str(path), cv2.IMREAD_UNCHANGED)
    if raw is None:
        raise RuntimeError(f"Could not read mask image: {path}")
    values = np.asarray(raw)
    if values.ndim == 3:
        values = np.max(values[..., :3], axis=2)
    if np.issubdtype(values.dtype, np.integer):
        threshold = 0.5 * float(np.iinfo(values.dtype).max)
    else:
        threshold = 0.5
    return np.nan_to_num(values.astype(np.float32), nan=0.0) > threshold


def candidate_mask_paths(
    directory: Path,
    include_instances: bool,
    policy: str,
    instance_min_score: float,
) -> list[Path]:
    metadata_path = directory / "candidates.tsv"
    if not metadata_path.exists():
        return sorted(directory.glob("mask_*.png"))

    semantic: list[Path] = []
    instances: list[tuple[float, Path]] = []
    with metadata_path.open("r", encoding="utf-8", newline="") as handle:
        for row in csv.DictReader(handle, delimiter="\t"):
            candidate = directory / str(row.get("filename", ""))
            if not candidate.exists():
                continue
            if str(row.get("kind", "")).casefold() == "semantic":
                semantic.append(candidate)
            elif str(row.get("kind", "")).casefold() == "instance":
                try:
                    score = float(row.get("score", 0.0))
                except (TypeError, ValueError):
                    score = 0.0
                instances.append((score, candidate))

    if policy == "confident-instances":
        confident = [path for score, path in instances if score >= instance_min_score]
        return confident if confident else semantic

    instance_paths = [path for _, path in instances] if include_instances else []
    return semantic + instance_paths if semantic else instance_paths


def collect_category_mask(
    explicit_paths: Iterable[str],
    candidate_dirs: Iterable[str],
    include_instances: bool,
    candidate_policy: str,
    instance_min_score: float,
) -> tuple[np.ndarray | None, list[str]]:
    paths = [Path(path).expanduser() for path in explicit_paths]
    for directory in candidate_dirs:
        paths.extend(
            candidate_mask_paths(
                Path(directory).expanduser(),
                include_instances,
                candidate_policy,
                instance_min_score,
            )
        )
    paths = [path for path in paths if path.exists()]
    if not paths:
        return None, []

    first = load_mask(paths[0])
    height, width = first.shape
    combined = first.copy()
    for path in paths[1:]:
        combined |= resize_mask(load_mask(path), (width, height))
    return combined, [str(path) for path in paths]


def run_sam_one_shot(
    runtime_root: Path,
    hf_home: Path,
    preview_path: Path,
    output_dir: Path,
    prompt: str,
    target_width: int,
    target_height: int,
    confidence: float,
) -> None:
    python = runtime_root / ".venv" / "bin" / "python3"
    helper = runtime_root / "mlx_sam3_mask_helper.py"
    if not python.exists() or not helper.exists() or not (runtime_root / "sam3").exists():
        raise RuntimeError(
            "hdrspace MLX SAM3 runtime was not found. Expected it in " + str(runtime_root)
        )
    env = os.environ.copy()
    env["PYTHONPATH"] = str(runtime_root) + (":" + env["PYTHONPATH"] if env.get("PYTHONPATH") else "")
    env["HF_HOME"] = str(hf_home)
    env["HF_HUB_CACHE"] = str(hf_home / "hub")
    completed = subprocess.run(
        [
            str(python),
            str(helper),
            "--input",
            str(preview_path),
            "--output-dir",
            str(output_dir),
            "--prompt",
            prompt,
            "--target-width",
            str(target_width),
            "--target-height",
            str(target_height),
            "--confidence-threshold",
            str(confidence),
        ],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    if completed.returncode != 0:
        diagnostic = (completed.stdout or "").strip()
        if len(diagnostic) > 4000:
            diagnostic = diagnostic[-4000:]
        raise RuntimeError(
            "SAM3 segmentation failed"
            + (f":\n{diagnostic}" if diagnostic else ".")
        )


def bilinear_sample(image: np.ndarray, x: np.ndarray, y: np.ndarray, fill: float = 0.0) -> np.ndarray:
    source = np.asarray(image)
    height, width = source.shape[:2]
    valid = (x >= 0.0) & (x <= width - 1.0) & (y >= 0.0) & (y <= height - 1.0)
    x0 = np.clip(np.floor(x).astype(np.int64), 0, width - 1)
    y0 = np.clip(np.floor(y).astype(np.int64), 0, height - 1)
    x1 = np.minimum(x0 + 1, width - 1)
    y1 = np.minimum(y0 + 1, height - 1)
    wx = x - x0
    wy = y - y0
    extra_dims = (1,) * max(0, source.ndim - 2)
    wx = wx.reshape(wx.shape + extra_dims)
    wy = wy.reshape(wy.shape + extra_dims)
    sampled = (
        source[y0, x0] * (1.0 - wx) * (1.0 - wy)
        + source[y0, x1] * wx * (1.0 - wy)
        + source[y1, x0] * (1.0 - wx) * wy
        + source[y1, x1] * wx * wy
    )
    valid_expanded = valid.reshape(valid.shape + extra_dims)
    return np.where(valid_expanded, sampled, fill)


def equidistant_rays(
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray]:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    nx = (xx + 0.5) / float(width) - 0.5
    ny = (yy + 0.5) / float(height) - 0.5
    u = nx * math.radians(horizontal_fov_deg)
    v = ny * math.radians(vertical_fov_deg)
    theta = np.sqrt(u * u + v * v)
    sin_theta = np.sin(theta)
    scale = np.divide(sin_theta, theta, out=np.ones_like(theta), where=theta > 1.0e-8)
    rays = np.stack((u * scale, v * scale, np.cos(theta)), axis=-1)
    normalized_radius = np.sqrt(nx * nx + ny * ny)
    valid = (normalized_radius <= 0.5) & (rays[..., 2] >= 0.0)
    rays[~valid] = 0.0
    return rays, valid


FACES = (
    ("front", (0.0, 0.0, 1.0), (1.0, 0.0, 0.0), (0.0, 1.0, 0.0)),
    ("right", (1.0, 0.0, 0.0), (0.0, 0.0, -1.0), (0.0, 1.0, 0.0)),
    ("left", (-1.0, 0.0, 0.0), (0.0, 0.0, 1.0), (0.0, 1.0, 0.0)),
    ("up", (0.0, -1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, 1.0)),
    ("down", (0.0, 1.0, 0.0), (1.0, 0.0, 0.0), (0.0, 0.0, -1.0)),
)


def project_rays_to_equidistant(
    rays: np.ndarray,
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    z = np.clip(rays[..., 2], -1.0, 1.0)
    theta = np.arccos(z)
    radial_xy = np.sqrt(rays[..., 0] ** 2 + rays[..., 1] ** 2)
    angular_scale = np.divide(theta, radial_xy, out=np.ones_like(theta), where=radial_xy > 1.0e-8)
    u = rays[..., 0] * angular_scale
    v = rays[..., 1] * angular_scale
    nx = u / math.radians(horizontal_fov_deg)
    ny = v / math.radians(vertical_fov_deg)
    x = (nx + 0.5) * width - 0.5
    y = (ny + 0.5) * height - 0.5
    valid = (np.sqrt(nx * nx + ny * ny) <= 0.5 + 1.0e-6) & (z >= 0.0)
    return x, y, valid


def make_rectilinear_tile(
    preview: np.ndarray,
    forward: np.ndarray,
    right: np.ndarray,
    down: np.ndarray,
    tile_size: int,
    tile_fov_deg: float,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> np.ndarray:
    focal = 0.5 * tile_size / math.tan(math.radians(tile_fov_deg * 0.5))
    yy, xx = np.mgrid[0:tile_size, 0:tile_size].astype(np.float32)
    xn = (xx - (tile_size - 1.0) * 0.5) / focal
    yn = (yy - (tile_size - 1.0) * 0.5) / focal
    rays = forward + xn[..., None] * right + yn[..., None] * down
    rays /= np.linalg.norm(rays, axis=-1, keepdims=True)
    source_x, source_y, valid = project_rays_to_equidistant(
        rays,
        preview.shape[1],
        preview.shape[0],
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    tile = bilinear_sample(preview.astype(np.float32), source_x, source_y)
    tile[~valid] = 0.0
    return np.clip(np.rint(tile), 0, 255).astype(np.uint8)


def infer_tile_depths(
    preview: np.ndarray,
    model_id: str,
    device: str,
    tile_size: int,
    tile_fov_deg: float,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[list[np.ndarray], str]:
    import torch
    from depth_anything_3.api import DepthAnything3

    if device == "mps":
        os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
    model = DepthAnything3.from_pretrained(model_id)
    model = model.to(torch.device(device))
    model.eval()
    focal = 0.5 * tile_size / math.tan(math.radians(tile_fov_deg * 0.5))
    depths: list[np.ndarray] = []
    for _, forward_values, right_values, down_values in FACES:
        forward = np.asarray(forward_values, dtype=np.float32)
        right = np.asarray(right_values, dtype=np.float32)
        down = np.asarray(down_values, dtype=np.float32)
        tile = make_rectilinear_tile(
            preview,
            forward,
            right,
            down,
            tile_size,
            tile_fov_deg,
            horizontal_fov_deg,
            vertical_fov_deg,
        )
        prediction = model.inference(
            [Image.fromarray(tile, mode="RGB")],
            process_res=tile_size,
            process_res_method="upper_bound_resize",
        )
        raw_depth = np.asarray(prediction.depth[0], dtype=np.float32)
        if raw_depth.shape != (tile_size, tile_size):
            raw_depth = np.asarray(
                Image.fromarray(raw_depth, mode="F").resize((tile_size, tile_size), Image.Resampling.BILINEAR),
                dtype=np.float32,
            )
        # Official DA3Metric conversion: metric_depth = focal_px * network_depth / 300.
        depths.append(raw_depth * (focal / METRIC_FOCAL_NORMALIZER))
    if device == "mps":
        torch.mps.empty_cache()
    return depths, device


def stitch_radial_depth(
    tile_depths: list[np.ndarray],
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
    tile_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray]:
    rays, valid_fisheye = equidistant_rays(
        width,
        height,
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    tile_size = int(tile_depths[0].shape[0])
    focal = 0.5 * tile_size / math.tan(math.radians(tile_fov_deg * 0.5))
    half_extent = math.tan(math.radians(tile_fov_deg * 0.5))
    weighted_sum = np.zeros((height, width), dtype=np.float64)
    weight_sum = np.zeros((height, width), dtype=np.float64)

    for depth_z, (_, forward_values, right_values, down_values) in zip(tile_depths, FACES):
        forward = np.asarray(forward_values, dtype=np.float32)
        right = np.asarray(right_values, dtype=np.float32)
        down = np.asarray(down_values, dtype=np.float32)
        denominator = np.sum(rays * forward, axis=-1)
        xn = np.divide(np.sum(rays * right, axis=-1), denominator, out=np.zeros_like(denominator), where=denominator > 1.0e-6)
        yn = np.divide(np.sum(rays * down, axis=-1), denominator, out=np.zeros_like(denominator), where=denominator > 1.0e-6)
        inside = valid_fisheye & (denominator > 1.0e-6) & (np.abs(xn) <= half_extent) & (np.abs(yn) <= half_extent)
        tile_x = xn * focal + (tile_size - 1.0) * 0.5
        tile_y = yn * focal + (tile_size - 1.0) * 0.5
        sampled_z = bilinear_sample(depth_z, tile_x, tile_y)
        radial_depth = np.divide(sampled_z, denominator, out=np.zeros_like(sampled_z), where=denominator > 1.0e-6)
        edge = np.maximum(np.abs(xn), np.abs(yn)) / half_extent
        weight = np.clip(1.0 - edge, 0.02, 1.0) ** 2 * np.clip(denominator, 0.0, 1.0) ** 2
        good = inside & np.isfinite(radial_depth) & (radial_depth > 0.0)
        weighted_sum[good] += radial_depth[good] * weight[good]
        weight_sum[good] += weight[good]

    depth = np.divide(weighted_sum, weight_sum, out=np.zeros_like(weighted_sum), where=weight_sum > 0.0).astype(np.float32)
    coverage = np.clip(weight_sum / np.maximum(np.max(weight_sum), 1.0e-9), 0.0, 1.0).astype(np.float32)
    depth[~valid_fisheye] = 0.0
    coverage[~valid_fisheye] = 0.0
    return depth, coverage


def perspective_focal_lengths(
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[float, float]:
    fx = 0.5 * width / math.tan(math.radians(horizontal_fov_deg * 0.5))
    fy = 0.5 * height / math.tan(math.radians(vertical_fov_deg * 0.5))
    return fx, fy


def perspective_radial_factor(
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> np.ndarray:
    fx, fy = perspective_focal_lengths(width, height, horizontal_fov_deg, vertical_fov_deg)
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float64)
    x = (xx + 0.5 - 0.5 * width) / fx
    y = (yy + 0.5 - 0.5 * height) / fy
    return np.sqrt(1.0 + x * x + y * y).astype(np.float32)


def infer_perspective_depth(
    preview: np.ndarray,
    model_id: str,
    device: str,
    process_size: int,
    target_width: int,
    target_height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray, str, dict[str, float]]:
    import torch
    from depth_anything_3.api import DepthAnything3

    if device == "mps":
        os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")
    model = DepthAnything3.from_pretrained(model_id)
    model = model.to(torch.device(device))
    model.eval()
    prediction = model.inference(
        [Image.fromarray(preview, mode="RGB")],
        process_res=process_size,
        process_res_method="upper_bound_resize",
    )
    raw_depth = np.asarray(prediction.depth[0], dtype=np.float32)
    processed_height, processed_width = raw_depth.shape
    original_fx, original_fy = perspective_focal_lengths(
        preview.shape[1],
        preview.shape[0],
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    processed_fx = original_fx * processed_width / preview.shape[1]
    processed_fy = original_fy * processed_height / preview.shape[0]
    metric_focal = 0.5 * (processed_fx + processed_fy)
    z_depth = raw_depth * (metric_focal / METRIC_FOCAL_NORMALIZER)
    if z_depth.shape != (target_height, target_width):
        z_depth = np.asarray(
            Image.fromarray(z_depth, mode="F").resize(
                (target_width, target_height),
                Image.Resampling.BILINEAR,
            ),
            dtype=np.float32,
        )
    radial_depth = z_depth * perspective_radial_factor(
        target_width,
        target_height,
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    coverage = np.ones((target_height, target_width), dtype=np.float32)
    if device == "mps":
        torch.mps.empty_cache()
    details = {
        "processed_width": float(processed_width),
        "processed_height": float(processed_height),
        "processed_fx_px": float(processed_fx),
        "processed_fy_px": float(processed_fy),
        "metric_focal_px": float(metric_focal),
    }
    return radial_depth.astype(np.float32), coverage, device, details


def equidistant_solid_angle(
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray]:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float64)
    nx = (xx + 0.5) / float(width) - 0.5
    ny = (yy + 0.5) / float(height) - 0.5
    horizontal_radians = math.radians(horizontal_fov_deg)
    vertical_radians = math.radians(vertical_fov_deg)
    u = nx * horizontal_radians
    v = ny * vertical_radians
    theta = np.sqrt(u * u + v * v)
    jacobian = np.divide(np.sin(theta), theta, out=np.ones_like(theta), where=theta > 1.0e-10)
    solid_angle = jacobian * horizontal_radians * vertical_radians / float(width * height)
    valid = (np.sqrt(nx * nx + ny * ny) <= 0.5) & (np.cos(theta) >= 0.0)
    solid_angle[~valid] = 0.0
    return solid_angle.astype(np.float32), valid


def perspective_solid_angle(
    width: int,
    height: int,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray]:
    fx, fy = perspective_focal_lengths(width, height, horizontal_fov_deg, vertical_fov_deg)
    x_edges = (np.arange(width + 1, dtype=np.float64) - 0.5 * width) / fx
    y_edges = (np.arange(height + 1, dtype=np.float64) - 0.5 * height) / fy
    x_grid, y_grid = np.meshgrid(x_edges, y_edges)
    primitive = np.arctan2(x_grid * y_grid, np.sqrt(1.0 + x_grid * x_grid + y_grid * y_grid))
    solid_angle = np.abs(
        primitive[1:, 1:]
        - primitive[:-1, 1:]
        - primitive[1:, :-1]
        + primitive[:-1, :-1]
    )
    return solid_angle.astype(np.float32), np.ones((height, width), dtype=bool)


def window_radial_distance(
    width: int,
    height: int,
    projection: str,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
    perpendicular_distance_m: float,
) -> np.ndarray:
    """Return ray distance to a front-facing glazing plane."""
    if perpendicular_distance_m <= 0.0:
        return np.zeros((height, width), dtype=np.float32)
    if projection == "perspective":
        return (
            float(perpendicular_distance_m)
            * perspective_radial_factor(
                width,
                height,
                horizontal_fov_deg,
                vertical_fov_deg,
            )
        ).astype(np.float32)
    rays, valid = equidistant_rays(
        width,
        height,
        horizontal_fov_deg,
        vertical_fov_deg,
    )
    forward = rays[..., 2]
    distance = np.divide(
        float(perpendicular_distance_m),
        forward,
        out=np.full_like(forward, np.inf),
        where=forward > 1.0e-6,
    )
    distance[~valid] = 0.0
    return distance.astype(np.float32)


def inferred_window_distance(path: Path) -> float:
    match = re.search(r"(?:^|_)view_(\d+(?:\.\d+)?)m(?:_|$)", path.name, flags=re.IGNORECASE)
    return float(match.group(1)) if match else 0.0


def float_to_rgbe(rgb: np.ndarray) -> np.ndarray:
    values = np.maximum(np.asarray(rgb, dtype=np.float32), 0.0)
    maximum = np.max(values, axis=-1)
    mantissa, exponent = np.frexp(maximum)
    scale = np.divide(mantissa * 256.0, maximum, out=np.zeros_like(maximum), where=maximum > 1.0e-32)
    output = np.empty((*values.shape[:2], 4), dtype=np.uint8)
    output[..., :3] = np.clip(values * scale[..., None], 0.0, 255.0).astype(np.uint8)
    output[..., 3] = np.where(maximum > 1.0e-32, exponent + 128, 0).astype(np.uint8)
    return output


def encode_rle_channel(channel: np.ndarray) -> bytes:
    data = np.asarray(channel, dtype=np.uint8).tolist()
    output = bytearray()
    index = 0
    while index < len(data):
        run = 1
        while index + run < len(data) and run < 127 and data[index + run] == data[index]:
            run += 1
        if run >= 4:
            output.extend((128 + run, data[index]))
            index += run
            continue
        start = index
        index += run
        while index < len(data) and index - start < 128:
            next_run = 1
            while index + next_run < len(data) and next_run < 4 and data[index + next_run] == data[index]:
                next_run += 1
            if next_run >= 4:
                break
            available = 128 - (index - start)
            index += min(next_run, available)
        output.append(index - start)
        output.extend(data[start:index])
    return bytes(output)


def write_radiance_hdr(path: Path, rgb: np.ndarray, metadata: dict[str, object]) -> None:
    rgbe = float_to_rgbe(rgb)
    height, width = rgbe.shape[:2]
    with path.open("wb") as handle:
        handle.write(b"#?RADIANCE\n")
        handle.write(b"SOFTWARE= mergehdr da3_vos_helper\n")
        for key, value in metadata.items():
            handle.write(f"{key}= {value}\n".encode("ascii", errors="replace"))
        handle.write(b"FORMAT=32-bit_rle_rgbe\n\n")
        handle.write(f"-Y {height} +X {width}\n".encode("ascii"))
        if width < 8 or width > 32767:
            handle.write(rgbe.tobytes())
            return
        for row in rgbe:
            handle.write(bytes((2, 2, (width >> 8) & 255, width & 255)))
            for channel_index in range(4):
                handle.write(encode_rle_channel(row[:, channel_index]))


def depth_visualization(depth: np.ndarray, valid: np.ndarray) -> np.ndarray:
    positive = depth[valid & (depth > 0.0)]
    if positive.size == 0:
        return np.zeros((*depth.shape, 3), dtype=np.uint8)
    lo, hi = np.percentile(positive, (2.0, 98.0))
    normalized = np.clip((np.log(np.maximum(depth, 1.0e-6)) - math.log(max(lo, 1.0e-6))) / max(math.log(max(hi, lo + 1.0e-6)) - math.log(max(lo, 1.0e-6)), 1.0e-6), 0.0, 1.0)
    red = np.clip(1.5 - np.abs(4.0 * normalized - 3.0), 0.0, 1.0)
    green = np.clip(1.5 - np.abs(4.0 * normalized - 2.0), 0.0, 1.0)
    blue = np.clip(1.5 - np.abs(4.0 * normalized - 1.0), 0.0, 1.0)
    rgb = np.stack((red, green, blue), axis=-1)
    rgb[~valid] = 0.0
    return np.clip(np.rint(rgb * 255.0), 0, 255).astype(np.uint8)


def depth_annotation_font(size: int, bold: bool = False) -> ImageFont.ImageFont:
    names = ["DejaVuSans-Bold.ttf", "Arial Bold.ttf"] if bold else ["DejaVuSans.ttf", "Arial.ttf"]
    for name in names:
        try:
            return ImageFont.truetype(name, size=size)
        except OSError:
            continue
    return ImageFont.load_default(size=size)


def format_distance(distance_m: float) -> str:
    if distance_m < 10.0:
        return f"{distance_m:.2f} m"
    if distance_m < 100.0:
        return f"{distance_m:.1f} m"
    return f"{distance_m:.0f} m"


def representative_depth_points(valid: np.ndarray) -> list[tuple[int, int]]:
    height, width = valid.shape
    points: list[tuple[int, int]] = []
    x_limit = max(1, int(round(width * 0.82)))
    y_start = max(0, int(round(height * 0.08)))
    y_limit = max(y_start + 1, int(round(height * 0.94)))
    for row in range(3):
        y0 = y_start + (y_limit - y_start) * row // 3
        y1 = y_start + (y_limit - y_start) * (row + 1) // 3
        for column in range(4):
            x0 = x_limit * column // 4
            x1 = x_limit * (column + 1) // 4
            local = np.argwhere(valid[y0:y1, x0:x1])
            if local.size == 0:
                continue
            center_y = 0.5 * max(0, y1 - y0 - 1)
            center_x = 0.5 * max(0, x1 - x0 - 1)
            distances = (local[:, 0] - center_y) ** 2 + (local[:, 1] - center_x) ** 2
            selected = local[int(np.argmin(distances))]
            points.append((x0 + int(selected[1]), y0 + int(selected[0])))
    return points


def annotated_depth_visualization(
    depth: np.ndarray,
    valid: np.ndarray,
    title_prefix: str = "DA3 radial distance",
) -> Image.Image:
    base = Image.fromarray(depth_visualization(depth, valid), mode="RGB")
    positive = depth[valid & np.isfinite(depth) & (depth > 0.0)]
    if positive.size == 0:
        return base

    width, height = base.size
    lo, median, hi = (float(value) for value in np.percentile(positive, (2.0, 50.0, 98.0)))
    overlay = Image.new("RGBA", base.size, (0, 0, 0, 0))
    draw = ImageDraw.Draw(overlay)
    title_font = depth_annotation_font(max(18, min(width, height) // 38), bold=True)
    label_font = depth_annotation_font(max(14, min(width, height) // 55), bold=True)
    tick_font = depth_annotation_font(max(12, min(width, height) // 68))

    title = (
        f"{title_prefix} (m)   "
        f"P02 {format_distance(lo)}   median {format_distance(median)}   P98 {format_distance(hi)}"
    )
    title_box = draw.textbbox((0, 0), title, font=title_font, stroke_width=1)
    title_height = title_box[3] - title_box[1]
    draw.rounded_rectangle(
        (12, 12, min(width - 12, title_box[2] + 34), title_height + 34),
        radius=8,
        fill=(0, 0, 0, 175),
    )
    draw.text(
        (24, 20),
        title,
        font=title_font,
        fill=(255, 255, 255, 255),
        stroke_width=2,
        stroke_fill=(0, 0, 0, 255),
    )

    marker_radius = max(3, min(width, height) // 250)
    for x, y in representative_depth_points(valid):
        label = format_distance(float(depth[y, x]))
        draw.ellipse(
            (x - marker_radius, y - marker_radius, x + marker_radius, y + marker_radius),
            fill=(255, 255, 255, 255),
            outline=(0, 0, 0, 255),
            width=max(1, marker_radius // 2),
        )
        draw.text(
            (x + marker_radius + 4, y - marker_radius - 2),
            label,
            font=label_font,
            fill=(255, 255, 255, 255),
            stroke_width=2,
            stroke_fill=(0, 0, 0, 255),
        )

    bar_width = max(18, width // 75)
    bar_left = width - max(120, width // 9)
    bar_right = bar_left + bar_width
    bar_top = max(80, title_height + 54)
    bar_bottom = height - max(60, height // 25)
    if bar_bottom > bar_top:
        normalized = np.linspace(1.0, 0.0, bar_bottom - bar_top, dtype=np.float32)
        red = np.clip(1.5 - np.abs(4.0 * normalized - 3.0), 0.0, 1.0)
        green = np.clip(1.5 - np.abs(4.0 * normalized - 2.0), 0.0, 1.0)
        blue = np.clip(1.5 - np.abs(4.0 * normalized - 1.0), 0.0, 1.0)
        colors = np.clip(np.rint(np.stack((red, green, blue), axis=-1) * 255.0), 0, 255).astype(np.uint8)
        colorbar = np.repeat(colors[:, None, :], bar_width, axis=1)
        overlay.alpha_composite(Image.fromarray(colorbar, mode="RGB").convert("RGBA"), (bar_left, bar_top))
        draw.rectangle((bar_left, bar_top, bar_right, bar_bottom), outline=(255, 255, 255, 255), width=2)
        log_lo = math.log(max(lo, 1.0e-6))
        log_hi = math.log(max(hi, lo + 1.0e-6))
        for fraction in (0.0, 0.25, 0.5, 0.75, 1.0):
            y = int(round(bar_bottom - fraction * (bar_bottom - bar_top)))
            value = math.exp(log_lo + fraction * (log_hi - log_lo))
            draw.line((bar_right, y, bar_right + 8, y), fill=(255, 255, 255, 255), width=2)
            draw.text(
                (bar_right + 12, y),
                format_distance(value),
                font=tick_font,
                anchor="lm",
                fill=(255, 255, 255, 255),
                stroke_width=2,
                stroke_fill=(0, 0, 0, 255),
            )

    return Image.alpha_composite(base.convert("RGBA"), overlay).convert("RGB")


def segmentation_distance_overlay(
    preview: np.ndarray,
    depth: np.ndarray,
    valid: np.ndarray,
    building: np.ndarray,
    greenery: np.ndarray,
) -> Image.Image:
    height, width = depth.shape
    background = Image.fromarray(preview, mode="RGB")
    if background.size != (width, height):
        background = background.resize((width, height), Image.Resampling.LANCZOS)
    rgb = np.asarray(background, dtype=np.float32) * 0.28
    building_valid = valid & building
    greenery_valid = valid & greenery
    building_color = np.asarray((30.0, 190.0, 255.0), dtype=np.float32)
    greenery_color = np.asarray((65.0, 230.0, 90.0), dtype=np.float32)
    rgb[building_valid] = 0.30 * rgb[building_valid] + 0.70 * building_color
    rgb[greenery_valid] = 0.25 * rgb[greenery_valid] + 0.75 * greenery_color
    image = Image.fromarray(np.clip(np.rint(rgb), 0, 255).astype(np.uint8), mode="RGB").convert("RGBA")
    draw = ImageDraw.Draw(image)
    title_font = depth_annotation_font(max(18, min(width, height) // 40), bold=True)
    label_font = depth_annotation_font(max(14, min(width, height) // 58), bold=True)
    title = "Segmented DA3 radial distance   BUILDING = cyan   GREENERY = green"
    title_box = draw.textbbox((0, 0), title, font=title_font, stroke_width=1)
    title_height = title_box[3] - title_box[1]
    draw.rounded_rectangle(
        (12, 12, min(width - 12, title_box[2] + 34), title_height + 34),
        radius=8,
        fill=(0, 0, 0, 205),
    )
    draw.text(
        (24, 20),
        title,
        font=title_font,
        fill=(255, 255, 255, 255),
        stroke_width=2,
        stroke_fill=(0, 0, 0, 255),
    )
    marker_radius = max(3, min(width, height) // 250)
    categories = (
        ("B", building_valid, (110, 225, 255, 255)),
        ("G", greenery_valid, (130, 255, 145, 255)),
    )
    for prefix, category_valid, text_color in categories:
        for x, y in representative_depth_points(category_valid):
            draw.ellipse(
                (x - marker_radius, y - marker_radius, x + marker_radius, y + marker_radius),
                fill=text_color,
                outline=(0, 0, 0, 255),
                width=max(1, marker_radius // 2),
            )
            draw.text(
                (x + marker_radius + 4, y - marker_radius - 2),
                f"{prefix} {format_distance(float(depth[y, x]))}",
                font=label_font,
                fill=text_color,
                stroke_width=2,
                stroke_fill=(0, 0, 0, 255),
            )
    return image.convert("RGB")


def category_summary(mask: np.ndarray, depth: np.ndarray, solid_angle: np.ndarray, local_vos: np.ndarray) -> dict[str, object]:
    selected = mask & (depth > 0.0)
    values = depth[selected]
    total = float(np.sum(local_vos, dtype=np.float64))
    return {
        "pixels": int(np.count_nonzero(mask)),
        "solid_angle_sr": float(np.sum(solid_angle[mask], dtype=np.float64)),
        "median_depth_m": float(np.median(values)) if values.size else None,
        "vos_m3": total,
        # Legacy alias retained for readers of earlier Image VOS output.
        "vos_m3_sr": total,
    }


def satisfaction_summary(
    building_summary: dict[str, object],
    greenery_summary: dict[str, object],
    building_coefficient: float | None,
    greenery_coefficient: float | None,
    intercept: float | None,
) -> dict[str, object]:
    coefficients = (building_coefficient, greenery_coefficient, intercept)
    if all(value is None for value in coefficients):
        return {
            "score": None,
            "status": "not_configured",
            "reason": "Regression coefficients a, b, and c have not been established; only V_built and V_greenery are reported.",
            "building_coefficient": None,
            "greenery_coefficient": None,
            "intercept": None,
            "formula": "score = a * log10(1 + V_built) + b * log10(1 + V_greenery) + c",
        }
    if any(value is None for value in coefficients):
        raise ValueError(
            "Supply --satisfaction-building-coefficient, --satisfaction-greenery-coefficient, "
            "and --satisfaction-intercept together."
        )

    assert building_coefficient is not None
    assert greenery_coefficient is not None
    assert intercept is not None
    building_vos = float(building_summary["vos_m3_sr"])
    greenery_vos = float(greenery_summary["vos_m3_sr"])
    score = (
        building_coefficient * math.log10(1.0 + building_vos)
        + greenery_coefficient * math.log10(1.0 + greenery_vos)
        + intercept
    )
    return {
        "score": score,
        "status": "calculated",
        "reason": None,
        "building_coefficient": building_coefficient,
        "greenery_coefficient": greenery_coefficient,
        "intercept": intercept,
        "formula": "score = a * log10(1 + V_built) + b * log10(1 + V_greenery) + c",
    }


def command_analyze(args: argparse.Namespace) -> int:
    if not (90.0 < args.tile_fov < 140.0):
        raise ValueError("--tile-fov must be between 90 and 140 degrees for five-face coverage.")
    if args.tile_size < 224 or args.tile_size % 14 != 0:
        raise ValueError("--tile-size must be at least 224 and divisible by the DA3 patch size (14).")
    if args.max_depth <= args.min_depth:
        raise ValueError("--max-depth must be greater than --min-depth.")
    if args.window_distance < 0.0:
        raise ValueError("--window-distance must be non-negative.")

    preview = load_display_rgb(Path(args.input).expanduser())
    original_input_path = Path(getattr(args, "original_input", None) or args.input).expanduser()
    projection_metadata_path = Path(
        getattr(args, "inference_source", None) or getattr(args, "original_input", None) or args.input
    ).expanduser()
    projection, horizontal_fov, vertical_fov, projection_metadata = resolve_projection(
        args,
        projection_metadata_path,
        preview.shape[1],
        preview.shape[0],
    )
    building, building_sources = collect_category_mask(
        args.building_mask,
        args.building_candidates,
        args.include_instances,
        args.building_candidate_policy or args.candidate_policy,
        args.instance_min_score,
    )
    greenery, greenery_sources = collect_category_mask(
        args.greenery_mask,
        args.greenery_candidates,
        args.include_instances,
        args.greenery_candidate_policy or args.candidate_policy,
        args.instance_min_score,
    )
    if building is None and greenery is None:
        raise RuntimeError("No building or greenery masks were found.")
    reference = building if building is not None else greenery
    assert reference is not None
    source_mask_height, source_mask_width = reference.shape
    target_height, target_width = source_mask_height, source_mask_width
    if args.output_max_side > 0 and max(target_width, target_height) > args.output_max_side:
        scale = float(args.output_max_side) / float(max(target_width, target_height))
        target_width = max(1, int(round(target_width * scale)))
        target_height = max(1, int(round(target_height * scale)))
    building = np.zeros_like(reference) if building is None else resize_mask(building, (target_width, target_height))
    greenery = np.zeros_like(reference) if greenery is None else resize_mask(greenery, (target_width, target_height))
    if building.shape != (target_height, target_width):
        building = resize_mask(building, (target_width, target_height))
    if greenery.shape != (target_height, target_width):
        greenery = resize_mask(greenery, (target_width, target_height))
    window_mask_path = Path(args.window_mask).expanduser() if getattr(args, "window_mask", None) else None
    window_mask = (
        resize_mask(load_mask(window_mask_path), (target_width, target_height))
        if window_mask_path is not None
        else np.ones((target_height, target_width), dtype=bool)
    )

    if projection == "equidistant":
        solid_angle, valid_view = equidistant_solid_angle(
            target_width,
            target_height,
            horizontal_fov,
            vertical_fov,
        )
    else:
        solid_angle, valid_view = perspective_solid_angle(
            target_width,
            target_height,
            horizontal_fov,
            vertical_fov,
        )
    window_mask &= valid_view
    building &= window_mask
    greenery &= window_mask
    overlap = building & greenery
    building_before_overlap = int(np.count_nonzero(building))
    greenery_before_overlap = int(np.count_nonzero(greenery))
    overlap_pixels = int(np.count_nonzero(overlap))
    building &= ~overlap
    greenery &= ~overlap

    if args.dry_run_depth is not None:
        if args.dry_run_depth <= 0.0:
            raise ValueError("--dry-run-depth must be positive.")
        depth = np.where(valid_view, float(args.dry_run_depth), 0.0).astype(np.float32)
        coverage = valid_view.astype(np.float32)
        device = "dry-run"
        depth_inference_details: dict[str, float] = {}
    else:
        device = select_device(args.device)
        if projection == "equidistant":
            tile_depths, device = infer_tile_depths(
                preview,
                args.model,
                device,
                args.tile_size,
                args.tile_fov,
                horizontal_fov,
                vertical_fov,
            )
            depth, coverage = stitch_radial_depth(
                tile_depths,
                target_width,
                target_height,
                horizontal_fov,
                vertical_fov,
                args.tile_fov,
            )
            depth_inference_details = {"rectilinear_tiles": float(len(tile_depths))}
        else:
            depth, coverage, device, depth_inference_details = infer_perspective_depth(
                preview,
                args.model,
                device,
                args.tile_size,
                target_width,
                target_height,
                horizontal_fov,
                vertical_fov,
            )

    originally_valid_depth = valid_view & np.isfinite(depth) & (depth > 0.0)
    below = originally_valid_depth & (depth < args.min_depth)
    above = originally_valid_depth & (depth > args.max_depth)
    depth = np.where(originally_valid_depth, np.clip(depth, args.min_depth, args.max_depth), 0.0).astype(np.float32)
    glazing_depth = window_radial_distance(
        target_width,
        target_height,
        projection,
        horizontal_fov,
        vertical_fov,
        args.window_distance,
    )
    exterior_radial_cubic = np.maximum(depth ** 3 - glazing_depth ** 3, 0.0)
    volume_factor = solid_angle * exterior_radial_cubic / 3.0
    building_vos_map = np.where(building, volume_factor, 0.0).astype(np.float32)
    greenery_vos_map = np.where(greenery, volume_factor, 0.0).astype(np.float32)

    output_dir = Path(args.output_dir).expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)
    np.save(output_dir / "depth_m.npy", depth)
    np.save(output_dir / "solid_angle_sr.npy", solid_angle)
    np.save(output_dir / "glazing_depth_m.npy", glazing_depth)
    Image.fromarray(building.astype(np.uint8) * 255, mode="L").save(output_dir / "building_mask.png")
    Image.fromarray(greenery.astype(np.uint8) * 255, mode="L").save(output_dir / "greenery_mask.png")
    Image.fromarray(window_mask.astype(np.uint8) * 255, mode="L").save(output_dir / "window_mask.png")
    Image.fromarray(overlap.astype(np.uint8) * 255, mode="L").save(output_dir / "ambiguous_overlap_mask.png")
    clean_depth_preview = Image.fromarray(depth_visualization(depth, originally_valid_depth), mode="RGB")
    clean_depth_preview.save(output_dir / "depth_preview_clean.png")
    annotated_depth_visualization(depth, originally_valid_depth, "DA3 all-pixel debug distance").save(
        output_dir / "depth_preview_all.png"
    )
    segmented_valid = originally_valid_depth & (building | greenery)
    annotated_depth_visualization(depth, segmented_valid, "DA3 segmented-object distance").save(
        output_dir / "segmented_depth_preview.png"
    )
    annotated_depth_visualization(depth, originally_valid_depth & building, "DA3 building distance").save(
        output_dir / "building_depth_preview.png"
    )
    annotated_depth_visualization(depth, originally_valid_depth & greenery, "DA3 greenery distance").save(
        output_dir / "greenery_depth_preview.png"
    )
    segmentation_distance_overlay(
        preview,
        depth,
        originally_valid_depth,
        building,
        greenery,
    ).save(output_dir / "depth_preview.png")

    common_metadata = {
        "MERGEHDR_IMAGE_VOS_MODEL": args.model,
        "MERGEHDR_IMAGE_VOS_DEVICE": device,
        "MERGEHDR_IMAGE_VOS_PROJECTION": projection,
        "MERGEHDR_IMAGE_VOS_HORIZONTAL_FOV_DEG": f"{horizontal_fov:.9g}",
        "MERGEHDR_IMAGE_VOS_VERTICAL_FOV_DEG": f"{vertical_fov:.9g}",
        "MERGEHDR_IMAGE_VOS_MAX_DEPTH_M": f"{args.max_depth:.6f}",
        "MERGEHDR_IMAGE_VOS_WINDOW_DISTANCE_M": f"{args.window_distance:.6f}",
    }
    radiance_view = projection_metadata.get("view")
    if isinstance(radiance_view, str) and radiance_view.strip():
        common_metadata["VIEW"] = radiance_view.strip()
    depth_rgb = np.repeat(depth[..., None], 3, axis=-1)
    write_radiance_hdr(output_dir / "depth_m.hdr", depth_rgb, {**common_metadata, "MERGEHDR_CHANNELS": "depth_m depth_m depth_m"})
    packed = np.stack((building_vos_map, greenery_vos_map, depth), axis=-1)
    write_radiance_hdr(
        output_dir / "image_vos.hdr",
        packed,
        {**common_metadata, "MERGEHDR_CHANNELS": "building_vos greenery_vos radial_depth_m"},
    )

    clear_reference = getattr(args, "clear_reference", None)
    overlap_fraction = (
        float(overlap_pixels) / float(min(building_before_overlap, greenery_before_overlap))
        if min(building_before_overlap, greenery_before_overlap) > 0
        else 0.0
    )
    quality_warnings: list[str] = []
    if "_blind_" in original_input_path.name.casefold() and not clear_reference:
        quality_warnings.append(
            "Direct inference through Venetian blinds is not a valid VOS input; use a matching clear reference and assess the blind with View Clarity."
        )
    if overlap_fraction > 0.25:
        quality_warnings.append(
            "Building/greenery overlap exceeds 25% of the smaller category; review the SAM masks."
        )
    if args.window_distance <= 0.0:
        quality_warnings.append(
            "No glazing distance was supplied, so indoor cone volume was not subtracted from VOS-outside."
        )

    building_summary = category_summary(building, depth, solid_angle, building_vos_map)
    greenery_summary = category_summary(greenery, depth, solid_angle, greenery_vos_map)
    satisfaction = satisfaction_summary(
        building_summary,
        greenery_summary,
        args.satisfaction_building_coefficient,
        args.satisfaction_greenery_coefficient,
        args.satisfaction_intercept,
    )

    result = {
        "schema": "mergehdr.image_vos.v1",
        "experimental": True,
        "input": str(original_input_path),
        "inference_source": str(Path(getattr(args, "inference_source", None) or args.input).expanduser()),
        "clear_reference": clear_reference,
        "model": args.model,
        "runtime": "Aedelon MPS wrapper with official ByteDance DA3Metric-Large weights",
        "device": device,
        "projection": {
            "type": projection,
            "horizontal_fov_degrees": horizontal_fov,
            "vertical_fov_degrees": vertical_fov,
            "selection": "header" if args.projection == "auto" else "explicit",
            "metadata": projection_metadata,
            "solid_angle_total_sr": float(np.sum(solid_angle, dtype=np.float64)),
        },
        "resolution": {
            "source_mask_width": source_mask_width,
            "source_mask_height": source_mask_height,
            "output_width": target_width,
            "output_height": target_height,
        },
        "depth": {
            "minimum_m": args.min_depth,
            "maximum_m": args.max_depth,
            "clipped_below_pixels": int(np.count_nonzero(below)),
            "clipped_above_pixels": int(np.count_nonzero(above)),
            "covered_pixels": int(np.count_nonzero(originally_valid_depth)),
            "coverage_mean": float(np.mean(coverage[valid_view])) if np.any(valid_view) else 0.0,
            "inference": depth_inference_details,
        },
        "window_boundary": {
            "perpendicular_distance_m": args.window_distance,
            "model": "front-facing plane; radial intersection distance varies by ray angle",
            "mask": str(window_mask_path) if window_mask_path is not None else None,
            "mask_mode": "white pixels included" if window_mask_path is not None else "full valid VIEW",
            "included_pixels": int(np.count_nonzero(window_mask)),
            "included_solid_angle_sr": float(np.sum(solid_angle[window_mask], dtype=np.float64)),
            "pixels_where_object_is_not_beyond_glazing": int(
                np.count_nonzero(valid_view & originally_valid_depth & (depth <= glazing_depth))
            ),
        },
        "building": building_summary,
        "greenery": greenery_summary,
        "vos": {
            "unit": "m3",
            "V_built_m3": building_summary["vos_m3"],
            "V_greenery_m3": greenery_summary["vos_m3"],
            # Legacy aliases retained for compatibility.
            "V_built_m3_sr": building_summary["vos_m3_sr"],
            "V_greenery_m3_sr": greenery_summary["vos_m3_sr"],
        },
        "segmentation": {
            "candidate_policy": args.candidate_policy,
            "building_candidate_policy": args.building_candidate_policy or args.candidate_policy,
            "greenery_candidate_policy": args.greenery_candidate_policy or args.candidate_policy,
            "instance_min_score": args.instance_min_score,
            "building_pixels_before_overlap": building_before_overlap,
            "greenery_pixels_before_overlap": greenery_before_overlap,
            "ambiguous_overlap_pixels_excluded": overlap_pixels,
            "overlap_fraction_of_smaller_category": overlap_fraction,
        },
        "quality": {
            "status": "review_required" if quality_warnings else "ok",
            "warnings": quality_warnings,
        },
        "ambiguous_overlap_pixels_excluded": overlap_pixels,
        "mask_sources": {
            "building": building_sources,
            "greenery": greenery_sources,
            "window": str(window_mask_path) if window_mask_path is not None else None,
        },
        "formula": "VOS_outside_category = sum(mask * solid_angle_sr * max(object_depth_m^3 - glazing_depth_m^3, 0) / 3)",
        "limitations": [
            "Monocular metric depth is an estimate, not a surveyed distance.",
            (
                "Equidistant input is inferred through five rectilinear tiles and blended at overlaps."
                if projection == "equidistant"
                else "Perspective input is inferred directly and converted from axial to radial depth."
            ),
            "Depth clipping materially affects VOS because distance is cubed.",
            "The glazing boundary is approximated as a front-facing plane.",
            "Overlapping building/greenery segmentation is excluded from both category totals.",
        ],
        "outputs": {
            "packed_hdr": str(output_dir / "image_vos.hdr"),
            "depth_hdr": str(output_dir / "depth_m.hdr"),
            "window_mask": str(output_dir / "window_mask.png"),
            "building_mask": str(output_dir / "building_mask.png"),
            "greenery_mask": str(output_dir / "greenery_mask.png"),
            "segmentation_distance_overlay": str(output_dir / "depth_preview.png"),
            "segmented_depth_preview": str(output_dir / "segmented_depth_preview.png"),
            "building_depth_preview": str(output_dir / "building_depth_preview.png"),
            "greenery_depth_preview": str(output_dir / "greenery_depth_preview.png"),
            "all_pixel_depth_preview": str(output_dir / "depth_preview_all.png"),
            "depth_preview_clean": str(output_dir / "depth_preview_clean.png"),
        },
    }
    if satisfaction.get("status") == "calculated":
        result["view_satisfaction"] = satisfaction
    result_path = output_dir / "result.json"
    result_path.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    if getattr(args, "emit_result", True):
        print(json.dumps(result, ensure_ascii=False))
    return 0


def command_auto(args: argparse.Namespace) -> int:
    output_dir = Path(args.output_dir).expanduser()
    output_dir.mkdir(parents=True, exist_ok=True)
    original_input = Path(args.input).expanduser()
    inference_source = Path(args.clear_reference).expanduser() if args.clear_reference else original_input
    full_preview = load_display_rgb(inference_source)
    target_height, target_width = full_preview.shape[:2]
    preview = full_preview
    if args.preview_max_side > 0 and max(target_width, target_height) > args.preview_max_side:
        scale = float(args.preview_max_side) / float(max(target_width, target_height))
        preview_size = (
            max(1, int(round(target_width * scale))),
            max(1, int(round(target_height * scale))),
        )
        preview = np.asarray(
            Image.fromarray(preview, mode="RGB").resize(preview_size, Image.Resampling.LANCZOS),
            dtype=np.uint8,
        )
    preview_path = output_dir / "ai_preview.png"
    Image.fromarray(preview, mode="RGB").save(preview_path)

    sam_runtime = Path(args.sam_runtime_root).expanduser()
    sam_hf_home = Path(args.sam_hf_home).expanduser()
    building_candidates = output_dir / "sam_building"
    greenery_candidates = output_dir / "sam_greenery"
    run_sam_one_shot(
        sam_runtime,
        sam_hf_home,
        preview_path,
        building_candidates,
        args.building_prompt,
        target_width,
        target_height,
        args.sam_confidence,
    )
    run_sam_one_shot(
        sam_runtime,
        sam_hf_home,
        preview_path,
        greenery_candidates,
        args.greenery_prompt,
        target_width,
        target_height,
        args.sam_confidence,
    )
    analysis_args = argparse.Namespace(**vars(args))
    analysis_args.command = "analyze"
    analysis_args.input = str(preview_path)
    analysis_args.original_input = str(original_input)
    analysis_args.inference_source = str(inference_source)
    if analysis_args.window_distance is None:
        analysis_args.window_distance = inferred_window_distance(original_input)
    analysis_args.building_mask = []
    analysis_args.greenery_mask = []
    analysis_args.building_candidates = [str(building_candidates)]
    analysis_args.greenery_candidates = [str(greenery_candidates)]
    return command_analyze(analysis_args)


def projection_rays(
    width: int,
    height: int,
    projection: str,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray]:
    if projection == "equidistant":
        return equidistant_rays(width, height, horizontal_fov_deg, vertical_fov_deg)
    fx, fy = perspective_focal_lengths(width, height, horizontal_fov_deg, vertical_fov_deg)
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    x = (xx + 0.5 - 0.5 * width) / fx
    y = (yy + 0.5 - 0.5 * height) / fy
    rays = np.stack((x, y, np.ones_like(x)), axis=-1)
    rays /= np.linalg.norm(rays, axis=-1, keepdims=True)
    return rays.astype(np.float32), np.ones((height, width), dtype=bool)


def view_basis_from_result(result: dict[str, object]) -> tuple[np.ndarray, bool]:
    projection = result.get("projection")
    metadata = projection.get("metadata") if isinstance(projection, dict) else None
    view = metadata.get("view") if isinstance(metadata, dict) else None
    if not isinstance(view, str):
        return np.eye(3, dtype=np.float32), False

    def vector_after(flag: str) -> np.ndarray | None:
        match = re.search(
            rf"(?:^|\s){re.escape(flag)}\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)",
            view,
            flags=re.IGNORECASE,
        )
        if not match:
            return None
        return np.asarray([float(match.group(i)) for i in range(1, 4)], dtype=np.float64)

    forward = vector_after("-vd")
    up = vector_after("-vu")
    if forward is None or up is None:
        return np.eye(3, dtype=np.float32), False
    forward_norm = float(np.linalg.norm(forward))
    if forward_norm <= 1.0e-9:
        return np.eye(3, dtype=np.float32), False
    forward /= forward_norm
    up -= forward * float(np.dot(up, forward))
    up_norm = float(np.linalg.norm(up))
    if up_norm <= 1.0e-9:
        return np.eye(3, dtype=np.float32), False
    up /= up_norm
    right = np.cross(forward, up)
    right /= max(float(np.linalg.norm(right)), 1.0e-9)
    down = -up
    return np.stack((right, down, forward), axis=1).astype(np.float32), True


def project_local_rays(
    rays: np.ndarray,
    width: int,
    height: int,
    projection: str,
    horizontal_fov_deg: float,
    vertical_fov_deg: float,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    if projection == "equidistant":
        return project_rays_to_equidistant(
            rays,
            width,
            height,
            horizontal_fov_deg,
            vertical_fov_deg,
        )
    fx, fy = perspective_focal_lengths(width, height, horizontal_fov_deg, vertical_fov_deg)
    z = rays[..., 2]
    x_normalized = np.divide(rays[..., 0], z, out=np.zeros_like(z), where=z > 1.0e-7)
    y_normalized = np.divide(rays[..., 1], z, out=np.zeros_like(z), where=z > 1.0e-7)
    x = x_normalized * fx + 0.5 * width - 0.5
    y = y_normalized * fy + 0.5 * height - 0.5
    valid = (
        (z > 1.0e-7)
        & (x >= 0.0)
        & (x <= width - 1.0)
        & (y >= 0.0)
        & (y <= height - 1.0)
    )
    return x, y, valid


def result_projection(result: dict[str, object]) -> tuple[str, float, float, int, int]:
    projection = result.get("projection")
    resolution = result.get("resolution")
    if not isinstance(projection, dict) or not isinstance(resolution, dict):
        raise RuntimeError("Paired analysis result is missing projection or resolution metadata.")
    return (
        str(projection["type"]),
        float(projection["horizontal_fov_degrees"]),
        float(projection["vertical_fov_degrees"]),
        int(resolution["output_width"]),
        int(resolution["output_height"]),
    )


def aligned_external_reference(
    internal_result: dict[str, object],
    external_result: dict[str, object],
    external_dir: Path,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, np.ndarray, bool]:
    target_projection, target_hfov, target_vfov, target_width, target_height = result_projection(internal_result)
    source_projection, source_hfov, source_vfov, source_width, source_height = result_projection(external_result)
    target_local_rays, target_valid = projection_rays(
        target_width,
        target_height,
        target_projection,
        target_hfov,
        target_vfov,
    )
    target_basis, target_has_pose = view_basis_from_result(internal_result)
    source_basis, source_has_pose = view_basis_from_result(external_result)
    world_rays = np.einsum("...j,ij->...i", target_local_rays, target_basis)
    source_local_rays = np.einsum("...j,ji->...i", world_rays, source_basis)
    source_x, source_y, source_valid = project_local_rays(
        source_local_rays,
        source_width,
        source_height,
        source_projection,
        source_hfov,
        source_vfov,
    )
    valid = target_valid & source_valid
    source_depth = np.load(external_dir / "depth_m.npy")
    source_building = load_mask(external_dir / "building_mask.png")
    source_greenery = load_mask(external_dir / "greenery_mask.png")
    aligned_depth = bilinear_sample(source_depth.astype(np.float32), source_x, source_y)
    aligned_building = bilinear_sample(source_building.astype(np.float32), source_x, source_y) >= 0.5
    aligned_greenery = bilinear_sample(source_greenery.astype(np.float32), source_x, source_y) >= 0.5
    aligned_depth = np.where(valid & np.isfinite(aligned_depth) & (aligned_depth > 0.0), aligned_depth, 0.0).astype(np.float32)
    aligned_building &= valid
    aligned_greenery &= valid

    source_preview = load_display_rgb(external_dir / "ai_preview.png")
    preview_x = (source_x + 0.5) * source_preview.shape[1] / source_width - 0.5
    preview_y = (source_y + 0.5) * source_preview.shape[0] / source_height - 0.5
    aligned_preview = bilinear_sample(source_preview.astype(np.float32), preview_x, preview_y)
    aligned_preview[~valid] = 0.0
    aligned_preview = np.clip(np.rint(aligned_preview), 0, 255).astype(np.uint8)
    return (
        aligned_depth,
        aligned_building,
        aligned_greenery,
        aligned_preview,
        target_local_rays,
        target_has_pose and source_has_pose,
    )


def mask_iou(a: np.ndarray, b: np.ndarray) -> float | None:
    union = a | b
    count = int(np.count_nonzero(union))
    if count == 0:
        return None
    return float(np.count_nonzero(a & b)) / float(count)


def paired_depth_errors(
    internal_depth: np.ndarray,
    reference_depth: np.ndarray,
    valid: np.ndarray,
) -> dict[str, object]:
    selected = valid & np.isfinite(internal_depth) & np.isfinite(reference_depth) & (internal_depth > 0.0) & (reference_depth > 0.0)
    if not np.any(selected):
        return {
            "pixels": 0,
            "bias_m": None,
            "mae_m": None,
            "rmse_m": None,
            "median_absolute_error_m": None,
            "mean_absolute_percentage_error": None,
        }
    difference = internal_depth[selected].astype(np.float64) - reference_depth[selected].astype(np.float64)
    absolute = np.abs(difference)
    return {
        "pixels": int(difference.size),
        "bias_m": float(np.mean(difference)),
        "mae_m": float(np.mean(absolute)),
        "rmse_m": float(np.sqrt(np.mean(difference * difference))),
        "median_absolute_error_m": float(np.median(absolute)),
        "mean_absolute_percentage_error": float(np.mean(absolute / np.maximum(reference_depth[selected], 1.0e-6))) * 100.0,
    }


def paired_category_summary(
    category: str,
    internal_mask: np.ndarray,
    external_mask: np.ndarray,
    internal_depth: np.ndarray,
    external_raw_depth: np.ndarray,
    external_corrected_depth: np.ndarray,
    solid_angle: np.ndarray,
    glazing_depth: np.ndarray,
) -> dict[str, object]:
    internal_volume = solid_angle * np.maximum(internal_depth ** 3 - glazing_depth ** 3, 0.0) / 3.0
    external_raw_volume = solid_angle * np.maximum(external_raw_depth, 0.0) ** 3 / 3.0
    external_corrected_volume = solid_angle * np.maximum(external_corrected_depth ** 3 - glazing_depth ** 3, 0.0) / 3.0
    internal_summary = category_summary(internal_mask, internal_depth, solid_angle, np.where(internal_mask, internal_volume, 0.0))
    external_raw_summary = category_summary(external_mask, external_raw_depth, solid_angle, np.where(external_mask, external_raw_volume, 0.0))
    external_corrected_summary = category_summary(
        external_mask,
        external_corrected_depth,
        solid_angle,
        np.where(external_mask, external_corrected_volume, 0.0),
    )
    internal_vos = float(internal_summary["vos_m3_sr"])
    reference_vos = float(external_corrected_summary["vos_m3_sr"])
    difference = internal_vos - reference_vos
    overlap = internal_mask & external_mask
    return {
        "category": category,
        "internal": internal_summary,
        "external_raw_at_reference_camera": external_raw_summary,
        "external_corrected_to_interior_observer": external_corrected_summary,
        "segmentation_iou": mask_iou(internal_mask, external_mask),
        "depth_error_on_mask_intersection": paired_depth_errors(
            internal_depth,
            external_corrected_depth,
            overlap,
        ),
        "vos_error_m3_sr": difference,
        "vos_relative_error_percent": (difference / reference_vos * 100.0) if reference_vos > 0.0 else None,
    }


def estimate_window_distance_from_pair(
    internal_depth: np.ndarray,
    external_depth: np.ndarray,
    internal_rays: np.ndarray,
    internal_building: np.ndarray,
    internal_greenery: np.ndarray,
    external_building: np.ndarray,
    external_greenery: np.ndarray,
    window_mask: np.ndarray | None,
) -> tuple[float, dict[str, object]]:
    """Estimate forward observer translation, with an interior-plane fallback."""
    cosine = np.clip(internal_rays[..., 2].astype(np.float64), 1.0e-4, 1.0)
    internal = internal_depth.astype(np.float64)
    external = external_depth.astype(np.float64)
    paired_mask = (
        (internal > 0.0)
        & (external > 0.0)
        & (internal_building | internal_greenery)
        & (external_building | external_greenery)
    )
    discriminant = internal ** 2 - external ** 2 * np.maximum(1.0 - cosine ** 2, 0.0)
    paired_valid = paired_mask & (discriminant >= 0.0)
    paired_candidates = (
        -external[paired_valid] * cosine[paired_valid]
        + np.sqrt(np.maximum(discriminant[paired_valid], 0.0))
    )
    paired_candidates = paired_candidates[
        np.isfinite(paired_candidates) & (paired_candidates >= 0.10) & (paired_candidates <= 20.0)
    ]
    if paired_candidates.size >= 256:
        lower, upper = np.percentile(paired_candidates, (10.0, 90.0))
        trimmed = paired_candidates[(paired_candidates >= lower) & (paired_candidates <= upper)]
        estimate = float(np.median(trimmed))
        return estimate, {
            "method": "paired DA3 ray-translation median",
            "candidate_pixels": int(paired_candidates.size),
            "trimmed_pixels": int(trimmed.size),
        }

    axial_depth = internal * cosine
    valid_internal = np.isfinite(axial_depth) & (axial_depth >= 0.10) & (axial_depth <= 20.0)
    method = "interior DA3 foreground-plane median"
    fallback_mask = np.zeros_like(valid_internal)
    if window_mask is not None and np.any(window_mask):
        import cv2

        kernel_size = max(5, int(round(min(window_mask.shape) * 0.015)))
        if kernel_size % 2 == 0:
            kernel_size += 1
        kernel = np.ones((kernel_size, kernel_size), dtype=np.uint8)
        dilated = cv2.dilate(window_mask.astype(np.uint8), kernel, iterations=1) > 0
        fallback_mask = dilated & ~window_mask & valid_internal
        method = "interior DA3 window-boundary median"

    if np.count_nonzero(fallback_mask) < 256:
        internal_only = (
            (internal_building | internal_greenery)
            & ~(external_building | external_greenery)
            & valid_internal
        )
        if np.count_nonzero(internal_only) >= 256:
            fallback_mask = internal_only
            method = "interior-only segmented-plane median"

    if np.count_nonzero(fallback_mask) < 256:
        valid_values = axial_depth[valid_internal]
        if valid_values.size < 256:
            raise RuntimeError(
                "DA3 could not estimate the observer-to-window distance. Supply --window-distance explicitly."
            )
        cutoff = float(np.percentile(valid_values, 40.0))
        fallback_mask = valid_internal & (axial_depth <= cutoff)
        method = "interior DA3 near-depth median"

    values = axial_depth[fallback_mask]
    lower, upper = np.percentile(values, (10.0, 90.0))
    trimmed = values[(values >= lower) & (values <= upper)]
    estimate = float(np.median(trimmed))
    if not np.isfinite(estimate) or estimate <= 0.0:
        raise RuntimeError(
            "DA3 produced an invalid observer-to-window distance estimate. Supply --window-distance explicitly."
        )
    return estimate, {
        "method": method,
        "candidate_pixels": int(values.size),
        "trimmed_pixels": int(trimmed.size),
        "warning": "This is an image-based DA3 estimate; a measured perpendicular distance is preferred.",
    }


def command_pair(args: argparse.Namespace) -> int:
    pair_root = Path(args.output_dir).expanduser()
    internal_dir = pair_root / "internal"
    external_dir = pair_root / "external"
    pair_root.mkdir(parents=True, exist_ok=True)
    internal_input = Path(args.internal_input).expanduser()
    external_input = Path(args.external_input).expanduser()
    window_distance = args.window_distance
    distance_source = "explicit"
    distance_estimation: dict[str, object] | None = None
    if window_distance is None and not args.estimate_window_distance:
        inferred_distance = inferred_window_distance(internal_input)
        window_distance = inferred_distance if inferred_distance > 0.0 else 0.0
        distance_source = "interior filename" if inferred_distance > 0.0 else "unspecified"
    elif window_distance is None:
        distance_source = "DA3 interior estimate"
    if window_distance is not None and window_distance < 0.0:
        raise ValueError("--window-distance must be non-negative.")
    initial_internal_window_distance = float(window_distance) if window_distance is not None else 0.0

    for input_path, output_dir, analysis_window_distance, analysis_window_mask in (
        (internal_input, internal_dir, initial_internal_window_distance, args.window_mask),
        (external_input, external_dir, 0.0, None),
    ):
        single = argparse.Namespace(**vars(args))
        single.command = "auto"
        single.input = str(input_path)
        single.output_dir = str(output_dir)
        single.clear_reference = None
        single.window_distance = analysis_window_distance
        single.window_mask = analysis_window_mask
        single.emit_result = False
        # The pair command has its own concise final report. Keep SAM3/DA3
        # progress messages and per-image JSON out of the user-facing terminal.
        quiet_output = io.StringIO()
        with contextlib.redirect_stdout(quiet_output), contextlib.redirect_stderr(quiet_output):
            command_auto(single)

    internal_result = json.loads((internal_dir / "result.json").read_text(encoding="utf-8"))
    external_result = json.loads((external_dir / "result.json").read_text(encoding="utf-8"))
    (
        external_depth,
        external_building,
        external_greenery,
        external_preview,
        internal_rays,
        pose_metadata_available,
    ) = aligned_external_reference(internal_result, external_result, external_dir)
    target_projection, target_hfov, target_vfov, target_width, target_height = result_projection(internal_result)
    internal_depth = np.load(internal_dir / "depth_m.npy").astype(np.float32)
    internal_building = load_mask(internal_dir / "building_mask.png")
    internal_greenery = load_mask(internal_dir / "greenery_mask.png")
    if target_projection == "equidistant":
        solid_angle, target_valid = equidistant_solid_angle(target_width, target_height, target_hfov, target_vfov)
    else:
        solid_angle, target_valid = perspective_solid_angle(target_width, target_height, target_hfov, target_vfov)
    window_mask_path = Path(args.window_mask).expanduser() if args.window_mask else None
    window_mask = (
        resize_mask(load_mask(window_mask_path), (target_width, target_height))
        if window_mask_path is not None
        else target_valid.copy()
    )
    window_mask &= target_valid
    internal_building &= window_mask
    internal_greenery &= window_mask

    if window_distance is None:
        window_distance, distance_estimation = estimate_window_distance_from_pair(
            internal_depth,
            external_depth,
            internal_rays,
            internal_building,
            internal_greenery,
            external_building,
            external_greenery,
            window_mask if window_mask_path is not None else None,
        )
        print(
            f"Estimated observer-to-window distance: {window_distance:.4g} m "
            f"({distance_estimation['method']})",
            file=sys.stderr,
            flush=True,
        )

    cosine_to_forward = np.clip(internal_rays[..., 2], -1.0, 1.0)
    corrected_squared = (
        external_depth.astype(np.float64) ** 2
        + float(window_distance) ** 2
        + 2.0 * external_depth.astype(np.float64) * float(window_distance) * cosine_to_forward
    )
    external_corrected_depth = np.where(
        target_valid & (external_depth > 0.0),
        np.sqrt(np.maximum(corrected_squared, 0.0)),
        0.0,
    ).astype(np.float32)
    corrected_valid = target_valid & (external_corrected_depth > 0.0)
    external_building &= corrected_valid & window_mask
    external_greenery &= corrected_valid & window_mask
    corrected_overlap = external_building & external_greenery
    external_building &= ~corrected_overlap
    external_greenery &= ~corrected_overlap
    glazing_depth = window_radial_distance(
        target_width,
        target_height,
        target_projection,
        target_hfov,
        target_vfov,
        float(window_distance),
    )
    corrected_volume_factor = (
        solid_angle
        * np.maximum(external_corrected_depth ** 3 - glazing_depth ** 3, 0.0)
        / 3.0
    )
    corrected_building_vos = np.where(
        external_building,
        corrected_volume_factor,
        0.0,
    ).astype(np.float32)
    corrected_greenery_vos = np.where(
        external_greenery,
        corrected_volume_factor,
        0.0,
    ).astype(np.float32)
    building = paired_category_summary(
        "building",
        internal_building,
        external_building,
        internal_depth,
        external_depth,
        external_corrected_depth,
        solid_angle,
        glazing_depth,
    )
    greenery = paired_category_summary(
        "greenery",
        internal_greenery,
        external_greenery,
        internal_depth,
        external_depth,
        external_corrected_depth,
        solid_angle,
        glazing_depth,
    )
    reference_satisfaction = satisfaction_summary(
        building["external_corrected_to_interior_observer"],
        greenery["external_corrected_to_interior_observer"],
        args.satisfaction_building_coefficient,
        args.satisfaction_greenery_coefficient,
        args.satisfaction_intercept,
    )
    internal_satisfaction = satisfaction_summary(
        building["internal"],
        greenery["internal"],
        args.satisfaction_building_coefficient,
        args.satisfaction_greenery_coefficient,
        args.satisfaction_intercept,
    )
    internal_score = internal_satisfaction.get("score") if isinstance(internal_satisfaction, dict) else None
    reference_score = reference_satisfaction.get("score")

    Image.fromarray(external_preview, mode="RGB").save(pair_root / "external_aligned_preview.png")
    external_overlay = segmentation_distance_overlay(
        external_preview,
        external_corrected_depth,
        external_corrected_depth > 0.0,
        external_building,
        external_greenery,
    )
    external_overlay.save(pair_root / "external_reference_depth_preview.png")
    internal_overlay = Image.open(internal_dir / "depth_preview.png").convert("RGB")
    if internal_overlay.size != external_overlay.size:
        internal_overlay = internal_overlay.resize(external_overlay.size, Image.Resampling.LANCZOS)
    pair_overlay = Image.new(
        "RGB",
        (internal_overlay.width + external_overlay.width, external_overlay.height),
    )
    pair_overlay.paste(internal_overlay, (0, 0))
    pair_overlay.paste(external_overlay, (internal_overlay.width, 0))
    pair_overlay.save(pair_root / "pair_depth_comparison.png")

    np.save(pair_root / "corrected_depth_m.npy", external_corrected_depth)
    np.save(pair_root / "corrected_solid_angle_sr.npy", solid_angle)
    np.save(pair_root / "corrected_glazing_depth_m.npy", glazing_depth)
    Image.fromarray(external_building.astype(np.uint8) * 255, mode="L").save(
        pair_root / "corrected_building_mask.png"
    )
    Image.fromarray(external_greenery.astype(np.uint8) * 255, mode="L").save(
        pair_root / "corrected_greenery_mask.png"
    )
    Image.fromarray(window_mask.astype(np.uint8) * 255, mode="L").save(
        pair_root / "corrected_window_mask.png"
    )
    corrected_metadata = {
        "MERGEHDR_IMAGE_VOS_MODEL": args.model,
        "MERGEHDR_IMAGE_VOS_DEVICE": external_result.get("device", "mps"),
        "MERGEHDR_IMAGE_VOS_PROJECTION": target_projection,
        "MERGEHDR_IMAGE_VOS_HORIZONTAL_FOV_DEG": f"{target_hfov:.9g}",
        "MERGEHDR_IMAGE_VOS_VERTICAL_FOV_DEG": f"{target_vfov:.9g}",
        "MERGEHDR_IMAGE_VOS_MAX_DEPTH_M": f"{args.max_depth:.6f}",
        "MERGEHDR_IMAGE_VOS_WINDOW_DISTANCE_M": f"{window_distance:.6f}",
        "MERGEHDR_IMAGE_VOS_PAIR_REFERENCE": "external_reprojected_to_interior_observer",
    }
    internal_projection_metadata = internal_result.get("projection", {}).get("metadata", {})
    internal_radiance_view = (
        internal_projection_metadata.get("view")
        if isinstance(internal_projection_metadata, dict)
        else None
    )
    if isinstance(internal_radiance_view, str) and internal_radiance_view.strip():
        corrected_metadata["VIEW"] = internal_radiance_view.strip()
    corrected_depth_rgb = np.repeat(external_corrected_depth[..., None], 3, axis=-1)
    write_radiance_hdr(
        pair_root / "corrected_depth_m.hdr",
        corrected_depth_rgb,
        {**corrected_metadata, "MERGEHDR_CHANNELS": "depth_m depth_m depth_m"},
    )
    corrected_packed = np.stack(
        (corrected_building_vos, corrected_greenery_vos, external_corrected_depth),
        axis=-1,
    )
    write_radiance_hdr(
        pair_root / "corrected_image_vos.hdr",
        corrected_packed,
        {
            **corrected_metadata,
            "MERGEHDR_CHANNELS": "building_vos greenery_vos radial_depth_m",
        },
    )
    result = {
        "schema": "mergehdr.image_vos_pair.v1",
        "internal_input": str(internal_input),
        "external_input": str(external_input),
        # Kept for compatibility with earlier consumers.
        "window_distance_m": float(window_distance),
        "window_distance": {
            "perpendicular_distance_m": float(window_distance),
            "source": distance_source,
            "estimation": distance_estimation,
        },
        "window_mask": {
            "path": str(window_mask_path) if window_mask_path is not None else None,
            "mode": (
                "white pixels included; black pixels excluded"
                if window_mask_path is not None
                else "full valid VIEW (no --window-mask supplied)"
            ),
            "included_pixels": int(np.count_nonzero(window_mask)),
            "included_solid_angle_sr": float(np.sum(solid_angle[window_mask])),
        },
        "alignment": {
            "target_projection": target_projection,
            "target_horizontal_fov_degrees": target_hfov,
            "target_vertical_fov_degrees": target_vfov,
            "radiance_view_orientation_used": pose_metadata_available,
            "method": "External depth and masks are angularly reprojected to the internal view; observer distance uses a forward-axis camera translation.",
            "assumption": "The external camera is at the glazing plane and its view orientation is registered to the internal camera.",
        },
        "building": building,
        "greenery": greenery,
        "vos": {
            "reference": "external_corrected_to_interior_observer",
            "unit": "m3",
            "integration": "sum(mask * solid_angle * max(object_depth^3 - glazing_depth^3, 0) / 3)",
            "V_built_m3": building["external_corrected_to_interior_observer"]["vos_m3"],
            "V_greenery_m3": greenery["external_corrected_to_interior_observer"]["vos_m3"],
            # Legacy aliases retained for compatibility.
            "V_built_m3_sr": building["external_corrected_to_interior_observer"]["vos_m3_sr"],
            "V_greenery_m3_sr": greenery["external_corrected_to_interior_observer"]["vos_m3_sr"],
        },
        "outputs": {
            "internal_result": str(internal_dir / "result.json"),
            "external_result": str(external_dir / "result.json"),
            "external_aligned_preview": str(pair_root / "external_aligned_preview.png"),
            "external_reference_depth_preview": str(pair_root / "external_reference_depth_preview.png"),
            "pair_depth_comparison": str(pair_root / "pair_depth_comparison.png"),
            "corrected_packed_hdr": str(pair_root / "corrected_image_vos.hdr"),
            "corrected_depth_hdr": str(pair_root / "corrected_depth_m.hdr"),
            "corrected_window_mask": str(pair_root / "corrected_window_mask.png"),
            "corrected_building_mask": str(pair_root / "corrected_building_mask.png"),
            "corrected_greenery_mask": str(pair_root / "corrected_greenery_mask.png"),
        },
    }
    if reference_satisfaction.get("status") == "calculated":
        result["view_satisfaction"] = {
            "internal": internal_satisfaction,
            "external_corrected_to_interior_observer": reference_satisfaction,
            "error": (
                float(internal_score) - float(reference_score)
                if isinstance(internal_score, (int, float)) and isinstance(reference_score, (int, float))
                else None
            ),
        }
    (pair_root / "pair_comparison.json").write_text(
        json.dumps(result, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    internal_overlap = float(
        internal_result.get("segmentation", {}).get("overlap_fraction_of_smaller_category", 0.0)
    )
    print("Image VOS completed")
    print(f"V_built:    {result['vos']['V_built_m3']:.3f} m³")
    print(f"V_greenery: {result['vos']['V_greenery_m3']:.3f} m³")
    if internal_overlap > 0.25:
        print(f"Warning: internal building/greenery overlap = {internal_overlap * 100.0:.2f}%")
    print(f"Results: {pair_root / 'pair_comparison.json'}")
    print(f"Preview: {pair_root / 'pair_depth_comparison.png'}")
    return 0


def main() -> int:
    args = parse_args()
    if args.command == "check":
        return command_check(args)
    if args.command == "download":
        return command_download(args)
    if args.command == "analyze":
        return command_analyze(args)
    if args.command == "auto":
        return command_auto(args)
    if args.command == "pair":
        return command_pair(args)
    raise RuntimeError(f"Unknown command: {args.command}")


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        sys.exit(1)
