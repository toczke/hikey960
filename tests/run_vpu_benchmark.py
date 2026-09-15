#!/usr/bin/env python3
"""
run_vpu_benchmark.py — HiKey960 VDEC hardware benchmark harness

Usage:
    python3 run_vpu_benchmark.py [--board BOARD_IP] [--results-dir RESULTS_DIR]
                                 [--streams-dir STREAMS_DIR] [--ssim] [--json JSON_OUT]

Runs vdec_test_more_bufs on the HiKey960 board for each test stream,
captures run.log and dmesg.log, optionally extracts sample frames and
computes SSIM vs CPU-decoded reference. Writes a JSON summary.

Requirements (local):
    - SSH access to the board (key-based, no password prompt)
    - ffmpeg installed locally (for CPU reference decoding + SSIM)
    - Python ≥ 3.8

Requirements (on-board):
    - /root/tests/vdec_test_more_bufs  (compiled)
    - /root/test_streams/              (test bitstreams)
    - ffmpeg                           (for PNG extraction from YUV)
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

BOARD_DEFAULT = "root@192.168.0.165"
BINARY_REMOTE = "/root/tests/vdec_test_more_bufs"
STREAMS_REMOTE = "/root/test_streams"
YUV_REMOTE = "/root/tests/yuv_out"

# (name, filename, codec, width, height, expected_total_frames)
STREAMS = [
    ("h264_baseline_320x240",  "h264_baseline_320x240.264",  "h264",  320,   240,  60),
    ("h264_main_720p30",       "h264_main_720p30.264",        "h264", 1280,   720,  60),
    ("h264_high_1080p30",      "h264_high_1080p30.264",       "h264", 1920,  1080,  60),
    ("h264_high_1080p60",      "h264_high_1080p60.264",       "h264", 1920,  1080, 120),
    ("hevc_main_720p30",       "hevc_main_720p30.hevc",       "hevc", 1280,   720,  60),
    ("hevc_main_1080p30",      "hevc_main_1080p30.hevc",      "hevc", 1920,  1080,  60),
    ("hevc_main10_1080p30",    "hevc_main10_1080p30.hevc",    "hevc", 1920,  1080,  60),
    ("vp8_720p30",             "vp8_720p30.ivf",              "vp8",  1280,   720,  60),
    ("mpeg2_720p30",           "mpeg2_720p30.m2v",            "mpeg2", 1280,  720,  60),
    ("mpeg4_720p30",           "mpeg4_720p30.m4v",            "mpeg4", 1280,  720,  60),
]

CODEC_TO_FFMPEG = {
    "h264":  "h264",
    "hevc":  "hevc",
    "vp8":   "vp8",
    "mpeg2": "mpeg2video",
    "mpeg4": "mpeg4",
}

# ---------------------------------------------------------------------------
# Data structures
# ---------------------------------------------------------------------------

@dataclass
class StreamResult:
    name: str
    codec: str
    width: int
    height: int
    expected_frames: int
    decoded_frames: int = 0
    exit_code: int = -1
    wall_time_s: float = 0.0
    fps: float = 0.0
    ssim_y: Optional[float] = None
    ssim_u: Optional[float] = None
    ssim_v: Optional[float] = None
    ssim_all: Optional[float] = None
    status: str = "pending"
    notes: str = ""


# ---------------------------------------------------------------------------
# SSH helpers
# ---------------------------------------------------------------------------

def ssh_run(board: str, cmd: str, capture: bool = True, timeout: int = 120) -> subprocess.CompletedProcess:
    """Run a command over SSH on the board."""
    full = ["ssh", "-o", "ConnectTimeout=10", "-o", "BatchMode=yes", board, cmd]
    if capture:
        return subprocess.run(full, capture_output=True, text=True, timeout=timeout)
    return subprocess.run(full, timeout=timeout)


def scp_get(board: str, remote_path: str, local_path: str) -> bool:
    """Copy a file from the board to local."""
    r = subprocess.run(
        ["scp", "-o", "BatchMode=yes", f"{board}:{remote_path}", local_path],
        capture_output=True, text=True, timeout=120
    )
    return r.returncode == 0


# ---------------------------------------------------------------------------
# Parse frames decoded from run.log
# ---------------------------------------------------------------------------

FRAMES_RE = re.compile(r"(\d+)\s+frames\s+decoded", re.IGNORECASE)
FRAMES_ALT_RE = re.compile(r"Decoded\s+(\d+)\s+frames", re.IGNORECASE)
FRAMES_COUNT_RE = re.compile(r"frame_count\s*=\s*(\d+)", re.IGNORECASE)


def parse_frames(log_text: str) -> int:
    for pattern in (FRAMES_RE, FRAMES_ALT_RE, FRAMES_COUNT_RE):
        m = pattern.search(log_text)
        if m:
            return int(m.group(1))
    # Try last integer on a "frames" line
    for line in reversed(log_text.splitlines()):
        if "frame" in line.lower():
            nums = re.findall(r"\d+", line)
            if nums:
                return int(nums[-1])
    return 0


# ---------------------------------------------------------------------------
# SSIM computation
# ---------------------------------------------------------------------------

def compute_ssim(hw_yuv: str, ref_stream: str, width: int, height: int,
                 codec: str, num_frames: int = 3) -> dict:
    """
    Decode num_frames from ref_stream via CPU (ffmpeg) and from hw_yuv,
    then compute SSIM for each luma/chroma plane.
    Returns dict with y/u/v/all keys (floats) or empty dict on failure.
    """
    with tempfile.TemporaryDirectory() as tmp:
        ref_yuv = os.path.join(tmp, "ref.yuv")
        # CPU decode reference
        r = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-c:v", CODEC_TO_FFMPEG.get(codec, codec),
             "-i", ref_stream,
             "-frames:v", str(num_frames),
             "-f", "rawvideo", "-pix_fmt", "yuv420p", ref_yuv],
            capture_output=True, text=True, timeout=60
        )
        if r.returncode != 0 or not os.path.exists(ref_yuv):
            return {}

        # Compute SSIM via ffmpeg filter
        r = subprocess.run(
            ["ffmpeg", "-hide_banner", "-loglevel", "error",
             "-f", "rawvideo", "-pix_fmt", "yuv420p",
             "-s", f"{width}x{height}", "-r", "1",
             "-i", hw_yuv,
             "-f", "rawvideo", "-pix_fmt", "yuv420p",
             "-s", f"{width}x{height}", "-r", "1",
             "-i", ref_yuv,
             "-lavfi", f"ssim=stats_file={tmp}/ssim_stats.txt",
             "-f", "null", "-", "-frames:v", str(num_frames)],
            capture_output=True, text=True, timeout=60
        )
        # Parse "SSIM Y:x.xxx U:x.xxx V:x.xxx All:x.xxx" from stderr
        out = r.stderr + r.stdout
        m = re.search(
            r"SSIM\s+Y:([0-9.]+)\s+U:([0-9.]+)\s+V:([0-9.]+)\s+All:([0-9.]+)",
            out
        )
        if m:
            return {
                "y": float(m.group(1)),
                "u": float(m.group(2)),
                "v": float(m.group(3)),
                "all": float(m.group(4)),
            }
    return {}


# ---------------------------------------------------------------------------
# Main benchmark loop
# ---------------------------------------------------------------------------

def run_benchmark(args: argparse.Namespace) -> list[StreamResult]:
    results_dir = Path(args.results_dir)
    streams_dir = Path(args.streams_dir)

    # Ensure remote dirs exist
    ssh_run(args.board, f"mkdir -p {YUV_REMOTE}")

    results = []

    for name, fname, codec, w, h, expected in STREAMS:
        print(f"\n{'='*60}")
        print(f"Stream: {name}")
        print(f"{'='*60}")

        result = StreamResult(name=name, codec=codec, width=w, height=h,
                              expected_frames=expected)
        result_dir = results_dir / name
        result_dir.mkdir(parents=True, exist_ok=True)

        remote_stream = f"{STREAMS_REMOTE}/{fname}"
        remote_yuv = f"{YUV_REMOTE}/{name}.yuv"

        # Check stream exists on board
        check = ssh_run(args.board, f"test -f {remote_stream} && echo ok")
        if "ok" not in check.stdout:
            print(f"  SKIP: {remote_stream} not found on board")
            result.status = "skipped"
            result.notes = f"stream file not found on board: {remote_stream}"
            results.append(result)
            continue

        # Clear dmesg
        ssh_run(args.board, "dmesg -c > /dev/null 2>&1", timeout=5)

        # Run decoder
        decode_cmd = (
            f"{BINARY_REMOTE} {remote_stream} {codec} {w} {h} {remote_yuv}"
        )
        print(f"  CMD: {decode_cmd}")
        t0 = time.monotonic()
        proc = ssh_run(args.board, decode_cmd, capture=True, timeout=300)
        elapsed = time.monotonic() - t0

        run_log = proc.stdout + proc.stderr
        result.exit_code = proc.returncode
        result.wall_time_s = round(elapsed, 2)
        result.decoded_frames = parse_frames(run_log)

        if result.decoded_frames > 0:
            result.fps = round(result.decoded_frames / elapsed, 1) if elapsed > 0 else 0.0
            result.status = (
                "pass" if result.decoded_frames >= expected * 0.95
                else "partial"
            )
        else:
            result.status = "fail"

        print(f"  Decoded: {result.decoded_frames}/{expected}  "
              f"({result.wall_time_s:.1f}s, {result.fps:.1f} fps)")
        print(f"  Status: {result.status}")

        # Save logs
        with open(result_dir / "run.log", "w") as f:
            f.write(run_log)

        dmesg_proc = ssh_run(args.board, "dmesg", timeout=10)
        with open(result_dir / "dmesg.log", "w") as f:
            f.write(dmesg_proc.stdout)

        # SSIM computation (if frames decoded and local stream available)
        if args.ssim and result.decoded_frames > 0:
            local_stream = streams_dir / fname
            if local_stream.exists():
                print("  Computing SSIM...")
                # SCP the YUV back locally
                local_yuv = result_dir / f"{name}.yuv"
                if scp_get(args.board, remote_yuv, str(local_yuv)):
                    ssim = compute_ssim(
                        str(local_yuv), str(local_stream),
                        w, h, codec, num_frames=min(3, result.decoded_frames)
                    )
                    if ssim:
                        result.ssim_y = ssim.get("y")
                        result.ssim_u = ssim.get("u")
                        result.ssim_v = ssim.get("v")
                        result.ssim_all = ssim.get("all")
                        print(f"  SSIM All={result.ssim_all:.4f}  "
                              f"Y={result.ssim_y:.4f}  "
                              f"U={result.ssim_u:.4f}  "
                              f"V={result.ssim_v:.4f}")
                    else:
                        print("  SSIM: computation failed")
                else:
                    print("  SSIM: scp of YUV failed")
            else:
                print(f"  SSIM: local stream not found at {local_stream}")

        results.append(result)

    return results


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(description="HiKey960 VPU hardware benchmark")
    parser.add_argument("--board", default=BOARD_DEFAULT,
                        help=f"SSH target (default: {BOARD_DEFAULT})")
    parser.add_argument("--results-dir",
                        default=str(Path(__file__).parent / "results"),
                        help="Directory to write per-stream results")
    parser.add_argument("--streams-dir",
                        default=str(Path(__file__).parent / "streams"),
                        help="Local directory containing bitstream files")
    parser.add_argument("--ssim", action="store_true",
                        help="Compute SSIM vs CPU reference (requires ffmpeg locally)")
    parser.add_argument("--json",
                        default=str(Path(__file__).parent / "vpu_hardware_results.json"),
                        help="Output JSON path")
    args = parser.parse_args()

    print(f"Board: {args.board}")
    print(f"Results dir: {args.results_dir}")
    print(f"SSIM: {args.ssim}")

    results = run_benchmark(args)

    # Summary table
    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"{'Stream':<30} {'Decoded':>8} {'Expected':>9} {'FPS':>8} {'Status':<10}")
    print("-" * 68)
    for r in results:
        ssim_str = f"SSIM={r.ssim_all:.3f}" if r.ssim_all else ""
        print(f"{r.name:<30} {r.decoded_frames:>8} {r.expected_frames:>9} "
              f"{r.fps:>8.1f} {r.status:<10} {ssim_str}")

    # Write JSON
    json_out = {
        "board": args.board,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "binary": BINARY_REMOTE,
        "streams": [asdict(r) for r in results],
        "summary": {
            "total": len(results),
            "pass": sum(1 for r in results if r.status == "pass"),
            "partial": sum(1 for r in results if r.status == "partial"),
            "fail": sum(1 for r in results if r.status == "fail"),
            "skipped": sum(1 for r in results if r.status == "skipped"),
        }
    }
    with open(args.json, "w") as f:
        json.dump(json_out, f, indent=2)
    print(f"\nJSON written to: {args.json}")


if __name__ == "__main__":
    main()
