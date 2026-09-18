# 60-Pin High Speed (HS) Expansion Header

The HiKey960 features a standard 60-pin High Speed Expansion Connector compliant with the 96Boards specification. This connector is primarily designed for high-bandwidth multimedia applications, exposing the Kirin 960's MIPI lanes.

## Interface Overview

The 60-pin connector provides the following physical lanes:
*   **MIPI DSI (Display Serial Interface):** 1x 4-lane interface.
*   **MIPI CSI (Camera Serial Interface):** 1x 4-lane interface (CSI0) and 1x 2-lane interface (CSI1).
*   **Data Buses:** 1x SPI interface (up to 48MHz), 2x I2C channels.
*   **USB:** USB 2.0 (D+/D-) lines.

### ⚠️ Logic Voltage Constraints
Similar to the Low Speed (LS) header, all logical data buses (SPI, I2C, GPIOs) on this connector operate at **1.8V logic levels**. Direct connections to higher voltage peripherals will damage the SoC.

## Mainline Kernel Support Status

In modern mainline Linux kernels (5.x - 7.x), the vast majority of the High Speed connector's multimedia capabilities are functionally disabled due to proprietary hardware restrictions.

### 1. MIPI CSI (Cameras) - **Unsupported**
*   **Status:** Completely non-functional on mainline Linux.
*   **Technical Blocker:** The Kirin 960 Image Signal Processor (ISP) requires complex, closed-source binary blobs and proprietary Huawei drivers. These drivers were never upstreamed to the V4L2 (Video for Linux 2) subsystem. Consequently, the kernel has no mechanism to process the raw MIPI CSI streams, rendering camera modules on this connector unusable.

### 2. MIPI DSI (Display) - **Routed via Onboard Multiplexer**
*   **Status:** Kirin DRM and Panfrost GPU are fully functional and verified (`CONFIG_DRM_PANFROST=y`, Weston DRM desktop operational on physical hardware).
*   **Technical Details:** The Kirin 960 DSI output is routed through an onboard hardware multiplexer controlled by `mux-gpios = <&gpio2 4 1>` (GPIO20). In the default device tree configuration, this multiplexer directs the 4-lane MIPI DSI output to the onboard ADV7533 HDMI bridge. To redirect DSI signaling to the 60-pin HS connector for an external MIPI display panel, the GPIO multiplexer must be switched and a compatible panel driver added.

### 3. SPI, I2C, and USB - **Workable**
*   **I2C / SPI:** The secondary I2C and SPI buses routed to this connector can be utilized in the same manner as the LS header. They require manual Device Tree (DTB) configuration (e.g., enabling `spidev` on the specific HS SPI controller node).
*   **USB 2.0:** The USB data lines are routed internally and generally function automatically if the DWC2/DWC3 USB PHY controllers are active in the kernel config.
