# VPU Bring-Up Agent Handoff — HiKey960 / Kirin960

> This document is a complete briefing for an agent picking up the HiKey960 VPU bring-up task.
> Read everything before touching code or SSH.

---

## 1. The Goal

Bring up the **HiSilicon Kirin960 VPU (VDEC) hardware decoder** on a HiKey960 board running
a custom Linux 7.1.13 kernel. Produce verified hardware-decoded frames for all 10 test streams,
measure SSIM correctness, and commit the evidence to the repo.

**Definition of Done:**
- `tests/results/<stream>/{dmesg.log,run.log}` exist for all 10 streams  
- ≥3 streams have decoded PNG frames with SSIM ≥ 0.95 vs CPU-decoded reference  
- `tests/vpu_hardware_results.json` written with per-stream frame counts + FPS  
- `docs/07-MULTIMEDIA_AND_GPU.md` updated with verified status table  

---

## 2. Hardware & Access

| Property | Value |
|----------|-------|
| Board | HiKey960 (Kirin960 SoC), 4 GB LPDDR4 |
| SSH | `root@192.168.0.165` (key-based, no password) |
| Kernel | `Linux 7.1.13` (custom, see §4) |
| Local repo | `/home/toczektomasz/Dokumenty/GitHub/hikey960` |
| Branch | `feature/kirin960-vpu-1080p` |

### Board Boot Entries (GRUB)

There are **3 entries** in `/boot/grub/grub.cfg`:

| # | Label | Kernel | DTB | cma= |
|---|-------|--------|-----|------|
| 0 | `Armbian GNU/Linux, with Linux 7.1.13-720p-stable` | `vmlinuz-7.1.13-720p-stable` | `armbian-dtb-7.1.13-720p-stable` | none |
| 1 | `Armbian GNU/Linux, with Linux 7.1.13-hikey960` | `vmlinuz-7.1.13-hikey960` | `armbian-dtb-7.1.13-hikey960` | `cma=256M` |
| 2 | `Test Kernel with Stable DTB` | `vmlinuz-7.1.13-hikey960` | `armbian-dtb-7.1.13-720p-stable` | none |

> [!IMPORTANT]
> **Always use entry #1** (`7.1.13-hikey960` + `cma=256M`). The VPU driver is ONLY in this kernel.
> Entry #0 has no VPU driver at all (`/dev/hi_vdec` absent). Entry #2 lacks `cma=256M` (only 35MB CMA).
>
> To reboot into the correct entry:
> ```bash
> ssh root@192.168.0.165 "grub-reboot 'Armbian GNU/Linux, with Linux 7.1.13-hikey960' && reboot"
> sleep 45
> ssh root@192.168.0.165 "cat /proc/meminfo | grep Cma"
> # Expect: CmaFree: ~249000 kB
> ```

---

## 3. VPU Driver Architecture

### Device Node
`/dev/hi_vdec` — opened by userland test binary

### Kernel Module Path
`drivers-import/vcodec/hi_vcodec/vdec/omxvdec/`

### Key Source Files

| File | Role |
|------|------|
| `omxvdec.c` | `open/release/ioctl` entry points |
| `channel.c` | Channel lifecycle, buffer bind/unbind |
| `task.c` | `task_memory_manager` — decides rebind vs skip |
| `processor_bpp.c` | Bypass-mode frame output, fires `SEQ_INFO_CHG` to userland |
| `decoder_vfmw.c` | VDH firmware callbacks, handles `EVNT_NEED_ARRANGE` |

### DFS State Machine (channel.h)
```
DFS_INIT → DFS_WAIT_ALLOC → DFS_WAIT_CLEAR → DFS_WAIT_INSERT
         → DFS_WAIT_BIND → DFS_WAIT_FILL → DFS_WAIT_ACTIVATE
         → DFS_ALREADY_ALLOC
```

### Normal Decode Flow (working case)
1. Userland opens `/dev/hi_vdec`, creates channel, binds 4 small output bufs (147456 bytes)
2. Starts channel, queues output bufs, feeds SPS/PPS NALs  
3. VDH firmware parses SPS → fires `EVNT_NEED_ARRANGE` in `decoder_vfmw.c:242`
4. `EVNT_NEED_ARRANGE` sets `dfs_alloc_flag = DFS_WAIT_ALLOC`, wakes task thread  
5. `task_memory_manager()` detects resolution change → `task_report_channel_mem()` → `DFS_WAIT_CLEAR`  
6. Processor thread (bypass mode) → `processor_report_size_change_bypass()` → queues `VDEC_EVT_REPORT_SEQ_INFO_CHG` to userland  
7. Userland gets `SEQ_INFO_CHG`, flushes output port, allocates new buffers with correct frame size, rebinds  
8. Frames start flowing via `VDEC_MSG_RESP_OUTPUT_DONE` messages  

### Buffer Size Check (processor_bpp.c:338)
```c
if (BUF_STATE_QUEUED == pbuf->status && pbuf->buf_len >= expect_size)
```
Initial 147456-byte buffers are correct for 320×240 only. For 720p+, rebind is mandatory.

---

## 4. Our Kernel Patches

Located in `drivers-import/` in the repo. Key modifications to the original Hisilicon VDEC driver:

1. **Bypass mode enable** — the Kirin960 VDH operates in "bypass mode" (direct DMA, no internal post-processor). Patches enable this path in `processor_bpp.c`.

2. **CMA allocation** — `cma=256M` in kernel cmdline (DTB `armbian-dtb-7.1.13-hikey960`). The VDH allocates ~60-80MB per 1080p channel from CMA via `task_alloc_channel_mem()`.

3. **omxvdec char device** — driver exposes `/dev/hi_vdec`, implements the IOCTL interface used by test binary.

4. **VP8 / MPEG-2 / MPEG-4 codec support** — added to codec dispatch table.

---

## 5. Test Binary

### Location (on board)
```
/root/tests/vdec_test_more_bufs       # current binary
/root/tests/vdec_test_more_bufs.c     # source
/root/tests/vdec_test_6398            # older binary with 80ms EOS timeout
/root/tests/vdec_test_6398.c          # older source
```

### Usage
```bash
/root/tests/vdec_test_more_bufs <stream_file> <codec> <width> <height> <output.yuv>
# codec: h264, hevc, vp8, mpeg2, mpeg4
```

### How to Recompile
```bash
ssh root@192.168.0.165 "cd /root/tests && gcc -O2 -I/root/tests/include -I/root/tests -o vdec_test_more_bufs vdec_test_more_bufs.c"
```

### Key Logic in vdec_test_more_bufs.c

| Section | Lines | Description |
|---------|-------|-------------|
| Channel config | ~620–636 | `out_count=4`, `out_len=147456`, `stride=(w+63)&~63` |
| `send_input_packet` | ~327–387 | Sends one NAL at a time with `usleep(1000)` between |
| `rebind_output_buffers` | ~390–545 | Flush → free old bufs → alloc new DMA-BUFs → bind → fill |
| `SEQ_INFO_CHG` handler | ~895–928 | Skips rebind if current bufs satisfy; otherwise calls `rebind_output_buffers` |
| EOS logic | ~928–942 | Sends EOS after 3.0s idle (frames>0) or 15.0s no-frames (after patch) |

### Critical Bug in vdec_test_more_bufs — PARTIALLY PATCHED

The EOS timeout was `4.0s` for `frames_decoded == 0`. This fired before the DPB could drain.
**Patched** to `15.0s` with guard `feeder.seq_info_chg_received`. But the fundamental issue remains:
the VDH stops producing frames mid-stream (see §7 Known Issues).

---

## 6. Test Streams

All at `/root/test_streams/` on the board:

| Name | File | Codec | W×H | Frames |
|------|------|-------|-----|--------|
| h264_baseline_320x240 | `h264_baseline_320x240.264` | h264 | 320×240 | 60 |
| h264_main_720p30 | `h264_main_720p30.264` | h264 | 1280×720 | 60 |
| h264_high_1080p30 | `h264_high_1080p30.264` | h264 | 1920×1080 | 60 |
| h264_high_1080p60 | `h264_high_1080p60.264` | h264 | 1920×1080 | 120 |
| hevc_main_720p30 | `hevc_main_720p30.hevc` | hevc | 1280×720 | 60 |
| hevc_main_1080p30 | `hevc_main_1080p30.hevc` | hevc | 1920×1080 | 60 |
| hevc_main10_1080p30 | `hevc_main10_1080p30.hevc` | hevc | 1920×1080 | 60 |
| vp8_720p30 | `vp8_720p30.ivf` | vp8 | 1280×720 | 60 |
| mpeg2_720p30 | `mpeg2_720p30.m2v` | mpeg2 | 1280×720 | 60 |
| mpeg4_720p30 | `mpeg4_720p30.m4v` | mpeg4 | 1280×720 | 60 |

---

## 7. Current Status — What Works, What Doesn't

### Best Single-Run Results (fresh boot, clean driver)

Run `vdec_test_6398` (80ms EOS) immediately after boot — this is the binary that gives the best single-stream results before CMA fragmentation sets in:

| Stream | vdec_test_6398 | vdec_test_more_bufs | Expected |
|--------|---------------|---------------------|----------|
| h264_baseline_320x240 | 8 frames ⚠️ | 4 frames ⚠️ | 60 |
| h264_main_720p30 | 25 frames ⚠️ | 28 frames ⚠️ | 60 |
| h264_high_1080p30 | 13 frames ⚠️ | unknown | 60 |
| h264_high_1080p60 | 66 frames ⚠️ | unknown | 120 |
| hevc_main_720p30 | 6 frames ⚠️ | unknown | 60 |
| hevc_main_1080p30 | 0 frames ❌ | 0 frames ❌ | 60 |
| hevc_main10_1080p30 | **60/60 ✅** | **60/60 ✅** | 60 |
| vp8_720p30 | **60/60 ✅** | **60/60 ✅** | 60 |
| mpeg2_720p30 | 21 frames ⚠️ | 40 frames ⚠️ | 60 |
| mpeg4_720p30 | 48 frames ⚠️ | 27 frames ⚠️ | 60 |

> [!NOTE]
> `hevc_main10_1080p30` and `vp8_720p30` reliably decode **60/60** because they complete all frames
> before the EOS idle timer fires. All other streams produce real hardware-decoded frames but not
> the full count.

### Sequential Benchmark Results (after multiple prior runs — CMA fragmented)

When running 10 streams back-to-back (after a prior 10-stream benchmark), only VP8/MPEG2/MPEG4 produce frames:
```
h264_baseline_320x240: 0 frames (SEQ_INFO_CHG never arrives — CMA fragmentation)
h264_main_720p30:      0 frames
h264_high_1080p30:     0 frames
hevc_main10_1080p30:   60/60
vp8_720p30:            60/60
mpeg2_720p30:          40 frames
mpeg4_720p30:          27 frames
```

---

## 8. Known Issues and Root Causes

### Issue 1: CMA Fragmentation → 0 frames on H264/HEVC after multiple runs

**Symptom:** `SEQ_INFO_CHG` never arrives in userland. Dmesg shows `vfmw channel create success ChanID N` but no `EVNT_NEED_ARRANGE`, no `REPORT_DEC_SIZE_CHG`.

**Root cause:** After several VPU channels open/close, the CMA heap becomes fragmented. The VDH firmware's `task_alloc_channel_mem()` (in `task.c:34-112`) calls `hi_mpi_sys_mmz_alloc_cached()` to allocate VDH internal buffers from CMA. For 720p it needs ~60MB contiguous; for 1080p ~120MB. Even with 246MB CMA total free, fragmentation means no single contiguous block of that size is available. The alloc fails silently — `EVNT_NEED_ARRANGE` never fires.

**VP8/MPEG2/MPEG4 immune:** These codecs require smaller VDH internal buffers and survive fragmentation.

**Workaround:** Reboot between benchmark runs. One clean benchmark run per boot.

**Proper fix:** Investigate whether `hi_mpi_sys_mmz_alloc_cached` can use non-contiguous allocation, or add a `CMA_PAGES_REMAP` path. Alternatively, pre-allocate the VDH internal buffers at module load time and reuse them (requires driver changes).

### Issue 2: Partial Frame Decode — DPB not fully draining

**Symptom:** For H264 Main 720p, only ~28/60 frames come out. The hardware stops producing output after 28 frames even though all 63 NALs were submitted.

**Root cause:** **UNKNOWN — needs investigation.** Hypotheses:
  - **Input NAL pipeline starvation**: The VDH's internal input FIFO is bounded. When we flood 63 NALs at once (usleep(1000) between = 63ms total), the VDH processes only a batch of them, then stalls because its internal output queue fills and it can't DMA decoded frames until userland re-queues output buffers. The interleaving between input feeding and output consumption may be broken.
  - **Output buffer accounting bug**: After rebind, `out_count` = 24. We call `FILL_OUTPUT_FRAME` for all 24. But `out_busy[i]` may not be tracked correctly after a rebind, causing some buffers to never be re-queued.
  - **DPB reference frame limit**: For H264 Main with B-frames, the DPB holds up to `max_num=17` reference frames. If the VDH needs frame N+17 before outputting frame N, and we stopped feeding at frame 60, the last 17 frames in the DPB may never flush without an explicit EOS.
  - **EOS NAL format**: Our EOS NAL is `{end_of_seq, end_of_stream}`. Some Hisilicon VDH firmware versions require a specific "flush" IOCTL instead of an EOS NAL to drain the DPB.

**Key experiment to run:**
```bash
# Count frames produced over a very long window (no EOS timeout)
# Modify vdec_test_more_bufs.c line ~932:
# Change "3.0" to "60.0" (60-second idle after last frame before EOS)
# Then run h264_main_720p30 and count how many frames total come out
```

### Issue 3: hevc_main_1080p30 — 0 frames always

**Symptom:** 0 frames even on a fresh boot. SEQ_INFO_CHG arrives (when CMA is clean), rebind succeeds, but no OUTPUT_DONE messages ever arrive.

**Root cause:** **UNKNOWN.** `hevc_main10_1080p30` (10-bit HEVC 1080p) works perfectly (60/60), but `hevc_main_1080p30` (8-bit HEVC 1080p) produces 0 frames. This is bizarre — 10-bit should be harder. 

Hypotheses:
  - The 8-bit HEVC 1080p stream may have a codec profile/level the driver doesn't handle (Main@L4.0 vs Main10@L4.0)
  - A `pixel_format` field in the channel config may need to differ for 8-bit vs 10-bit HEVC
  - The `hevc_main_1080p30.hevc` stream file itself may be corrupt or use features the VDH doesn't support

**Key experiment:**
```bash
# Check if the HEVC 8-bit stream is parseable
ffprobe /root/test_streams/hevc_main_1080p30.hevc
# Check dmesg during a clean-boot run for any error codes
dmesg | grep -i 'hevc\|err\|fail'
```

### Issue 4: Cleanup Path Segfault (minor)

**Symptom:** After 320×240 H264 decode, kernel oops in `VDEC_MEM_UnmapAndRelease+0xe0` during channel cleanup. Fires AFTER all frames are decoded. Does not affect frame decode correctness.

**Root cause:** `processor_release_inst → channel_release_inst` dereferences a pointer that was already freed during the `processor_work_in_bypass_mode` DPB flush.

---

## 9. What To Fix Next (Priority Order)

### P0 — Understand and fix partial frame drain (Issue 2)

The most impactful fix. If H264 Main 720p can produce 60/60, H264 High 1080p likely can too.

**Approach A — Slow down input feeding:**
Modify `send_input_packet` to NOT immediately feed the next NAL after `INPUT_DONE`. Instead, wait until there are free output buffers before feeding input. This creates back-pressure and prevents VDH internal input queue overflow.

**Approach B — Remove EOS NAL, use IOCTL flush:**
Check if the Hisilicon VDH supports a VDEC_IOCTL_CHAN_FLUSH (analogous to OMX EmptyThisBuffer with EOS). Look in `omxvdec.c` ioctl table for a flush command. Using a hardware flush IOCTL instead of an EOS NAL packet may properly drain the DPB.

**Approach C — Check max output buffer re-queue:**
After each OUTPUT_DONE, we re-queue the buffer. But `rebind_output_buffers` sets `out_count` to `max_num + 7`. Check if the driver limits the number of concurrently-queued output buffers. If so, the VDH may stall when the FIFO is full.

### P1 — Fix hevc_main_1080p30 (Issue 3)

Compare channel config between `hevc_main_1080p30` and `hevc_main10_1080p30`. The only difference should be `bitdepth` or `pixel_format` in the OMXVDEC_CHAN_CFG struct.

In `vdec_test_more_bufs.c` around lines 620–660, check what `codec_type` value is passed for HEVC and whether there's a separate type for HEVC Main10.

### P2 — Fix CMA fragmentation (Issue 1)

Pre-allocate a fixed-size CMA pool at module load. Or restructure `task_alloc_channel_mem` to use `dma_alloc_from_contiguous` with a retry/compaction loop.

---

## 10. Key IOCTLs (from drv_omxvdec.h)

```c
VDEC_IOCTL_CHAN_CREATE         // Create decoder channel
VDEC_IOCTL_CHAN_START          // Start channel (begin accepting input)  
VDEC_IOCTL_CHAN_BIND_BUFFER    // Bind output DMA-BUF to channel
VDEC_IOCTL_CHAN_UNBIND_BUFFER  // Unbind output buffer
VDEC_IOCTL_EMPTY_INPUT_STREAM  // Submit compressed input NAL
VDEC_IOCTL_FILL_OUTPUT_FRAME   // Queue output buffer to receive decoded frame
VDEC_IOCTL_CHAN_GET_MSG        // Poll for events (OUTPUT_DONE, SEQ_INFO_CHG, etc.)
VDEC_IOCTL_FLUSH_PORT          // Flush input or output port
VDEC_IOCTL_CHAN_STOP            // Stop channel
VDEC_IOCTL_CHAN_DESTROY         // Destroy channel
```

### Event Messages (OMXVDEC_MSG_INFO.msgcode)
```c
VDEC_MSG_RESP_OUTPUT_DONE      // Frame ready (check data_len > 0 for real frame vs EOS)
VDEC_MSG_RESP_INPUT_DONE       // Input NAL consumed, buffer returned
VDEC_MSG_RESP_START_DONE       // Channel started
VDEC_EVT_REPORT_SEQ_INFO_CHG   // Resolution/buffer requirements changed — must rebind
VDEC_EVT_REPORT_IMG_SIZE_CHG   // Image size change
```

---

## 11. Kernel Debug Tips

```bash
# Enable verbose driver logging (if supported)
echo 0xFFFF > /sys/module/omxvdec/parameters/log_level 2>/dev/null || true

# Watch VDH state in real time
dmesg -w | grep -E 'EVNT|DFS|REPORT|SEQ|chan|VDEC'

# Check CMA fragmentation
cat /proc/buddyinfo | grep -i cma
cat /proc/meminfo | grep Cma

# Check for stuck D-state processes (always do this before a run)
ps aux | grep vdec | grep -v grep
```

---

## 12. Reproduction Steps for a Fresh Agent

```bash
# 1. Reboot to correct kernel
ssh root@192.168.0.165 "grub-reboot 'Armbian GNU/Linux, with Linux 7.1.13-hikey960' && reboot"
sleep 50

# 2. Verify
ssh root@192.168.0.165 "uname -r && ls /dev/hi_vdec && cat /proc/meminfo | grep CmaFree"
# Expect: 7.1.13  /dev/hi_vdec  CmaFree: ~249000 kB

# 3. Run ONE stream (best chance of clean state)
ssh root@192.168.0.165 "dmesg -c > /dev/null && /root/tests/vdec_test_more_bufs /root/test_streams/vp8_720p30.ivf vp8 1280 720 /tmp/out_vp8.yuv"
# Expected: 60 frames decoded

# 4. NEVER run two streams concurrently — one at a time only
# 5. Reboot if any run produces 0 frames and it's not hevc_main_1080p30
```

---

## 13. Repo Layout

```
hikey960/
├── drivers-import/
│   └── vcodec/hi_vcodec/vdec/omxvdec/   ← VPU kernel driver
│       ├── omxvdec.c
│       ├── channel.c / channel.h
│       ├── task.c
│       ├── processor_bpp.c
│       └── decoder_vfmw.c
├── tests/
│   ├── vdec_test.c                        ← test binary source (canonical)
│   ├── run_vpu_benchmark.py               ← Python benchmark harness (not yet working e2e)
│   ├── README.md                          ← evidence policy
│   └── results/
│       ├── h264_baseline_320x240/         ← {run.log, dmesg.log, frames/}
│       ├── vp8_720p30/
│       └── ... (10 subdirs total)
└── docs/
    └── 07-MULTIMEDIA_AND_GPU.md           ← needs updating with verified results
```

---

## 14. SSIM Measurement (once frames are obtained)

```bash
# On board: save YUV
/root/tests/vdec_test_more_bufs /root/test_streams/vp8_720p30.ivf vp8 1280 720 /tmp/hw_vp8.yuv

# SCP to local
scp root@192.168.0.165:/tmp/hw_vp8.yuv /tmp/

# Convert to PNG (first 3 frames)
ffmpeg -f rawvideo -pix_fmt nv12 -s 1280x720 -i /tmp/hw_vp8.yuv -frames:v 3 /tmp/hw_frame_%03d.png

# CPU reference
ffmpeg -i /path/to/vp8_720p30.ivf -frames:v 3 /tmp/ref_frame_%03d.png

# SSIM
ffmpeg -i /tmp/hw_frame_001.png -i /tmp/ref_frame_001.png -lavfi ssim -f null -
```

---

*Generated: 2026-09-15. Contact: Tomasz (repo owner). Branch: `feature/kirin960-vpu-1080p`.*
