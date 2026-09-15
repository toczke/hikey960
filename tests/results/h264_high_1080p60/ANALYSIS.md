# Analysis: h264_high_1080p60 Decoder Behavior

## Summary
`h264_high_1080p60.264` (120 frames, High Profile, 1920x1080 @ 60fps) decodes partially (up to 66/120 frames, 55%) in single-stream isolated runs and fails (0/120 frames) in back-to-back sequential benchmark runs.

## Empirical Observations & Test Data

| Run Configuration | Binary / Delay | Frames Decoded | Result | Notes |
|-------------------|----------------|----------------|--------|-------|
| Isolated single run | `vdec_test_6398` (80ms idle EOS) | 66 / 120 (55.0%) | PARTIAL | Decodes 66 frames cleanly at ~50 FPS before EOS sentinel closes pipeline |
| Extended EOS delay (400ms) | `vdec_test_clean` (400ms idle EOS) | 4 / 120 | PARTIAL | Decoder stalls waiting on DPB buffer return; early NAL flood causes queue overflow |
| Extended EOS delay (800ms) | `vdec_test_clean` (800ms idle EOS) | 0 / 120 | FAIL | Stalls waiting for initial `SEQ_INFO_CHG` or DPB buffer rebind |
| Back-to-back run | `run_final_benchmark.sh` | 0 / 120 | FAIL | `vfmw channel create success ChanID 2`, but `EVNT_NEED_ARRANGE` never fires |

## Root Cause Analysis

1. **Non-monotonic EOS Convergence:**
   Increasing the post-EOS drain delay does *not* monotonically increase decoded frame count to 120/120. Thus, the early termination is not purely an idle timing issue.
2. **DPB Reference Buffer Starvation:**
   H.264 High Profile 1080p60 with B-frames requires `min_num=14`, `max_num=18` DPB frames. When only 18 output buffers are bound, the VDH hardware pipeline requires active re-queuing of consumed frames. If userspace sends all 123 NALs in <200ms, the input FIFO fills while output buffers are saturated with reference frames.
3. **`Last frame report failed!` in dmesg:**
   When userspace submits the EOS sentinel NAL packet while the DPB has unconsumed reference dependencies, the kernel driver logs `[timestamp] Last frame report failed!`. The sentinel cannot acquire a free output buffer slot to report EOF to userspace.
4. **CMA Heap Fragmentation:**
   Each 1080p channel requires ~120MB contiguous physical memory allocated from CMA for internal VDH buffers (`task_alloc_channel_mem()`). Sequential channel create/destroy cycles fragment the CMA heap, causing `task_alloc_channel_mem()` to fail silently on subsequent 1080p runs.

## Status: OPEN BUG
Tracked as open issue for H.264 High 1080p60.
Recommended resolution:
- Implement flow control in userspace NAL feeder: do not submit NAL $N+1$ until output frame $N - \text{DPB\_DEPTH}$ has been processed.
- Pre-allocate contiguous CMA buffers at driver load time rather than per-channel dynamic allocation.
