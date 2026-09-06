# Multimedia, GPU, and HDMI

The HiKey960 features a powerful ARM Mali-G71 MP8 GPU and a physical HDMI output port. While running a modern mainline Linux kernel (7.x) originally posed significant stability challenges for these subsystems, the `feature/kirin960-drm-rewrite` branch has successfully resurrected full 3D hardware acceleration and display output.

## 1. HDMI Output (Kirin DRM)
The HDMI port is fully functional.
*   **Hardware Architecture:** The Kirin 960 SoC does not have a native HDMI controller. Instead, the display pipeline consists of three interconnected hardware blocks:
    *   **DPE** (Display Processing Engine) - The core rendering engine.
    *   **DSI** (Display Serial Interface) - The MIPI output interface.
    *   **ADV7533** (Analog Devices) - An onboard bridge chip that converts the MIPI DSI stream into the physical HDMI signal.
*   **Software Implementation:** The legacy Android 4.9 `kirin-drm` driver was manually ported to the Linux 7.1 KMS API. 
*   **Device Tree Fixes:** Upstream Linux completely lacked the hardware memory maps for the DPE and DSI blocks. We injected the `hisilicon,hi3660-dpe` and `hisilicon,hi3660-dsi` nodes into the Device Tree, alongside the complex port graphs routing the video signal from `dpe` ➔ `dsi` ➔ `adv7533`.
*   **Driver Renaming:** To avoid symbol collisions with the Kirin 620 DRM driver (which shares the `"kirin-drm"` namespace in mainline), the driver was successfully renamed to `"kirin960-drm"`.

## 2. Mali-G71 GPU (Panfrost)
The ARM Mali-G71 (Bifrost architecture) GPU is fully initialized and accelerated.
*   **Open Source Drivers:** We utilize the mainline `panfrost` driver, which natively supports Bifrost GPUs.
*   **Boot Stability Fix:** Previously, attempting to initialize the GPU alongside the Kirin memory management unit led to hard kernel panics (`SError`) at `[ 0.000000]` during boot. This was traced to a CMA (Contiguous Memory Allocator) collision. By reducing `CONFIG_CMA_SIZE_MBYTES` from 256MB to 64MB, the kernel boots flawlessly and Panfrost successfully powers up the 8-core Mali-G71.

## 3. Hardware Video Acceleration (VPU)
*   **Decoding/Encoding:** The Kirin 960 hardware video decoders require proprietary Huawei binary blobs (traditionally using the OpenMAX/OMX API from Android 8/9).
*   **Mainline Status:** These closed-source blobs cannot be integrated into modern V4L2 (Video for Linux 2) or Codec2 frameworks. Consequently, all video decoding or encoding on this board must be performed purely in software by the CPU (A73/A53 cores). This is the only multimedia subsystem that remains permanently disabled on mainline Linux.

## Summary
The modern kernel build on this repository now statically embeds (`CONFIG_DRM=y`) both the Panfrost GPU driver and the custom Kirin960 DRM driver. KMS (Kernel Mode Setting) correctly probes the EDID from connected HDMI displays, allowing the HiKey960 to function as a fully accelerated desktop or media platform.
