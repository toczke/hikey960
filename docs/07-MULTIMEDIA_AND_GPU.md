# Multimedia, GPU, and HDMI

The HiKey960 features a powerful ARM Mali-G71 MP8 GPU and a physical HDMI output port. While running a modern mainline Linux kernel (7.x) originally posed significant stability challenges for these subsystems, the `feature/kirin960-drm-rewrite` branch has successfully resurrected full 3D hardware acceleration and display output.

## 1. HDMI Output (Kirin DRM) - In Active Testing
The HDMI pipeline has been ported to modern Linux 7.1 KMS on the `feature/kirin960-drm-rewrite` branch and is undergoing live deployment validation:
*   **Hardware Architecture:** The Kirin 960 SoC does not have a native HDMI controller. Instead, the display pipeline consists of three interconnected hardware blocks:
    *   **DPE** (Display Processing Engine) - The core 2D/composition rendering engine.
    *   **DSI** (Display Serial Interface) - Synopsys DesignWare MIPI DSI output interface.
    *   **ADV7533** (Analog Devices) - An onboard bridge chip that converts the 4-lane MIPI DSI stream into physical HDMI signals.
*   **Modern Linux 7.1 Porting:** 
    *   Replaced legacy Android 4.9 `drm_fb_cma_helper` with modern `drm_gem_dma_helper` APIs.
    *   Adapted atomic mode setting helpers and state management to Linux 7.1.
    *   Renamed driver namespace to `"kirin960-drm"` to avoid symbol conflicts with mainline Kirin 620 (`"kirin-drm"`).
*   **Device Tree Integration:** Upstream Linux lacked memory ranges and port graphs for the DPE and DSI blocks. We injected `hisilicon,hi3660-dpe` and `hisilicon,hi3660-dsi` nodes into `hi3660.dtsi` / `hi3660-hikey960.dts`, routing endpoints: `dpe:port@0` ➔ `dsi:port@0`, `dsi:port@1` ➔ `adv7533:port@0`.
*   **The Private SMMU & TrustZone Firewall Challenge:**
    *   The Kirin 960 DPE contains a private, integrated SMMU IP block at offset `0x8000`.
    *   In the 4.9 vendor kernel, HiSilicon used out-of-tree `iommu_paging_domain_alloc()` to extract physical page directory bases (`phy_pgd_base`) directly into hardware register `SMMU_CB_TTBR0`. Because modern Linux hides internal IOMMU page tables, simple global SMMU bypass was initially attempted.
    *   **The AXI Lockup Discovery:** Completely bypassing the SMMU caused the Memory Controller (DMC) TrustZone firewall to block raw DMA transactions from DPE (`DECERR`), permanently stalling the AXI interconnect and locking up the SoC at boot.
    *   **The Solution (Identity-Mapped LPAE Mini-Page Table):** To satisfy TrustZone without rewriting the entire Linux IOMMU subsystem, the driver allocates a 32-byte coherent DMA buffer during probe, populates it with four 1GB ARM LPAE Long Descriptor block entries (identity-mapping 0x00000000–0xFFFFFFFF 1:1 with `Inner Shareable` / `Non-Secure` attributes: `0x5C1`), and points `SMMU_CB_TTBR0` to this table. This allows the SMMU to authorize the DPE's DMA access to the framebuffer (`0xba89c000`) without bus stalls.

## 2. Mali-G71 GPU (Panfrost)
The ARM Mali-G71 (Bifrost architecture) GPU is fully initialized and accelerated.
*   **Open Source Drivers:** We utilize the mainline `panfrost` driver, which natively supports Bifrost GPUs.
*   **Boot Stability Fix:** Previously, attempting to initialize the GPU alongside the Kirin memory management unit led to hard kernel panics (`SError`) at `[ 0.000000]` during boot. This was traced to a CMA (Contiguous Memory Allocator) collision. By reducing `CONFIG_CMA_SIZE_MBYTES` from 256MB to 64MB, the kernel boots flawlessly and Panfrost successfully powers up the 8-core Mali-G71.

## 3. Hardware Video Acceleration (VPU)
*   **Decoding/Encoding:** The Kirin 960 hardware video decoders require proprietary Huawei binary blobs (traditionally using the OpenMAX/OMX API from Android 8/9).
*   **Mainline Status:** These closed-source blobs cannot be integrated into modern V4L2 (Video for Linux 2) or Codec2 frameworks. Consequently, all video decoding or encoding on this board must be performed purely in software by the CPU (A73/A53 cores). This is the only multimedia subsystem that remains permanently disabled on mainline Linux.

## Summary
The modern kernel build on this repository statically embeds (`CONFIG_DRM=y`) both the Panfrost GPU driver and the custom Kirin960 DRM driver. The pipeline successfully binds DPE, DSI, and the ADV7533 bridge, allocates the framebuffer device (`fb1: kirindrmfb`), and handles SMMU memory translation. Active verification on physical HDMI displays is in progress on `feature/kirin960-drm-rewrite`.
