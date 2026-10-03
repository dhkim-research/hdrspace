#!/usr/bin/env python3

import argparse
import contextlib
import io
import json
import hashlib
import shutil
import sys
from pathlib import Path

import numpy as np
from PIL import Image

from sam3.model_builder import build_sam3_image_model
from sam3.model.sam3_image_processor import Sam3Processor
import mlx.core as mx
from sam3.model.geometry_encoders import concat_padded_sequences


def parse_args():
    parser = argparse.ArgumentParser(description="Generate candidate masks with MLX SAM3.")
    parser.add_argument("--server", action="store_true", help="Run as a persistent stdin/stdout server")
    parser.add_argument("--weights-dir", default="", help="Directory containing model.safetensors and index json")
    parser.add_argument("--input", help="Input RGB preview image path")
    parser.add_argument("--output-dir", help="Directory where mask_*.png files will be written")
    parser.add_argument("--prompt", default="", help="Optional text prompt")
    parser.add_argument("--positive-point", action="append", default=[], help="Positive point in original image coords: x,y")
    parser.add_argument("--negative-point", action="append", default=[], help="Negative point in original image coords: x,y")
    parser.add_argument("--target-width", type=int, help="Original full-resolution image width")
    parser.add_argument("--target-height", type=int, help="Original full-resolution image height")
    parser.add_argument("--box-size", type=int, default=22, help="Square box prompt size in preview pixels")
    parser.add_argument("--confidence-threshold", type=float, default=0.5, help="Detection confidence threshold")
    args = parser.parse_args()

    if not args.server:
        missing = []
        if not args.input:
            missing.append("--input")
        if not args.output_dir:
            missing.append("--output-dir")
        if args.target_width is None:
            missing.append("--target-width")
        if args.target_height is None:
            missing.append("--target-height")
        if missing:
            parser.error("Missing required arguments for one-shot mode: " + ", ".join(missing))

    return args


def sanitize_status_text(value: str) -> str:
    return value.replace("\t", " ").replace("\r", " ").replace("\n", " ").strip()


def split_prompt_variants(prompt: str) -> list[str]:
    raw_parts = prompt.replace("\n", ",").replace(";", ",").split(",")
    seen = set()
    variants: list[str] = []
    for part in raw_parts:
        text = part.strip()
        if not text:
            continue
        key = text.casefold()
        if key in seen:
            continue
        seen.add(key)
        variants.append(text)
    if not variants and prompt.strip():
        variants.append(prompt.strip())
    return variants


def parse_point(value: str) -> tuple[float, float]:
    if "," not in value:
        raise ValueError(f"Point must be x,y: {value}")
    x_text, y_text = value.split(",", 1)
    return float(x_text.strip()), float(y_text.strip())


def load_image_rgb(input_path: Path) -> Image.Image:
    return Image.open(input_path).convert("RGB")


def set_image_quiet(processor: Sam3Processor, image: Image.Image) -> dict:
    with contextlib.redirect_stdout(io.StringIO()):
        return processor.set_image(image)


def clone_state_template(state_template: dict) -> dict:
    return {
        "original_height": state_template["original_height"],
        "original_width": state_template["original_width"],
        "backbone_out": dict(state_template["backbone_out"]),
    }


def point_to_normalized_box(
    point_xy: tuple[float, float],
    original_width: int,
    original_height: int,
    preview_width: int,
    preview_height: int,
    box_size: int,
) -> list[float]:
    scale_x = float(preview_width) / float(max(1, original_width))
    scale_y = float(preview_height) / float(max(1, original_height))
    center_x = float(point_xy[0]) * scale_x
    center_y = float(point_xy[1]) * scale_y
    side = max(2.0, float(box_size))
    half = side * 0.5

    left = max(0.0, min(float(preview_width - 1), center_x - half))
    top = max(0.0, min(float(preview_height - 1), center_y - half))
    width = max(1.0, min(side, float(preview_width) - left))
    height = max(1.0, min(side, float(preview_height) - top))

    box_center_x = left + width * 0.5
    box_center_y = top + height * 0.5
    return [
        box_center_x / float(preview_width),
        box_center_y / float(preview_height),
        width / float(preview_width),
        height / float(preview_height),
    ]


def point_to_normalized_point(
    point_xy: tuple[float, float],
    original_width: int,
    original_height: int,
) -> list[float]:
    return [
        float(point_xy[0]) / float(max(1, original_width)),
        float(point_xy[1]) / float(max(1, original_height)),
    ]


def append_point_prompt(processor: Sam3Processor, state: dict, point_xy: tuple[float, float], label: bool) -> dict:
    if "backbone_out" not in state:
        raise ValueError("You must call set_image before set_text_prompt")

    if "language_features" not in state["backbone_out"]:
        dummy_text_outputs = processor.model.backbone.call_text(["visual"])
        state["backbone_out"].update(dummy_text_outputs)

    if "geometric_prompt" not in state:
        state["geometric_prompt"] = processor.model._get_dummy_prompt()

    prompt = state["geometric_prompt"]
    point = mx.array(point_xy, dtype=mx.float32).reshape(1, 1, 2)
    point_labels = mx.array([label], dtype=mx.bool_).reshape(1, 1)
    point_mask = mx.zeros((1, 1), dtype=mx.bool_)

    if prompt.point_embeddings is None:
        prompt.point_embeddings = point
        prompt.point_labels = point_labels
        prompt.point_mask = point_mask
    else:
        prompt.point_labels, _ = concat_padded_sequences(
            prompt.point_labels[..., None],
            prompt.point_mask,
            point_labels[..., None],
            point_mask,
        )
        prompt.point_labels = prompt.point_labels.squeeze(-1)
        prompt.point_embeddings, prompt.point_mask = concat_padded_sequences(
            prompt.point_embeddings,
            prompt.point_mask,
            point,
            point_mask,
        )

    return processor._call_grounding(state)


def prepare_binary_mask(mask_2d: np.ndarray, target_width: int, target_height: int) -> np.ndarray | None:
    binary = np.asarray(mask_2d) > 0
    if binary.ndim == 3:
        binary = binary[0]
    if binary.ndim != 2:
        return None
    if not np.any(binary):
        return None

    image = Image.fromarray(binary.astype(np.uint8) * 255, mode="L")
    if image.size != (target_width, target_height):
        image = image.resize((target_width, target_height), Image.Resampling.NEAREST)
    return np.asarray(image) > 127


def write_binary_mask(binary_mask: np.ndarray, output_path: Path) -> bool:
    binary = np.asarray(binary_mask) > 0
    if binary.ndim != 2 or not np.any(binary):
        return False
    Image.fromarray(binary.astype(np.uint8) * 255, mode="L").save(output_path)
    return True


def collect_candidates_from_state(state: dict, include_semantic: bool, prompt_variant: str) -> list[dict]:
    target_width = int(state["original_width"])
    target_height = int(state["original_height"])
    candidates: list[dict] = []

    if include_semantic and "semantic_seg" in state:
        semantic = np.asarray(state["semantic_seg"])
        if semantic.ndim == 4:
            semantic = semantic[0, 0]
        elif semantic.ndim == 3:
            semantic = semantic[0]
        semantic_probs = 1.0 / (1.0 + np.exp(-np.clip(semantic, -80.0, 80.0)))
        prepared = prepare_binary_mask(semantic_probs > 0.5, target_width, target_height)
        if prepared is not None:
            candidates.append({
                "kind": "semantic",
                "score": 1.0,
                "prompt": prompt_variant,
                "mask": prepared,
            })

    masks = np.asarray(state.get("masks", []))
    scores = np.asarray(state.get("scores", []), dtype=np.float32).reshape(-1)
    if masks.ndim == 4:
        if masks.shape[1] == 1:
            masks = masks[:, 0]
    elif masks.ndim == 3:
        pass
    elif masks.ndim == 2:
        masks = masks[np.newaxis, ...]
    else:
        masks = np.empty((0, target_height, target_width), dtype=bool)

    order = list(range(masks.shape[0]))
    if scores.size == len(order):
        order.sort(key=lambda idx: float(scores[idx]), reverse=True)

    for idx in order:
        prepared = prepare_binary_mask(masks[idx], target_width, target_height)
        if prepared is None:
            continue
        score = float(scores[idx]) if idx < scores.size else 0.0
        candidates.append({
            "kind": "instance",
            "score": score,
            "prompt": prompt_variant,
            "mask": prepared,
        })

    return candidates


def dedupe_candidates(candidates: list[dict]) -> list[dict]:
    deduped: list[dict] = []
    seen: dict[str, int] = {}
    for candidate in candidates:
        mask = np.asarray(candidate["mask"]) > 0
        key = hashlib.sha1(mask.astype(np.uint8).tobytes()).hexdigest()
        previous_index = seen.get(key)
        if previous_index is None:
            seen[key] = len(deduped)
            deduped.append(candidate)
            continue

        previous = deduped[previous_index]
        previous_score = float(previous.get("score", 0.0))
        current_score = float(candidate.get("score", 0.0))
        previous_kind = str(previous.get("kind", "instance"))
        current_kind = str(candidate.get("kind", "instance"))
        replace = (current_kind == "instance" and previous_kind != "instance") or (current_score > previous_score)
        if replace:
            deduped[previous_index] = candidate
    return deduped


def save_candidates(candidates: list[dict], output_dir: Path) -> int:
    if output_dir.exists():
        shutil.rmtree(output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    saved = 0
    metadata_lines = ["filename\tkind\tscore\tactive_pixels\tprompt"]
    for candidate in candidates:
        filename = f"mask_{saved:03d}.png"
        binary = np.asarray(candidate["mask"]) > 0
        if not write_binary_mask(binary, output_dir / filename):
            continue
        metadata_lines.append(
            f"{filename}\t{candidate.get('kind', 'instance')}\t"
            f"{float(candidate.get('score', 0.0)):.6f}\t{int(binary.sum())}\t"
            f"{sanitize_status_text(str(candidate.get('prompt', '')))}"
        )
        saved += 1

    (output_dir / "candidates.tsv").write_text("\n".join(metadata_lines) + "\n", encoding="utf-8")
    return saved


def run_prediction(
    processor: Sam3Processor,
    state_template: dict,
    preview_size: tuple[int, int],
    prompt: str,
    positive_points: list[tuple[float, float]],
    negative_points: list[tuple[float, float]],
    box_size: int,
    confidence_threshold: float,
) -> dict:
    processor.confidence_threshold = float(confidence_threshold)
    state = clone_state_template(state_template)

    prompt = prompt.strip()
    if prompt:
        state = processor.set_text_prompt(prompt, state)

    for point in positive_points:
        normalized_point = point_to_normalized_point(
            point,
            original_width=state["original_width"],
            original_height=state["original_height"],
        )
        state = append_point_prompt(processor, state, normalized_point, True)

    for point in negative_points:
        normalized_point = point_to_normalized_point(
            point,
            original_width=state["original_width"],
            original_height=state["original_height"],
        )
        state = append_point_prompt(processor, state, normalized_point, False)

    return state


def run_prompt_variants(
    processor: Sam3Processor,
    state_template: dict,
    preview_size: tuple[int, int],
    prompt: str,
    positive_points: list[tuple[float, float]],
    negative_points: list[tuple[float, float]],
    box_size: int,
    confidence_threshold: float,
) -> list[dict]:
    prompt_variants = split_prompt_variants(prompt)
    if not prompt_variants:
        prompt_variants = [""]

    include_semantic = bool(prompt_variants[0].strip() or positive_points or negative_points)
    all_candidates: list[dict] = []
    for prompt_variant in prompt_variants:
        state = run_prediction(
            processor,
            state_template,
            preview_size,
            prompt_variant,
            positive_points,
            negative_points,
            box_size,
            confidence_threshold,
        )
        all_candidates.extend(
            collect_candidates_from_state(
                state,
                include_semantic=include_semantic,
                prompt_variant=prompt_variant,
            )
        )
    return dedupe_candidates(all_candidates)


def run_one_shot(args) -> int:
    weights_dir = args.weights_dir or None
    model = build_sam3_image_model(local_weights_dir=weights_dir)
    processor = Sam3Processor(model, confidence_threshold=args.confidence_threshold)

    input_path = Path(args.input)
    output_dir = Path(args.output_dir)
    image = load_image_rgb(input_path)
    preview_width, preview_height = image.size
    state_template = set_image_quiet(processor, image)
    # One-shot inference may use a reduced preview, but masks must be exported
    # in the caller's original HDR dimensions just like server mode does.
    if args.target_width > 0 and args.target_height > 0:
        state_template["original_width"] = args.target_width
        state_template["original_height"] = args.target_height

    positive_points = [parse_point(v) for v in args.positive_point]
    negative_points = [parse_point(v) for v in args.negative_point]
    if not args.prompt.strip() and not positive_points:
        raise SystemExit("Provide a prompt or at least one positive point.")

    candidates = run_prompt_variants(
        processor,
        state_template,
        (preview_width, preview_height),
        args.prompt,
        positive_points,
        negative_points,
        args.box_size,
        args.confidence_threshold,
    )
    count = save_candidates(candidates, output_dir)
    print(f"saved_candidates={count}")
    return 0


def run_server(args) -> int:
    weights_dir = args.weights_dir or None
    model = build_sam3_image_model(local_weights_dir=weights_dir)
    processor = Sam3Processor(model, confidence_threshold=args.confidence_threshold)

    cached_key = None
    cached_state_template = None
    cached_preview_size = (0, 0)

    print("READY", flush=True)

    for raw_line in sys.stdin:
        line = raw_line.strip()
        if not line:
            continue

        try:
            request = json.loads(line)
            command = request.get("command", "predict")
            if command == "quit":
                print("OK\tbye", flush=True)
                break
            if command != "predict":
                raise ValueError(f"Unknown command: {command}")

            input_path = Path(request["input"])
            output_dir = Path(request["output_dir"])
            prompt = str(request.get("prompt", ""))
            target_width = int(request["target_width"])
            target_height = int(request["target_height"])
            box_size = int(request.get("box_size", 22))
            confidence_threshold = float(request.get("confidence_threshold", 0.5))
            positive_points = [tuple(map(float, point)) for point in request.get("positive_points", [])]
            negative_points = [tuple(map(float, point)) for point in request.get("negative_points", [])]

            if not prompt.strip() and not positive_points:
                raise ValueError("Provide a prompt or at least one positive point.")

            stat = input_path.stat()
            image_key = (str(input_path.resolve()), int(stat.st_mtime_ns), int(stat.st_size))
            if image_key != cached_key:
                image = load_image_rgb(input_path)
                if image.size[0] <= 0 or image.size[1] <= 0:
                    raise ValueError("Input image is empty.")
                cached_preview_size = image.size
                cached_state_template = set_image_quiet(processor, image)
                cached_key = image_key

            state_template = clone_state_template(cached_state_template)
            state_template["original_width"] = target_width
            state_template["original_height"] = target_height
            candidates = run_prompt_variants(
                processor,
                state_template,
                cached_preview_size,
                prompt,
                positive_points,
                negative_points,
                box_size,
                confidence_threshold,
            )
            count = save_candidates(candidates, output_dir)
            print(f"OK\t{count}", flush=True)
        except Exception as exc:
            print(f"ERR\t{sanitize_status_text(str(exc))}", flush=True)

    return 0


def main():
    args = parse_args()
    if args.server:
        return run_server(args)
    return run_one_shot(args)


if __name__ == "__main__":
    sys.exit(main())
