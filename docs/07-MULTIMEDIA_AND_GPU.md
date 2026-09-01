# Multimedia, GPU, and HDMI (Headless Configuration)

The HiKey960 was originally designed as an Android reference board, featuring a powerful Mali-G71 MP8 GPU and a physical HDMI port. However, running a modern mainline Linux kernel (5.x - 7.x) introduces several architectural compromises. 

In this repository, the system is strictly configured as a **headless server**. This document explains the technical rationale behind disabling the multimedia subsystems.

## 1. HDMI Output (Black Screen)
If you connect an HDMI cable to the board, you will receive no signal. This is intentional.
*   **Hardware Architecture:** The Kirin 960 SoC does not have a native HDMI controller. Instead, it outputs a MIPI DSI signal which is converted into HDMI by an onboard **Analog Devices ADV7533** bridge chip.
*   **Software Blocker:** To drive the ADV7533, the kernel's Direct Rendering Manager (DRM) and the `kirin-dsi` subsystem must be active. Due to severe stability issues with the Kirin DRM driver on mainline Linux, the entire DRM pipeline is explicitly disabled. Without the DSI stream, the HDMI bridge has no data to output.

## 2. Mali-G71 GPU (Panfrost)
The board features an ARM Mali-G71 (Bifrost architecture) GPU.
*   **Open Source Drivers:** The mainline Linux kernel provides the `panfrost` driver, which supports Bifrost GPUs. 
*   **Why it is disabled:** On the HiKey960, initializing the GPU alongside the Kirin memory management unit and power domains frequently leads to hard kernel panics (`SError` interrupts) or complete system freezes when under load. Because rock-solid uptime is the priority for this server build, `CONFIG_DRM_PANFROST` is completely unset in the kernel configuration. The GPU is electrically powered down.

## 3. Hardware Video Acceleration (VPU)
*   **Decoding/Encoding:** The Kirin 960 hardware video decoders require proprietary Huawei binary blobs (traditionally using the OpenMAX/OMX API from Android 8/9).
*   **Mainline Status:** These closed-source blobs cannot be integrated into modern V4L2 (Video for Linux 2) or Codec2 frameworks. Consequently, all video decoding or encoding on this board must be performed purely in software by the CPU (A73/A53 cores).

## Summary
To utilize the GPU, HDMI port, and hardware video acceleration, you must run the deprecated, vendor-provided Android AOSP kernels (e.g., Linux 4.4 or 4.14). On modern mainline Linux, attempting to enable these features will severely compromise system stability.
