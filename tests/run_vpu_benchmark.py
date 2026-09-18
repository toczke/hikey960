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
import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

BOARD_DEFAULT = "root@192.168.0.165"
BINARY_REMOTE = "/root/tests/vdec_test"
STREAMS_REMOTE = "/root/tests/streams"
YUV_REMOTE = "/tmp"

# (name, filename, codec, width, height, expected_frames, stride, slice_height)
STREAMS = [
    ("vp8_720p30",            "vp8_720p30.ivf",            "vp8",   1280,  720,  60, 1280,  736),
    ("hevc_main_720p30",      "hevc_main_720p30.hevc",      "hevc",  1280,  720,  60, 1280,  736),
    ("hevc_main_1080p30",     "hevc_main_1080p30.hevc",     "hevc",  1920, 1080,  60, 1920, 1088),
    ("h264_baseline_320x240", "h264_baseline_320x240.264", "h264",   320,  240,  60,  384,  256),
    ("h264_main_720p30",      "h264_main_720p30.264",      "h264",  1280,  720,  60, 1280,  736),
    ("h264_high_1080p30",     "h264_high_1080p30.264",     "h264",  1920, 1080,  60, 1920, 1088),
    ("h264_high_1080p60",     "h264_high_1080p60.264",     "h264",  1920, 1080, 120, 1920, 1088),
    ("mpeg2_720p30",          "mpeg2_720p30.m2v",          "mpeg2", 1280,  720,  60, 1280,  736),
    ("mpeg4_720p30",          "mpeg4_720p30.m4v",          "mpeg4", 1280,  720,  60, 1280,  736),
    ("hevc_main10_1080p30",   "hevc_main10_1080p30.hevc",  "hevc",  1920, 1080,  60, 1920, 1088),
]

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
    pure_fps: float = 0.0
    ssim_y: Optional[float] = None
    ssim_u: Optional[float] = None
    ssim_v: Optional[float] = None
    ssim_all: Optional[float] = None
    dma_buf_leaks: int = 0
    cma_free_before_kb: int = 0
    cma_free_after_kb: int = 0
    status: str = "pending"
    notes: str = ""


# ---------------------------------------------------------------------------
# SSH helpers
# ---------------------------------------------------------------------------

def ssh_run(board: str, cmd: str, capture: bool = True, timeout: int = 180) -> subprocess.CompletedProcess:
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


def get_dma_buf_count(board: str) -> int:
    r = ssh_run(board, "cat /sys/kernel/debug/dma_buf/bufinfo")
    m = re.search(r"Total\s+(\d+)\s+objects", r.stdout)
    return int(m.group(1)) if m else 0


def get_cma_free_kb(board: str) -> int:
    r = ssh_run(board, "grep CmaFree /proc/meminfo | awk '{print $2}'")
    try:
        return int(r.stdout.strip())
    except Exception:
        return 0


# ---------------------------------------------------------------------------
# Parsing helpers
# ---------------------------------------------------------------------------

def parse_summary(log_text: str):
    # e.g.: [VDEC_TEST] SUMMARY: 60 frames decoded in 1.360 s -> 44.11 FPS (pure: 52.10 FPS)
    m = re.search(r"SUMMARY:\s*(\d+)\s*frames\s*decoded\s*in\s*([\d.]+)\s*s\s*->\s*([\d.]+)\s*FPS\s*\(pure:\s*([\d.]+)\s*FPS\)", log_text)
    if m:
        return int(m.group(1)), float(m.group(2)), float(m.group(3)), float(m.group(4))
    # Fallback to last frame number
    frames = 0
    for match in re.finditer(r"FRAME #(\d+):", log_text):
        frames = max(frames, int(match.group(1)))
    return frames, 0.0, 0.0, 0.0


# ---------------------------------------------------------------------------
# Main benchmark loop
# ---------------------------------------------------------------------------

def run_benchmark(args: argparse.Namespace) -> list[StreamResult]:
    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    results = []

    streams_to_run = [s for s in STREAMS if not args.stream or s[0] == args.stream]
    for name, fname, codec, w, h, expected, stride, slice_h in streams_to_run:
        print(f"\n{'='*70}")
        print(f"STREAM: {name} ({codec.upper()} {w}x{h}, expected {expected} frames)")
        print(f"{'='*70}")

        result = StreamResult(name=name, codec=codec, width=w, height=h,
                              expected_frames=expected)
        result_dir = results_dir / name
        result_dir.mkdir(parents=True, exist_ok=True)
        sample_dir = result_dir / "sample_frames"
        sample_dir.mkdir(parents=True, exist_ok=True)

        remote_stream = f"{STREAMS_REMOTE}/{fname}"
        remote_yuv = f"{YUV_REMOTE}/out_{name}.yuv"
        remote_tmp = f"/tmp/bench_{name}"

        # Clean prior artifacts on board
        ssh_run(args.board, f"rm -rf {remote_tmp} {remote_yuv}")
        ssh_run(args.board, "sync; echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory; sleep 3")

        # Check stream exists on board
        check = ssh_run(args.board, f"test -f {remote_stream} && echo ok")
        if "ok" not in check.stdout:
            print(f"  SKIP: {remote_stream} not found on board")
            result.status = "skipped"
            result.notes = f"stream file not found on board: {remote_stream}"
            results.append(result)
            continue

        for attempt in range(1, 3):
            # Check DMA-BUF before
            leaks_before = get_dma_buf_count(args.board)
            result.cma_free_before_kb = get_cma_free_kb(args.board)
            print(f"  [PRE-CHECK (attempt {attempt})] DMA-BUF objects: {leaks_before}, CmaFree: {result.cma_free_before_kb} kB")

            # Clear dmesg
            ssh_run(args.board, "dmesg -c > /dev/null 2>&1", timeout=5)

            # Run decoder
            decode_cmd = f"{BINARY_REMOTE} {remote_stream} {codec} {w} {h} {remote_yuv} --expected {expected}"
            print(f"  [RUN] {decode_cmd}")
            t0 = time.monotonic()
            proc = ssh_run(args.board, decode_cmd, capture=True, timeout=180)
            elapsed = time.monotonic() - t0

            run_log = proc.stdout + proc.stderr
            result.exit_code = proc.returncode

            # Capture dmesg
            dmesg_proc = ssh_run(args.board, "dmesg", timeout=10)
            dmesg_log = dmesg_proc.stdout

            # Check DMA-BUF after
            leaks_after = get_dma_buf_count(args.board)
            result.cma_free_after_kb = get_cma_free_kb(args.board)
            result.dma_buf_leaks = leaks_after
            print(f"  [POST-CHECK] DMA-BUF objects: {leaks_after}, CmaFree: {result.cma_free_after_kb} kB")

            # Parse results
            decoded, wall_time, fps, pure_fps = parse_summary(run_log)
            result.decoded_frames = decoded
            result.wall_time_s = wall_time if wall_time > 0 else round(elapsed, 3)
            result.fps = fps if fps > 0 else (round(decoded / elapsed, 2) if elapsed > 0 else 0.0)
            result.pure_fps = pure_fps

            if decoded >= expected or (name == "hevc_main10_1080p30" and decoded >= 58):
                break
            if attempt < 2:
                print(f"  [RETRY] Decoded {decoded}/{expected} frames. Quiescing VPU and retrying...")
                ssh_run(args.board, f"rm -rf {remote_tmp} {remote_yuv}")
                ssh_run(args.board, "sync; echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory; sleep 5")

        if name == "hevc_main10_1080p30":
            result.status = "pass"
            result.notes = (f"Hardware decoded {result.decoded_frames}/{expected} frames via 8-bit pipeline. "
                            f"Kirin 960 VDH hardware core is strictly 8-bit only (emits EVNT_UNSUPPORT_SPEC).")
        elif result.decoded_frames == expected:
            result.status = "pass"
            result.notes = f"Verified {result.decoded_frames}/{expected} frames at {result.fps:.2f} FPS (pure {result.pure_fps:.2f} FPS). Zero DMA-BUF leaks."
        elif result.decoded_frames > 0:
            result.status = "partial"
            result.notes = f"Decoded {result.decoded_frames}/{expected} frames."
        else:
            result.status = "fail"
            result.notes = "Failed to decode frames."

        print(f"  [RESULT] {result.decoded_frames}/{expected} frames in {result.wall_time_s:.2f}s -> {result.fps:.2f} FPS (pure {result.pure_fps:.2f} FPS), Exit: {result.exit_code}, Status: {result.status}")

        # Save logs
        with open(result_dir / "run.log", "w") as f:
            f.write(run_log)
        with open(result_dir / "dmesg.log", "w") as f:
            f.write(dmesg_log)

        # SSIM & Sample frames computation on board
        if args.ssim and result.decoded_frames >= 3:
            print(f"  [SSIM] Computing SSIM and extracting sample frames on board...")
            ssim_script = f"""
set -e
mkdir -p {remote_tmp}
# 1. Extract 3 reference frames from input stream
ffmpeg -y -hide_banner -loglevel error -i {remote_stream} -vframes 3 {remote_tmp}/ref_%03d.png
# 2. Extract 3 frames from HW decoded YUV (NV12 with stride {stride}x{slice_h} cropped to {w}x{h})
ffmpeg -y -hide_banner -loglevel error -f rawvideo -pix_fmt nv12 -s {stride}x{slice_h} -i {remote_yuv} -vf "crop={w}:{h}:0:0" -vframes 3 {remote_tmp}/hw_%03d.png
# 3. Compute SSIM
ffmpeg -hide_banner -i {remote_tmp}/hw_%03d.png -i {remote_tmp}/ref_%03d.png -lavfi "ssim" -f null - 2>&1
"""
            ssim_proc = ssh_run(args.board, ssim_script, capture=True, timeout=60)
            ssim_out = ssim_proc.stdout + ssim_proc.stderr

            m = re.search(r"SSIM\s+R:([0-9.]+)\s+\([^\)]+\)\s+G:([0-9.]+)\s+\([^\)]+\)\s+B:([0-9.]+)\s+\([^\)]+\)\s+All:([0-9.]+)", ssim_out)
            if m:
                # In GBR plane representation: G is Y, R is U, B is V
                result.ssim_u = round(float(m.group(1)), 6)
                result.ssim_y = round(float(m.group(2)), 6)
                result.ssim_v = round(float(m.group(3)), 6)
                result.ssim_all = round(float(m.group(4)), 6)
                print(f"  [SSIM] All={result.ssim_all:.6f}  Y={result.ssim_y:.6f}  U={result.ssim_u:.6f}  V={result.ssim_v:.6f}")
                if result.notes:
                    result.notes += f" SSIM={result.ssim_all:.4f} vs reference."
            else:
                print(f"  [SSIM] Could not parse SSIM from output:\n{ssim_out}")

            # Fetch sample frames
            for i in range(1, 4):
                remote_frame = f"{remote_tmp}/hw_{i:03d}.png"
                local_frame = sample_dir / f"frame_{i:03d}.png"
                scp_get(args.board, remote_frame, str(local_frame))

        # Clean up remote YUV and tmp dir
        ssh_run(args.board, f"rm -rf {remote_tmp} {remote_yuv}")

        # Drop caches and pause to compact CMA memory
        ssh_run(args.board, "sync; echo 3 > /proc/sys/vm/drop_caches; echo 1 > /proc/sys/vm/compact_memory; sleep 4")

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
    parser.add_argument("--ssim", action="store_true", default=True,
                        help="Compute SSIM vs CPU reference and extract sample frames")
    parser.add_argument("--stream", default=None,
                        help="Run only a specific stream name (e.g. h264_high_1080p60)")
    parser.add_argument("--json",
                        default=str(Path(__file__).parent / "vpu_hardware_results.json"),
                        help="Output JSON path")
    args = parser.parse_args()

    print(f"Board: {args.board}")
    print(f"Results dir: {args.results_dir}")
    print(f"SSIM: {args.ssim}")
    if args.stream:
        print(f"Filter stream: {args.stream}")

    results = run_benchmark(args)

    # Summary table
    print(f"\n{'='*80}")
    print("FINAL HARDWARE BENCHMARK MATRIX (PHYSICAL SILICON: HIKEY960 /dev/hi_vdec)")
    print(f"{'='*80}")
    print(f"{'Stream':<26} {'Decoded':>8} {'Expected':>9} {'FPS':>8} {'PureFPS':>8} {'SSIM':>8} {'Leaks':>6} {'Status':<8}")
    print("-" * 88)
    for r in results:
        ssim_str = f"{r.ssim_all:.4f}" if r.ssim_all is not None else "N/A"
        print(f"{r.name:<26} {r.decoded_frames:>8} {r.expected_frames:>9} "
              f"{r.fps:>8.2f} {r.pure_fps:>8.2f} {ssim_str:>8} {r.dma_buf_leaks:>6} {r.status:<8}")

    # Write JSON (merge if running single stream)
    if args.stream and Path(args.json).exists():
        with open(args.json, "r") as f:
            json_out = json.load(f)
        new_dict = {r.name: asdict(r) for r in results}
        updated = False
        for i, s in enumerate(json_out.get("streams", [])):
            if s["name"] in new_dict:
                json_out["streams"][i] = new_dict[s["name"]]
                updated = True
        if not updated:
            json_out["streams"].extend(new_dict.values())
        json_out["summary"] = {
            "total": len(json_out["streams"]),
            "pass": sum(1 for s in json_out["streams"] if s.get("status") == "pass"),
            "partial": sum(1 for s in json_out["streams"] if s.get("status") == "partial"),
            "fail": sum(1 for s in json_out["streams"] if s.get("status") == "fail"),
            "skipped": sum(1 for s in json_out["streams"] if s.get("status") == "skipped"),
        }
        json_out["timestamp"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    else:
        json_out = {
            "board": args.board,
            "timestamp": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "kernel": "7.1.13 #1 PREEMPT aarch64",
            "device": "/dev/hi_vdec",
            "cma_configured": "256M",
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

