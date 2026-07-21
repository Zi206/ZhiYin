#!/usr/bin/env python3
"""Convert puppy GIF sources into indexed JPEG animation containers.

The converter streams no temporary image sequence to disk: FFmpeg emits optical-
flow-interpolated RGB frames through stdout, Pillow encodes each frame as JPEG,
and the resulting frames are packed into the little-endian ZHMJ v1 container.
"""

from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import shutil
import statistics
import struct
import subprocess
import sys

import cv2
import numpy as np
from PIL import Image, ImageDraw


MAGIC = b"ZHMJ"
VERSION = 1
HEADER = struct.Struct("<4sHHHHHHIIII")
HEADER_SIZE = HEADER.size
EXPECTED_SIZE = (240, 320)
TARGET_CENTER_X = 120.0
TARGET_BASELINE_Y = 288.0
SAFE_BOUNDS = (12, 16, 228, 304)
BACKGROUND_THRESHOLD = 34


def ffmpeg_path(explicit: str | None) -> str:
    candidate = explicit or shutil.which("ffmpeg")
    if not candidate:
        raise RuntimeError("未找到 FFmpeg；请安装 FFmpeg 或通过 --ffmpeg 指定路径")
    return candidate


def interpolate_frames(source: Path, ffmpeg: str, fps: int) -> tuple[tuple[int, int], list[bytes]]:
    with Image.open(source) as gif:
        width, height = gif.size
    if (width, height) != EXPECTED_SIZE:
        raise ValueError(f"{source.name}: 期望 {EXPECTED_SIZE[0]}x{EXPECTED_SIZE[1]}，实际 {width}x{height}")

    graph = (
        f"[0:v]format=rgba[fg];"
        f"color=c=black:s={width}x{height}:r={fps}[bg];"
        f"[bg][fg]overlay=shortest=1:format=auto,"
        f"minterpolate=fps={fps}:mi_mode=mci:mc_mode=aobmc:me_mode=bidir:vsbmc=1,"
        "format=rgb24[out]"
    )
    command = [
        ffmpeg,
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(source),
        "-filter_complex",
        graph,
        "-map",
        "[out]",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    ]
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        detail = result.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"{source.name}: FFmpeg 插帧失败: {detail}")

    frame_size = width * height * 3
    if not result.stdout or len(result.stdout) % frame_size != 0:
        raise RuntimeError(f"{source.name}: FFmpeg 输出长度不是完整 RGB 帧")
    frames = [
        result.stdout[offset : offset + frame_size]
        for offset in range(0, len(result.stdout), frame_size)
    ]
    if len(frames) < 2:
        raise RuntimeError(f"{source.name}: 插帧后只有 {len(frames)} 帧")
    return (width, height), frames


def apply_warm_filter(image: Image.Image) -> Image.Image:
    """Preserve the R+8% / B-8% look used by the previous GIF renderer."""
    red, green, blue = image.split()
    red = red.point([min(255, (value * 276) >> 8) for value in range(256)])
    blue = blue.point([(value * 235) >> 8 for value in range(256)])
    return Image.merge("RGB", (red, green, blue))


def edge_background(array: np.ndarray) -> np.ndarray:
    border = np.concatenate((array[0], array[-1], array[:, 0], array[:, -1]), axis=0)
    return np.median(border, axis=0)


def subject_bounds(image: Image.Image) -> tuple[int, int, int, int]:
    """Find the puppy with edge-background difference + largest component."""
    array = np.asarray(image, dtype=np.uint8)
    background = edge_background(array)
    difference = np.max(np.abs(array.astype(np.int16) - background.astype(np.int16)), axis=2)
    mask = (difference >= BACKGROUND_THRESHOLD).astype(np.uint8)
    kernel = np.ones((3, 3), dtype=np.uint8)
    mask = cv2.morphologyEx(mask, cv2.MORPH_CLOSE, kernel, iterations=1)
    count, _labels, stats, _centroids = cv2.connectedComponentsWithStats(mask, connectivity=8)
    if count <= 1:
        raise RuntimeError("无法从边缘背景中提取动画主体")
    largest = 1 + int(np.argmax(stats[1:, cv2.CC_STAT_AREA]))
    x = int(stats[largest, cv2.CC_STAT_LEFT])
    y = int(stats[largest, cv2.CC_STAT_TOP])
    width = int(stats[largest, cv2.CC_STAT_WIDTH])
    height = int(stats[largest, cv2.CC_STAT_HEIGHT])
    if width < 24 or height < 24:
        raise RuntimeError(f"提取到的主体过小: {width}x{height}")
    return x, y, x + width - 1, y + height - 1


def fixed_alignment(rgb_frames: list[bytes], size: tuple[int, int]) -> tuple[list[Image.Image], dict[str, object]]:
    source_images = [Image.frombytes("RGB", size, frame) for frame in rgb_frames]
    source_bounds = [subject_bounds(image) for image in source_images]
    centers = [(left + right) / 2.0 for left, _top, right, _bottom in source_bounds]
    baselines = [float(bottom) for _left, _top, _right, bottom in source_bounds]
    anchor_x = float(statistics.median(centers))
    anchor_y = float(statistics.median(baselines))

    union_left = min(bounds[0] for bounds in source_bounds)
    union_top = min(bounds[1] for bounds in source_bounds)
    union_right = max(bounds[2] for bounds in source_bounds)
    union_bottom = max(bounds[3] for bounds in source_bounds)
    safe_left, safe_top, safe_right, safe_bottom = SAFE_BOUNDS
    # Reserve a small rasterization margin because bicubic resampling and the
    # post-transform mask can expand a connected component by 1-2 pixels.
    fit_left, fit_top = safe_left + 3, safe_top + 3
    fit_right, fit_bottom = safe_right - 3, safe_bottom - 3

    limits = [1.0]
    if union_left < anchor_x:
        limits.append((TARGET_CENTER_X - fit_left) / (anchor_x - union_left))
    if union_right > anchor_x:
        limits.append((fit_right - TARGET_CENTER_X) / (union_right - anchor_x))
    if union_top < anchor_y:
        limits.append((TARGET_BASELINE_Y - fit_top) / (anchor_y - union_top))
    if union_bottom > anchor_y:
        limits.append((fit_bottom - TARGET_BASELINE_Y) / (union_bottom - anchor_y))
    scale = max(0.1, min(limits))

    inverse = (
        1.0 / scale,
        0.0,
        anchor_x - TARGET_CENTER_X / scale,
        0.0,
        1.0 / scale,
        anchor_y - TARGET_BASELINE_Y / scale,
    )
    aligned: list[Image.Image] = []
    for image in source_images:
        background = tuple(int(round(value)) for value in edge_background(np.asarray(image)))
        aligned.append(image.transform(
            EXPECTED_SIZE,
            Image.Transform.AFFINE,
            inverse,
            resample=Image.Resampling.BICUBIC,
            fillcolor=background,
        ))

    aligned_bounds = [subject_bounds(image) for image in aligned]
    aligned_centers = [(left + right) / 2.0 for left, _top, right, _bottom in aligned_bounds]
    aligned_baselines = [float(bottom) for _left, _top, _right, bottom in aligned_bounds]
    median_center = float(statistics.median(aligned_centers))
    median_baseline = float(statistics.median(aligned_baselines))
    if abs(median_center - TARGET_CENTER_X) > 3 or abs(median_baseline - TARGET_BASELINE_Y) > 3:
        raise RuntimeError(
            f"对齐误差超限: center={median_center:.1f}, baseline={median_baseline:.1f}"
        )
    aligned_union = (
        min(item[0] for item in aligned_bounds),
        min(item[1] for item in aligned_bounds),
        max(item[2] for item in aligned_bounds),
        max(item[3] for item in aligned_bounds),
    )
    if (aligned_union[0] < safe_left - 1 or aligned_union[1] < safe_top - 1 or
            aligned_union[2] > safe_right + 1 or aligned_union[3] > safe_bottom + 1):
        raise RuntimeError(f"固定变换后的主体超出安全区: {aligned_union}")

    transform = {
        "source_anchor": {"center_x": round(anchor_x, 3), "baseline_y": round(anchor_y, 3)},
        "target_anchor": {"center_x": TARGET_CENTER_X, "baseline_y": TARGET_BASELINE_Y},
        "scale": round(scale, 6),
        "translation": {
            "x": round(TARGET_CENTER_X - scale * anchor_x, 3),
            "y": round(TARGET_BASELINE_Y - scale * anchor_y, 3),
        },
        "source_union_bounds": [union_left, union_top, union_right, union_bottom],
        "aligned_union_bounds": list(aligned_union),
        "median_result": {
            "center_x": round(median_center, 3),
            "baseline_y": round(median_baseline, 3),
            "center_error_px": round(median_center - TARGET_CENTER_X, 3),
            "baseline_error_px": round(median_baseline - TARGET_BASELINE_Y, 3),
        },
        "max_frame_deviation": {
            "center_px": round(max(abs(value - median_center) for value in aligned_centers), 3),
            "baseline_px": round(max(abs(value - median_baseline) for value in aligned_baselines), 3),
        },
    }
    return aligned, transform


def encode_jpeg(image: Image.Image, quality: int) -> bytes:
    image = apply_warm_filter(image)
    output = io.BytesIO()
    image.save(
        output,
        format="JPEG",
        quality=quality,
        subsampling=2,
        optimize=True,
        progressive=False,
    )
    encoded = output.getvalue()
    if not encoded.startswith(b"\xff\xd8") or not encoded.endswith(b"\xff\xd9"):
        raise RuntimeError("Pillow 生成了无效 JPEG 帧")
    return encoded


def pack_container(size: tuple[int, int], fps: int, frames: list[bytes]) -> bytes:
    offsets = [0]
    for frame in frames:
        offsets.append(offsets[-1] + len(frame))

    index_offset = HEADER_SIZE
    raw_index_size = len(offsets) * 4
    frame_data_offset = (index_offset + raw_index_size + 3) & ~3
    header = HEADER.pack(
        MAGIC,
        VERSION,
        HEADER_SIZE,
        size[0],
        size[1],
        fps,
        0,
        len(frames),
        index_offset,
        frame_data_offset,
        0,
    )
    index = struct.pack(f"<{len(offsets)}I", *offsets)
    padding = b"\0" * (frame_data_offset - len(header) - len(index))
    return header + index + padding + b"".join(frames)


def parse_container(path: Path, verify_jpegs: bool = True) -> dict[str, int | str]:
    payload = path.read_bytes()
    if len(payload) < HEADER_SIZE:
        raise ValueError(f"{path.name}: 文件过短")
    (
        magic,
        version,
        header_size,
        width,
        height,
        fps,
        _reserved,
        frame_count,
        index_offset,
        frame_data_offset,
        _flags,
    ) = HEADER.unpack_from(payload)
    if magic != MAGIC or version != VERSION or header_size != HEADER_SIZE:
        raise ValueError(f"{path.name}: ZHMJ 头无效")
    if (width, height) != EXPECTED_SIZE or not (1 <= fps <= 60) or frame_count < 2:
        raise ValueError(f"{path.name}: 动画参数无效")
    index_end = index_offset + (frame_count + 1) * 4
    if index_offset < header_size or index_end > frame_data_offset or frame_data_offset > len(payload):
        raise ValueError(f"{path.name}: 索引范围无效")
    offsets = struct.unpack_from(f"<{frame_count + 1}I", payload, index_offset)
    if offsets[0] != 0 or any(b <= a for a, b in zip(offsets, offsets[1:])):
        raise ValueError(f"{path.name}: 帧偏移无效")
    if frame_data_offset + offsets[-1] != len(payload):
        raise ValueError(f"{path.name}: 帧数据长度不匹配")

    if verify_jpegs:
        for frame_index, (begin, end) in enumerate(zip(offsets, offsets[1:])):
            encoded = payload[frame_data_offset + begin : frame_data_offset + end]
            if not encoded.startswith(b"\xff\xd8") or not encoded.endswith(b"\xff\xd9"):
                raise ValueError(f"{path.name}: 第 {frame_index} 帧 JPEG 边界无效")
            with Image.open(io.BytesIO(encoded)) as frame:
                if frame.size != (width, height) or frame.format != "JPEG":
                    raise ValueError(f"{path.name}: 第 {frame_index} 帧尺寸或格式无效")
                frame.verify()

    return {
        "width": width,
        "height": height,
        "fps": fps,
        "frames": frame_count,
        "bytes": len(payload),
        "sha256": hashlib.sha256(payload).hexdigest(),
    }


def convert_one(source: Path, destination: Path, ffmpeg: str, fps: int,
                quality: int) -> tuple[dict[str, int | str], dict[str, object], Image.Image]:
    size, rgb_frames = interpolate_frames(source, ffmpeg, fps)
    aligned_frames, alignment = fixed_alignment(rgb_frames, size)
    jpeg_frames = [encode_jpeg(frame, quality) for frame in aligned_frames]
    container = pack_container(size, fps, jpeg_frames)

    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".tmp")
    try:
        temporary.write_bytes(container)
        temporary.replace(destination)
    finally:
        temporary.unlink(missing_ok=True)
    return parse_container(destination), alignment, aligned_frames[len(aligned_frames) // 2]


def write_alignment_report(report_dir: Path, entries: list[dict[str, object]],
                           previews: list[tuple[str, Image.Image]]) -> None:
    report_dir.mkdir(parents=True, exist_ok=True)
    manifest = {
        "schema": "zhiyin-expression-alignment-v1",
        "target_size": list(EXPECTED_SIZE),
        "target_center_x": TARGET_CENTER_X,
        "target_baseline_y": TARGET_BASELINE_Y,
        "safe_bounds": list(SAFE_BOUNDS),
        "background_threshold": BACKGROUND_THRESHOLD,
        "animations": entries,
    }
    (report_dir / "alignment_manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
    )

    cell_width, cell_height = EXPECTED_SIZE
    caption_height = 28
    sheet = Image.new("RGB", (cell_width * 3, (cell_height + caption_height) * 3), "black")
    draw = ImageDraw.Draw(sheet)
    for index, (name, preview) in enumerate(previews):
        col, row = index % 3, index // 3
        x, y = col * cell_width, row * (cell_height + caption_height)
        sheet.paste(preview, (x, y))
        draw.line((x + int(TARGET_CENTER_X), y, x + int(TARGET_CENTER_X), y + cell_height), fill=(0, 255, 255), width=1)
        draw.line((x, y + int(TARGET_BASELINE_Y), x + cell_width, y + int(TARGET_BASELINE_Y)), fill=(0, 255, 255), width=1)
        draw.rectangle((x + SAFE_BOUNDS[0], y + SAFE_BOUNDS[1], x + SAFE_BOUNDS[2], y + SAFE_BOUNDS[3]), outline=(0, 128, 255), width=1)
        draw.text((x + 6, y + cell_height + 6), name, fill="white")
    sheet.save(report_dir / "alignment_contact_sheet.png", format="PNG", optimize=True)


def main() -> int:
    parser = argparse.ArgumentParser(description="生成小狗 ZHMJ 动画资源")
    parser.add_argument("--source-dir", type=Path, required=True, help="dog_*.gif 源目录")
    parser.add_argument("--output-dir", type=Path, required=True, help="dog_*.mjp 输出目录")
    parser.add_argument("--fps", type=int, default=20, help="目标固定帧率，默认 20")
    parser.add_argument("--quality", type=int, default=85, help="JPEG 质量，默认 85")
    parser.add_argument("--ffmpeg", help="FFmpeg 可执行文件路径")
    parser.add_argument("--report-dir", type=Path, help="输出 UTF-8 对齐清单和九宫格预览")
    parser.add_argument("--verify-only", action="store_true", help="只校验已有 MJP 文件")
    args = parser.parse_args()

    if not (1 <= args.fps <= 60):
        parser.error("--fps 必须在 1..60")
    if not (1 <= args.quality <= 95):
        parser.error("--quality 必须在 1..95")

    if args.verify_only:
        outputs = sorted(args.output_dir.glob("dog_*.mjp"))
        if not outputs:
            raise RuntimeError(f"未找到待校验文件: {args.output_dir}")
        for output in outputs:
            info = parse_container(output)
            print(
                f"OK {output.name}: {info['width']}x{info['height']} "
                f"{info['fps']} FPS, {info['frames']} frames, {info['bytes']} bytes, "
                f"sha256={info['sha256']}"
            )
        return 0

    ffmpeg = ffmpeg_path(args.ffmpeg)
    sources = sorted(args.source_dir.glob("dog_*.gif"))
    if not sources:
        raise RuntimeError(f"未找到 dog_*.gif: {args.source_dir}")

    total_source = 0
    total_output = 0
    expected_outputs: set[Path] = set()
    alignment_entries: list[dict[str, object]] = []
    previews: list[tuple[str, Image.Image]] = []
    for source in sources:
        destination = args.output_dir / f"{source.stem}.mjp"
        expected_outputs.add(destination.resolve())
        info, alignment, preview = convert_one(source, destination, ffmpeg, args.fps, args.quality)
        alignment_entries.append({
            "name": source.stem.removeprefix("dog_"),
            "source": source.name,
            "output": destination.name,
            "fps": info["fps"],
            "frames": info["frames"],
            "sha256": info["sha256"],
            **alignment,
        })
        previews.append((source.stem.removeprefix("dog_"), preview))
        total_source += source.stat().st_size
        total_output += int(info["bytes"])
        print(
            f"OK {source.name} -> {destination.name}: {info['frames']} frames, "
            f"{info['fps']} FPS, {info['bytes']} bytes"
        )

    for stale in args.output_dir.glob("dog_*.mjp"):
        if stale.resolve() not in expected_outputs:
            stale.unlink()
            print(f"删除无对应源文件的旧资源: {stale.name}")

    ratio = total_output / total_source if total_source else 0.0
    if args.report_dir is not None:
        write_alignment_report(args.report_dir, alignment_entries, previews)
        print(f"对齐报告: {args.report_dir.resolve()}")
    print(
        f"完成: {len(sources)} 个动画，GIF {total_source} bytes -> "
        f"ZHMJ {total_output} bytes ({ratio:.1%})"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:  # keep CLI failures concise and actionable
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(1)
