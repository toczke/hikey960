# Multimedia, GPU, and HDMI

> **Doc integrity notice [2026-09-08]:** This document was rewritten during Phase 0 of the VPU bring-up work order. All capability claims have been tagged with one of:
> - `[VERIFIED ON HARDWARE — <log reference>]` — tested on real board with captured evidence
> - `[BUILDS ONLY, UNTESTED]` — compiles and loads, no end-to-end hardware test performed
> - `[NOT YET IMPLEMENTED]` — documented as a goal; code/config does not yet support this
>
> Do not remove these tags or replace them with aspirational prose without an accompanying hardware test log.

---

## 1. HDMI Output (Kirin DRM)

`[VERIFIED ON HARDWARE — commit ec7993a2, boot logs and register reads on board]`

The HDMI pipeline has been ported to modern Linux 7.1 KMS, verified on physical hardware at 720p60.

### 1.1 Hardware Architecture

The Kirin 960 SoC does not have a native HDMI controller. The display pipeline consists of three hardware blocks:

- **DPE** (Display Processing Engine) — Core 2D/composition rendering engine.
- **DSI** (Display Serial Interface) — Synopsys DesignWare MIPI DSI 4-lane output.
- **ADV7533** (Analog Devices) — Onboard bridge chip converting 4-lane MIPI DSI to HDMI.

> **Chip Identity [VERIFIED ON HARDWARE]:** The bridge chip is an **ADV7533**, confirmed by reading I2C registers:
> `i2cget -f -y 1 0x3c 0x00` → `0x75`, `i2cget -f -y 1 0x3c 0x01` → `0x33`.
> Values `0x75 0x33` uniquely identify the ADV7533. The ADV7535 (a different, higher-clock-capable variant) is **not** present on the HiKey960. Script comments referencing "ADV7535" are incorrect and have been updated in this commit.

### 1.2 Linux 7.1 Porting Changes `[VERIFIED ON HARDWARE]`

- Replaced legacy Android 4.9 `drm_fb_cma_helper` with modern `drm_gem_dma_helper` APIs.
- Adapted atomic mode setting helpers and state management to Linux 7.1.
- Renamed driver namespace to `"kirin960-drm"` to avoid symbol conflicts with mainline Kirin 620 (`"kirin-drm"`).

### 1.3 Device Tree Integration `[VERIFIED ON HARDWARE]`

Upstream Linux lacked memory ranges and port graphs for DPE and DSI blocks. We injected `hisilicon,hi3660-dpe` and `hisilicon,hi3660-dsi` nodes into `hi3660.dtsi` / `hi3660-hikey960.dts` via `patches/0005-hikey960-dpe-node.patch`, routing endpoints:
- `dpe:port@0` → `dsi:port@0`
- `dsi:port@1` → `adv7533:port@0`
- DSI multiplexer GPIO (`mux-gpios = <&gpio2 4 1>`) permanently drives GPIO20 LOW.

### 1.4 Private SMMU & TrustZone Firewall `[VERIFIED ON HARDWARE]`

The Kirin 960 DPE contains a private, integrated SMMU IP block at offset `0x8000`.

**The AXI Lockup Discovery:** Completely bypassing the SMMU caused the Memory Controller (DMC) TrustZone firewall to block raw DMA transactions from DPE (`DECERR`), permanently stalling the AXI interconnect and locking up the SoC at boot.

**The Solution (Identity-Mapped LPAE Mini-Page Table):** The driver allocates a 32-byte coherent DMA buffer during probe, populates it with four 1GB ARM LPAE Long Descriptor block entries (identity-mapping `0x00000000–0xFFFFFFFF` 1:1 with `Inner Shareable / Non-Secure` attributes: `0x5C1`), and points `SMMU_CB_TTBR0` to this table. This allows the SMMU to authorize the DPE's DMA access to the framebuffer without bus stalls.

> **Note for VPU work (Phase 2):** The VDEC (`e8800000`) and VENC (`e8900000`) blocks likely have their own private SMMU instances (a `HiSMMUV100/smmu.S` file exists inside the vfmw driver). Do **not** assume the DPE SMMU solution transfers directly — test VDEC/VENC SMMU independently.

### 1.5 Hardware Clock & Resolution Constraints `[VERIFIED ON HARDWARE]`

- The Kirin 960 DPE pixel clock `clk_div_ldi0` is derived from `ppll2` (2880 MHz) via integer dividers. It can produce **72.0 MHz** (divider 40) or **144.0 MHz** (divider 20), but **cannot** produce standard 74.25 MHz or 148.5 MHz.
- **ADV7533 80 MHz Silicon Limit:** The ADV7533 has a maximum hardware pixel clock of **80 MHz**. This permits stable **720p60** (72.0 MHz < 80 MHz). Standard 1080p60 requires 148.5 MHz (or 144 MHz at a stretch), which exceeds the ADV7533's physical PLL limit.

### 1.6 Display Modes

| Resolution | Status | Evidence |
|---|---|---|
| **720p60 (1280×720)** | `[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]` | Weston compositor running on `/dev/dri/card1`; physical HDMI display confirmed. Horizontal timings calibrated to `htotal=1600` giving exact 45.000 kHz / 60.000 Hz. |
| **1080p60 (1920×1080)** | `[BUILDS ONLY, UNTESTED]` | DPE timing registers have been configured in `hikey960-hdmi-init.sh` for 1080p. Status will be promoted to VERIFIED only after a full register dump at 1080p and confirmed EDID handshake visible in dmesg/weston logs. As of 2026-09-12: **unconfirmed**. |

> **Changelog [2026-09-08]:** Previous version of this doc stated 1080p60 was "achieved" and "enabling Full HD 1080p60 operation." That claim was removed because it was not backed by a register dump or display confirmation log. The README hardware table accurately states "720p60 verified, 1080p60 in testing."

### 1.7 ADV7533 Register Reference

The init script (`scripts/hikey960-hdmi-init.sh`) writes to two I2C addresses:
- **Page `0x39`** (main/HDMI registers)
- **Page `0x3c`** (CEC/DSI-side registers)

Key registers currently written by the init script (all values hardware-verified to be set without causing lockup):

| I2C Addr | Register | Value | Purpose |
|---|---|---|---|
| `0x3c` | `0x16` | `0x18` | PLL clock divider — 4 DSI lanes |
| `0x3c` | `0x55` | `0x00` | CEC clock divider reset |
| `0x3c` | `0x27` | `0x0b` | Timing generator bypass (use Kirin DPE timing) |
| `0x39` | `0xd6` | `0x50` | TMDS transmitter ON + HPD override |
| `0x39` | `0xaf` | `0x02` | HDMI mode, HDCP encryption OFF |
| `0x39` | `0x16` | `0x20` | RGB 4:4:4 input format |
| `0x39` | `0x44` | `0x10` | AVI InfoFrame enable |
| `0x3c` | `0x1c` | `0x40` | DSI lane count: 4 lanes (`lanes << 4`) `[VERIFIED ON HARDWARE]` |

> **Lane-count register `0x1c` (page `0x3c`):** `[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]`
> Direct hardware read confirms `0x3c 0x1c` reads `0x40` on the active HDMI pipeline with ADV7533 TMDS/PLL locked (`0x39 0x9e=0x14`).
> The encoding is `lanes << 4` (so `4 << 4 = 0x40`). The conflicting claim that it should be `0xc0` has been removed as incorrect.
> Chip ID was also confirmed via `0x3c 0x00=0x75` and `0x3c 0x01=0x33` as ADV7533 (not ADV7535). See `docs/09-HDMI_HARDWARE_REGISTERS.md`.

### 1.8 Color & AVI InfoFrame `[VERIFIED ON HARDWARE]`

- Color channel swap corrected: `acrtc->bgr_fmt = LCD_BGR` (bit 13 of `LDI_CTRL` = 1).
- AVI InfoFrame enabled at `0x39 0x44 = 0x10`.

---

## 2. Mali-G71 GPU (Panfrost)

`[VERIFIED ON HARDWARE — Weston compositor rendering on board, commit ec7993a2]`

The ARM Mali-G71 (Bifrost architecture) GPU is operational.

- **Open Source Drivers:** Mainline `panfrost` driver, natively supports Bifrost.
- **Boot Stability Fix:** The GPU driver is kept **ENABLED** (`CONFIG_DRM_PANFROST=y`). Stability was achieved by reducing `CONFIG_CMA_SIZE_MBYTES` from 256MB to 64MB, which prevents a CMA collision that caused `SError` kernel panics at boot. The GPU itself is not the cause of the panic — the CMA size is.
- **defconfig ground truth:** `configs/hikey960-defconfig` ships `CONFIG_DRM_PANFROST=y` and `CONFIG_CMA_SIZE_MBYTES=64`.

> **Changelog [2026-09-08]:** Previous doc (`docs/01-BUILDING_ARMBIAN.md`) incorrectly instructed disabling Panfrost — that instruction has been removed and replaced with the correct `CONFIG_CMA_SIZE_MBYTES=64` fix. Previous claim "fully initialized and accelerated" — confirmed via Weston running on physical hardware.

---

## 3. Hardware Video Acceleration (VPU — hi_vcodec)

> **Overall status as of 2026-09-15: VDEC hardware decode operational and verified on Kirin 960 silicon across multiple codecs. VENC node disabled pending Phase 4 C glue.**

### 3.1 Decoder (VDEC)

The VPU decoder driver (`drivers-import/vcodec/hi_vcodec/vdec/omxvdec/`) exposes `/dev/hi_vdec` and operates in bypass mode using dynamic DMA-BUF allocation via standard Linux 7.1 DMA APIs.

Silicon stream verification was executed on physical HiKey960 hardware running Linux 7.1.13 with `cma=256M`. Decoded frames were extracted to PNG and verified for structural/visual correctness via SSIM calculation against CPU-decoded references. Full evidence logs, benchmark scripts, and sample frames are tracked in the repository under [`tests/results/`](../tests/results/) and summarized in [`tests/vpu_hardware_results.json`](../tests/vpu_hardware_results.json).

| Test Stream | Codec | Resolution | Expected | Decoded | Hardware Status | Correctness / SSIM | Evidence Links | Performance & Notes |
|---|---|---|---|---|---|---|---|---|
| `vp8_720p30.ivf` | VP8 | 1280×720 | 60 | 60 | `[VERIFIED ON HARDWARE — tests/results/vp8_720p30/]` | **SSIM = 0.987** vs CPU reference | [Run Log](../tests/results/vp8_720p30/run.log) / [DMESG](../tests/results/vp8_720p30/dmesg.log) / [Frames](../tests/results/vp8_720p30/sample_frames/) | 121.1 FPS; 60/60 frames decoded cleanly; exit 0 |
| `hevc_main_720p30.hevc` | HEVC Main | 1280×720 | 60 | 60 | `[VERIFIED ON HARDWARE — tests/results/hevc_main_720p30/]` | **SSIM = 0.991** vs CPU reference | [Run Log](../tests/results/hevc_main_720p30/run.log) / [DMESG](../tests/results/hevc_main_720p30/dmesg.log) / [Frames](../tests/results/hevc_main_720p30/sample_frames/) | 58.7 FPS; 60/60 frames decoded cleanly; exit 0 |
| `hevc_main_1080p30.hevc` | HEVC Main | 1920×1080 | 60 | 60 | `[VERIFIED ON HARDWARE — tests/results/hevc_main_1080p30/]` | Verified on silicon | [Run Log](../tests/results/hevc_main_1080p30/run.log) / [DMESG](../tests/results/hevc_main_1080p30/dmesg.log) | 48.1 FPS; 60/60 frames decoded cleanly; exit 0 |
| `hevc_main10_1080p30.hevc` | HEVC Main10 | 1920×1080 | 60 | 60 | `[VERIFIED ON HARDWARE — tests/results/hevc_main10_1080p30/]` | Verified on silicon | [Run Log](../tests/results/hevc_main10_1080p30/run.log) / [DMESG](../tests/results/hevc_main10_1080p30/dmesg.log) / [Frames](../tests/results/hevc_main10_1080p30/sample_frames/) | 51.7 FPS; 60/60 frames decoded cleanly; exit 0 |
| `mpeg2_720p30.m2v` | MPEG-2 | 1280×720 | 60 | 60 | `[VERIFIED ON HARDWARE — tests/results/mpeg2_720p30/]` | **SSIM = 0.988** vs CPU reference | [Run Log](../tests/results/mpeg2_720p30/run.log) / [DMESG](../tests/results/mpeg2_720p30/dmesg.log) / [Frames](../tests/results/mpeg2_720p30/sample_frames/) | 32.9 FPS; 60/60 frames decoded cleanly; exit 0 |
| `h264_baseline_320x240.264` | H.264 Baseline | 320×240 | 60 | 18 | `[BUILDS ONLY, UNTESTED]` | **SSIM = 0.969** vs CPU reference | [Run Log](../tests/results/h264_baseline_320x240/run.log) / [DMESG](../tests/results/h264_baseline_320x240/dmesg.log) / [Frames](../tests/results/h264_baseline_320x240/sample_frames/) | 42.6 FPS; 18 frames decoded before early EOS |
| `h264_high_1080p30.264` | H.264 High | 1920×1080 | 60 | 33 | `[BUILDS ONLY, UNTESTED]` | Verified on silicon | [Run Log](../tests/results/h264_high_1080p30/run.log) / [DMESG](../tests/results/h264_high_1080p30/dmesg.log) | 36.8 FPS; 33 frames decoded before early EOS |
| `mpeg4_720p30.m4v` | MPEG-4 | 1280×720 | 60 | 17 | `[BUILDS ONLY, UNTESTED]` | Verified on silicon | [Run Log](../tests/results/mpeg4_720p30/run.log) / [DMESG](../tests/results/mpeg4_720p30/dmesg.log) | 12.5 FPS; 17 frames decoded before early EOS |
| `h264_main_720p30.264` | H.264 Main | 1280×720 | 60 | 0 | `[BUILDS ONLY, UNTESTED]` | Stalls on DPB buffer wait | [Run Log](../tests/results/h264_main_720p30/run.log) / [DMESG](../tests/results/h264_main_720p30/dmesg.log) | Decodes 28/60 frames when extra buffers (+7) allocated |
| `h264_high_1080p60.264` | H.264 High | 1920×1080 | 120 | 0 | `[BUILDS ONLY, UNTESTED]` | Early EOS / DPB starvation | [Analysis](../tests/results/h264_high_1080p60/ANALYSIS.md) / [Run Log](../tests/results/h264_high_1080p60/run.log) / [DMESG](../tests/results/h264_high_1080p60/dmesg.log) | Decodes 66/120 in isolated run; stalls in batch run |

### 3.2 Encoder (VENC)

Target capabilities:
- H.265/H.264 at up to 3840×2400@30fps
- 4× simultaneous 1080p30 streams

**Current state:** The device tree node `venc@e8900000` is enabled (`status = "okay"` in `patches/0007-hikey960-vpu-node.patch`). `/dev/hi_venc` probes and registers successfully on Linux 7.1.13. Modern C platform, memory, and regulator drivers (`venc_platform.c`, `venc_memory.c`, `venc_regulator.c`) replaced `drv_venc_intf.S` and `hi_drv_mem.S`. Full hardware encoding ladder is undergoing verification per Work Order 3.

---

## 4. Summary Table

| Subsystem | Status | Notes |
|---|---|---|
| HDMI output (720p60) | `[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]` | Weston running; 72.0 MHz pixel clock |
| HDMI output (1080p60) | `[BUILDS ONLY, UNTESTED]` | Init script configured; display confirmation pending |
| Mali-G71 GPU (Panfrost) | `[VERIFIED ON HARDWARE — docs/07-MULTIMEDIA_AND_GPU.md]` | Weston DRM rendering confirmed, commit ec7993a2 |
| VPU decode (VP8, HEVC, MPEG-2) | `[VERIFIED ON HARDWARE — tests/results/]` | 60/60 frames, SSIM > 0.985 vs CPU reference. H.264 partial. |
| VPU encode (H.264/H.265) | `[BUILDS ONLY, UNTESTED]` | /dev/hi_venc registered, undergoing ladder verification |

