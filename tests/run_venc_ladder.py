#!/usr/bin/env python3
"""
run_venc_ladder.py — HiKey960 Kirin 960 VENC hardware encoder ladder test harness
Work Order 3: Drives and verifies Steps 1-5 of the VENC resolution ladder.

Outputs:
  - tests/results/venc_<step_name>/{dmesg.log, run.log, output.<h264|hevc>, ffprobe_output.txt}
  - tests/venc_hardware_results.json
"""

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Optional, List

BOARD_DEFAULT = "root@192.168.0.165"
VENC_BINARY = "/root/tests/venc_test"

@dataclass
class LadderStep:
    name: str
    codec: str           # "h264" or "hevc"
    width: int
    height: int
    fps: int
    num_frames: int
    bitrate: int
    concurrency: int = 1 # 1 for single stream, 4 for 4 concurrent
    iterations: int = 1  # repeat N times in a loop
    description: str = ""

LADDER_STEPS = [
    LadderStep(
        name="step1_h264_640x480_30fps",
        codec="h264",
        width=640,
        height=480,
        fps=30,
        num_frames=30,
        bitrate=1500000,
        concurrency=1,
        iterations=10,
        description="Single H.264 stream, 640x480@30fps, 30 frames, 10x loop"
    ),
    LadderStep(
        name="step2_h264_1080p_30fps",
        codec="h264",
        width=1920,
        height=1080,
        fps=30,
        num_frames=60,
        bitrate=5000000,
        concurrency=1,
        iterations=1,
        description="Single H.264 stream, 1920x1080@30fps, 60 frames"
    ),
    LadderStep(
        name="step3_hevc_1080p_30fps",
        codec="hevc",
        width=1920,
        height=1080,
        fps=30,
        num_frames=60,
        bitrate=5000000,
        concurrency=1,
        iterations=1,
        description="Single H.265/HEVC stream, 1920x1080@30fps, 60 frames"
    ),
    LadderStep(
        name="step4_concurrent_4x1080p",
        codec="h264",
        width=1920,
        height=1080,
        fps=30,
        num_frames=60,
        bitrate=4000000,
        concurrency=4,
        iterations=5,
        description="4x concurrent 1080p30 H.264 streams, 5x loop"
    ),
    LadderStep(
        name="step5_h264_4k_uhd_3840x2160",
        codec="h264",
        width=3840,
        height=2160,
        fps=30,
        num_frames=30,
        bitrate=20000000,
        concurrency=1,
        iterations=1,
        description="Target maximum 4K UHD H.264 3840x2160@30fps (hardware limit min_dim<=2160)"
    ),
    LadderStep(
        name="step5_h264_3840x2400_30fps",
        codec="h264",
        width=3840,
        height=2400,
        fps=30,
        num_frames=30,
        bitrate=20000000,
        concurrency=1,
        iterations=1,
        description="Target maximum resolution H.264 3840x2400@30fps (exceeds hardware limit min_dim<=2160)"
    ),
    LadderStep(
        name="step5_hevc_4k_uhd_3840x2160",
        codec="hevc",
        width=3840,
        height=2160,
        fps=30,
        num_frames=30,
        bitrate=20000000,
        concurrency=1,
        iterations=1,
        description="Target maximum 4K UHD H.265 3840x2160@30fps"
    ),
    LadderStep(
        name="step5_hevc_3840x2400_30fps",
        codec="hevc",
        width=3840,
        height=2400,
        fps=30,
        num_frames=30,
        bitrate=20000000,
        concurrency=1,
        iterations=1,
        description="Target maximum resolution H.265 3840x2400@30fps (exceeds hardware limit min_dim<=2160)"
    ),
]

def ssh_cmd(board: str, cmd: str, timeout: int = 180) -> subprocess.CompletedProcess:
    full = ["ssh", "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=no", board, cmd]
    return subprocess.run(full, capture_output=True, text=True, timeout=timeout)

def scp_from(board: str, remote_path: str, local_path: str, timeout: int = 120):
    full = ["scp", "-o", "ConnectTimeout=10", "-o", "StrictHostKeyChecking=no", f"{board}:{remote_path}", local_path]
    subprocess.run(full, check=True, timeout=timeout)

def probe_stream(file_path: str) -> dict:
    cmd = [
        "ffprobe", "-v", "error",
        "-show_entries", "stream=codec_name,width,height,r_frame_rate,nb_read_frames,nb_read_packets",
        "-count_frames",
        "-count_packets",
        "-of", "json",
        file_path
    ]
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        return {"error": res.stderr}
    try:
        data = json.loads(res.stdout)
        streams = data.get("streams", [])
        if streams:
            info = streams[0]
            try:
                nb = int(info.get("nb_read_frames", 0) or info.get("nb_read_packets", 0) or 0)
            except Exception:
                nb = 0
            if nb == 0 or info.get("width", 0) == 0:
                return {"error": f"Invalid stream geometry or frame count: frames={nb}, width={info.get('width')}", "raw": info}
            info["nb_read_frames"] = str(nb)
            return info
        return {"error": "no streams found"}
    except Exception as e:
        return {"error": str(e)}

def run_step(board: str, step: LadderStep, results_dir: Path) -> dict:
    print(f"\n=================================================================")
    print(f"Executing {step.name}: {step.description}")
    print(f"=================================================================")
    step_dir = results_dir / f"venc_{step.name}"
    step_dir.mkdir(parents=True, exist_ok=True)

    ext = "hevc" if step.codec == "hevc" else "h264"
    remote_out = f"/tmp/{step.name}.{ext}"
    local_out = step_dir / f"output.{ext}"

    # Clear dmesg
    ssh_cmd(board, "dmesg -c > /dev/null")

    t_start = time.time()
    all_runs = []
    success = True
    exit_code = 0
    run_log_content = ""

    if step.concurrency == 1:
        for it in range(1, step.iterations + 1):
            cmd = f"{VENC_BINARY} {step.codec} {step.width} {step.height} {step.num_frames} {remote_out} --fps {step.fps} --bitrate {step.bitrate}"
            print(f"[{step.name}] Iteration {it}/{step.iterations}: {cmd}")
            proc = ssh_cmd(board, cmd)
            run_log_content += f"--- Iteration {it} ---\nCommand: {cmd}\nExit Code: {proc.returncode}\n{proc.stdout}\n{proc.stderr}\n"
            if proc.returncode != 0:
                print(f"[{step.name}] Iteration {it} FAILED (exit {proc.returncode})")
                success = False
                exit_code = proc.returncode
                break
    else:
        # Concurrent execution
        for it in range(1, step.iterations + 1):
            cmds = []
            for c in range(step.concurrency):
                out_c = f"/tmp/{step.name}_chn{c}.{ext}"
                cmds.append(f"{VENC_BINARY} {step.codec} {step.width} {step.height} {step.num_frames} {out_c} --fps {step.fps} --bitrate {step.bitrate}")
            joined_cmd = " & \n".join(cmds) + " & \nwait"
            print(f"[{step.name}] Concurrent Iteration {it}/{step.iterations} (4 streams)...")
            proc = ssh_cmd(board, joined_cmd)
            run_log_content += f"--- Concurrent Iteration {it} ---\n{proc.stdout}\n{proc.stderr}\n"
            if proc.returncode != 0:
                print(f"[{step.name}] Concurrent Iteration {it} FAILED")
                success = False
                exit_code = proc.returncode
                break

    wall_time = time.time() - t_start

    # Capture dmesg
    dmesg_proc = ssh_cmd(board, "dmesg")
    (step_dir / "dmesg.log").write_text(dmesg_proc.stdout)
    (step_dir / "run.log").write_text(run_log_content)

    ffprobe_info = {}
    if success:
        try:
            scp_from(board, remote_out if step.concurrency == 1 else f"/tmp/{step.name}_chn0.{ext}", str(local_out))
            ffprobe_info = probe_stream(str(local_out))
            (step_dir / "ffprobe_output.txt").write_text(json.dumps(ffprobe_info, indent=2))
            print(f"[{step.name}] Bitstream verified by ffprobe: {ffprobe_info}")
        except Exception as e:
            print(f"[{step.name}] Error copying/probing bitstream: {e}")
            ffprobe_info = {"error": str(e)}

    return {
        "name": step.name,
        "codec": step.codec,
        "width": step.width,
        "height": step.height,
        "fps": step.fps,
        "num_frames": step.num_frames,
        "bitrate": step.bitrate,
        "iterations": step.iterations,
        "concurrency": step.concurrency,
        "wall_time_s": wall_time,
        "exit_code": exit_code,
        "success": success,
        "ffprobe": ffprobe_info,
        "status": "[VERIFIED ON HARDWARE]" if (success and "error" not in ffprobe_info) else "[FAIL / OPEN BUG]"
    }

def main():
    parser = argparse.ArgumentParser(description="HiKey960 VENC Ladder Benchmark")
    parser.add_argument("--board", default=BOARD_DEFAULT, help="SSH target (default: root@192.168.0.165)")
    parser.add_argument("--results-dir", default="tests/results", help="Directory for evidence results")
    parser.add_argument("--step", default=None, help="Run specific step only (e.g. step1)")
    args = parser.parse_args()

    results_dir = Path(args.results_dir)
    results_dir.mkdir(parents=True, exist_ok=True)

    summary = []
    for step in LADDER_STEPS:
        if args.step and args.step not in step.name:
            continue
        res = run_step(args.board, step, results_dir)
        summary.append(res)

    out_json = results_dir / "venc_hardware_results.json"
    out_json.write_text(json.dumps(summary, indent=2))
    print(f"\nAll ladder tests completed. Results written to {out_json}")

if __name__ == "__main__":
    main()
