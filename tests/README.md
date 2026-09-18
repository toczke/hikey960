# HiKey960 Video Codec (VDEC/VENC) Test Suite & Evidence Protocol

## Evidence-Before-Percentage Rule (Strict Policy)

1. **No Evidence, No Claim**:
   No stream, resolution, or codec may be marked as "VERIFIED ON HARDWARE" in any documentation, commit message, pull request, or status report unless a corresponding directory `tests/results/<stream_name>/` exists in the repository.
   
2. **Mandatory Per-Stream Artifacts**:
   For any hardware decode verification, `tests/results/<stream_name>/` must contain:
   - `dmesg.log`: Clean kernel ring buffer capture spanning the execution (`dmesg -w` during the run).
   - `run.log`: Full command line, exit code, wall-clock timing, FPS, and thermals.
   - `sample_frames/*.png`: Decoded frame PNGs extracted from the output YUV and verified against reference decode.
   - Structural correctness metrics (e.g. SSIM / PSNR / Luma histogram vs CPU reference decode), never purely "byte non-zero percentage".

3. **Status Reports are Derived Summaries Only**:
   A status report is strictly an aggregated summary of evidence files that already exist in `tests/results/`. It is never a projection, extrapolation, or forward-looking guess.
   Any claimed percentage must link directly to the specific `tests/results/...` paths backing each claim. Any percentage without verifiable linked evidence is treated as an ungrounded guess and must be retracted.

4. **Transparent Reporting of Incomplete Work**:
   When a codec or stream is partially working or unstable (e.g. trailing frame drops, pipeline stalls), it must be reported explicitly as:
   `PARTIAL: <N>/<total> frames decoded, root cause under investigation`
   It must never be blended into an aggregate completion percentage that conceals the defect.

5. **Hardware Memory Safety Criterion**:
   Every test run must leave Linux DMA-BUF heaps and CMA free of leaked attachments (`/sys/kernel/debug/dma_buf/bufinfo` must return 0 objects after teardown).

6. **Preservation of Hardware Evidence (Never Delete Receipts)**:
   No PR or commit that touches `docs/07-MULTIMEDIA_AND_GPU.md` hardware-status claims or documentation may delete files under `tests/results/` or add `tests/` to `.gitignore`. Hardware evidence must remain tracked in version control permanently to guarantee auditability.

## Directory Structure

```
tests/
├── README.md                   # This policy document
├── run_vpu_benchmark.py        # Automated test harness driving VDEC hardware verification
├── run_venc_ladder.py          # Automated test harness driving VENC ladder verification
├── vpu_hardware_results.json   # Machine-readable VDEC execution & SSIM results
├── venc_hardware_results.json  # Machine-readable VENC ladder results (Steps 1-5, 4K, concurrent)
├── cpu_baseline_results.json   # Software (CPU FFmpeg) reference decode benchmark
├── vdec_test.c                 # Direct userspace test harness for /dev/hi_vdec ioctl
├── venc_test.c                 # Direct userspace test harness for /dev/hi_venc ioctl
├── drv_omxvdec.h               # OMX VDEC kernel driver header
├── hi_type.h                   # Common HiSilicon types
├── streams/                    # Test bitstream vectors (.264, .hevc, .ivf, .m2v, .m4v)
└── results/                    # Ground-truth hardware execution evidence
    ├── venc_hardware_results.json
    ├── venc_probe.log          # VENC probe and device-tree status verification
    ├── venc_<step_name>/       # Per-step VENC ladder evidence (dmesg, run log, bitstream, ffprobe)
    └── <stream_name>/          # Per-stream VDEC verification directory
        ├── dmesg.log           # Kernel dmesg during hardware decode
        ├── run.log             # Execution command line, exit code, timing, thermals
        └── sample_frames/      # Visual verification frame PNGs
```
