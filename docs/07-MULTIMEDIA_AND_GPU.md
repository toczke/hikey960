# Multimedia, GPU, and HDMI

The HiKey960 features a powerful ARM Mali-G71 MP8 GPU and a physical HDMI output port. While running a modern mainline Linux kernel (7.x) originally posed significant stability challenges for these subsystems, the `feature/kirin960-drm-rewrite` branch has successfully resurrected full 3D hardware acceleration and display output.

## 1. HDMI Output (Kirin DRM) - Fully Operational
The HDMI pipeline has been ported to modern Linux 7.1 KMS on the `feature/kirin960-drm-rewrite` branch, fully verified on physical hardware, and is now **production ready**:
*   **Hardware Architecture:** The Kirin 960 SoC does not have a native HDMI controller. Instead, the display pipeline consists of three interconnected hardware blocks:
    *   **DPE** (Display Processing Engine) - The core 2D/composition rendering engine.
    *   **DSI** (Display Serial Interface) - Synopsys DesignWare MIPI DSI output interface.
    *   **ADV7533** (Analog Devices) - An onboard bridge chip that converts the 4-lane MIPI DSI stream into physical HDMI signals.
*   **Modern Linux 7.1 Porting:** 
    *   Replaced legacy Android 4.9 `drm_fb_cma_helper` with modern `drm_gem_dma_helper` APIs.
    *   Adapted atomic mode setting helpers and state management to Linux 7.1.
    *   Renamed driver namespace to `"kirin960-drm"` to avoid symbol conflicts with mainline Kirin 620 (`"kirin-drm"`).
*   **Device Tree Integration:** Upstream Linux lacked memory ranges and port graphs for the DPE and DSI blocks. We injected `hisilicon,hi3660-dpe` and `hisilicon,hi3660-dsi` nodes into `hi3660.dtsi` / `hi3660-hikey960.dts`, routing endpoints: `dpe:port@0` ➔ `dsi:port@0`, `dsi:port@1` ➔ `adv7533:port@0`, and bound the DSI multiplexer GPIO (`mux-gpios = <&gpio2 4 1>;`) to permanently drive GPIO20 LOW.
*   **The Private SMMU & TrustZone Firewall Challenge:**
    *   The Kirin 960 DPE contains a private, integrated SMMU IP block at offset `0x8000`.
    *   In the 4.9 vendor kernel, HiSilicon used out-of-tree `iommu_paging_domain_alloc()` to extract physical page directory bases (`phy_pgd_base`) directly into hardware register `SMMU_CB_TTBR0`. Because modern Linux hides internal IOMMU page tables, simple global SMMU bypass was initially attempted.
    *   **The AXI Lockup Discovery:** Completely bypassing the SMMU caused the Memory Controller (DMC) TrustZone firewall to block raw DMA transactions from DPE (`DECERR`), permanently stalling the AXI interconnect and locking up the SoC at boot.
    *   **The Solution (Identity-Mapped LPAE Mini-Page Table):** To satisfy TrustZone without rewriting the entire Linux IOMMU subsystem, the driver allocates a 32-byte coherent DMA buffer during probe, populates it with four 1GB ARM LPAE Long Descriptor block entries (identity-mapping 0x00000000–0xFFFFFFFF 1:1 with `Inner Shareable` / `Non-Secure` attributes: `0x5C1`), and points `SMMU_CB_TTBR0` to this table. This allows the SMMU to authorize the DPE's DMA access to the framebuffer without bus stalls.
    *   **Direct Physical CMA Memory Addressing (Build 79):** In `hisi_fb_pan_display()` and `hisi_dss_online_play()`, setting `mmu_enable = false` configures the RDMA channels to read directly from physical contiguous memory (`0xdc200000`), completely eliminating DPE RDMA underflows (`LDI_INTS = 0x0`).
*   **Physical Display Breakthrough & Frequency Synchronization:**
    *   **Hardware Clock Constraint:** The Kirin 960 DPE pixel clock `clk_div_ldi0` is derived from `ppll2` (2880 MHz) via integer dividers. It can produce **72.0 MHz** (divider 40) or **144.0 MHz** (divider 20), but cannot produce standard 74.25 MHz or 148.5 MHz.
    *   **Horizontal Line Recalibration:** Standard 720p60 timings (`htotal = 1650`) produced 43.636 kHz horizontal frequency and 58.18 Hz vertical refresh rate, causing TVs to report *"Video format not supported"*. By tuning horizontal blanking to `htotal = 1600` (`hfp = 80, hsw = 40, hbp = 200`), the horizontal frequency becomes **exact 45.000 kHz** and the frame rate becomes **exact 60.000 Hz**, instantly recognized by any HDMI monitor or TV.
    *   **ADV7533 80 MHz Silicon Limit:** The onboard ADV7533 has a maximum hardware pixel clock limit of **80 MHz**. This permits stable **720p @ 60Hz** (72.0 MHz < 80 MHz), which modern displays scale cleanly to 1080p. Standard 1080p60 (requiring 148.5 MHz / 144.0 MHz) exceeds the chip's physical PLL limit and is hardware-unsupported on the ADV7533.
    *   **Color Channel Correction (RGB vs BGR):** Corrected red/blue color swapping on physical HDMI output by setting `acrtc->bgr_fmt = LCD_BGR` (setting bit 13 of `LDI_CTRL` to 1), mapping color components accurately to the ADV7533 input.
    *   **Automated Console Service:** Deployed `hikey960-hdmi-init.service` (`scripts/hikey960-hdmi-init.sh`) to automatically configure DPE line timings, set ADV7533 TMDS output driver (`0xd6 = 0x50`), and unblank the console on boot, providing an out-of-the-box interactive bash shell on `/dev/tty1`.

## 2. Mali-G71 GPU (Panfrost)
The ARM Mali-G71 (Bifrost architecture) GPU is fully initialized and accelerated.
*   **Open Source Drivers:** We utilize the mainline `panfrost` driver, which natively supports Bifrost GPUs.
*   **Boot Stability Fix:** Previously, attempting to initialize the GPU alongside the Kirin memory management unit led to hard kernel panics (`SError`) at `[ 0.000000]` during boot. This was traced to a CMA (Contiguous Memory Allocator) collision. By reducing `CONFIG_CMA_SIZE_MBYTES` from 256MB to 64MB, the kernel boots flawlessly and Panfrost successfully powers up the 8-core Mali-G71.

## 3. Hardware Video Acceleration (VPU)
*   **Decoding/Encoding:** The Kirin 960 hardware video decoders require proprietary Huawei binary blobs (traditionally using the OpenMAX/OMX API from Android 8/9).
*   **Mainline Status:** These closed-source blobs cannot be integrated into modern V4L2 (Video for Linux 2) or Codec2 frameworks. Consequently, all video decoding or encoding on this board must be performed purely in software by the CPU (A73/A53 cores). This is the only multimedia subsystem that remains permanently disabled on mainline Linux.

## Summary
The modern kernel build on this repository statically embeds (`CONFIG_DRM=y`) both the Panfrost GPU driver and the custom Kirin960 DRM driver. The pipeline successfully binds DPE, DSI, and the ADV7533 bridge, allocates the framebuffer device (`kirindrmfb`), maps physical CMA memory, and cleanly displays the interactive serial/tty1 console on external HDMI displays.
