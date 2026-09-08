# HiKey960 HDMI & ADV7533 Hardware Register Reference

This document provides a comprehensive breakdown of the hardware video pipeline, I2C register maps, clock configurations, and known working values for the **HiKey960 (Huawei Kirin 960 / Hi3660)** HDMI display subsystem.

---

## 1. Hardware Pipeline Architecture

The display pipeline consists of four distinct hardware stages:

```
+---------------------------+
|  Kirin 960 SoC (Hi3660)   |
|  Display Processing Engine|
|  (DPE) @ 0xE8600000       |
+-------------+-------------+
              |
              v (Parallel RGB / Pixel Stream)
+-------------+-------------+
|  DesignWare MIPI DSI Host |
|  @ 0xE8601000             |
+-------------+-------------+
              |
              v (MIPI DSI: 4 Data Lanes + 1 Clock Lane)
+-------------+-------------+
|  DSI Hardware MUX (U201)  | <--- Controlled by GPIO20 (&gpio2 4)
+-------------+-------------+      LOW (0)  = Route to ADV7533 (HDMI)
              |                    HIGH (1) = Route to 60-pin HS Header
              v
+-------------+-------------+
|  Analog Devices ADV7533   |
|  DSI-to-HDMI Bridge       | <--- I2C Bus 1: Main (0x39), CEC/DSI (0x3c)
+-------------+-------------+
              |
              v (TMDS Clock + 3 Data Pairs: Red, Green, Blue)
+-------------+-------------+
|  Physical HDMI-A Connector| ===> External Monitor / TV
+---------------------------+
```

---

## 2. I2C Bus and Slave Addresses

The ADV7533 bridge chip is connected to **I2C Bus 1** (`/soc/i2c@ffd72000`). It exposes multiple I2C 7-bit slave addresses:

| I2C Address (7-bit) | Linux Sysfs Node | Purpose / Page Description |
| :--- | :--- | :--- |
| **`0x39`** | `1-0039` | **Main Register Page** (Power, Video Input Format, HDCP, Clock Delay, ADI Fixed) |
| **`0x3c`** | `1-003c` | **CEC / DSI Control Page** (DSI Lane Count, Timing Gen, HDMI Output Enable) |
| **`0x3f`** | `1-003f` | **EDID / DDC Read Page** (I2C pass-through to monitor's 24Cxx EDID EEPROM) |

---

## 3. ADV7533 Main Page (`0x39`) Working Register Map

Below is the verified working register map for the main register page (`0x39`):

| Register (Hex) | Working Value | Name / Description | Bitfield Breakdown & Meaning |
| :--- | :--- | :--- | :--- |
| **`0x16`** | `0x20` | Input Format | `[7:6]=00` (RGB), `[5:4]=10` (4:4:4), `[3:2]=00` (8-bit), `[1:0]=00` |
| **`0x41`** | `0x10` | Power Down Control | `[6]=0` (Powered Normal / Active), `[4]=1` (Reserved bit must be 1) |
| **`0x42`** | `0xf0` | HPD & Monitor Status *(Read-Only)* | `[7]=1` (Interrupt), `[6]=1` (HPD High), `[5]=1` (HPD state), `[4]=1` (MSEN detected) |
| **`0x55`** | `0x02` | ADI Fixed Register | ADI recommended fixed value |
| **`0x98`** | `0x03` | ADI Fixed Register | ADI recommended fixed value |
| **`0x9a`** | `0xe0` | ADI Fixed Register | ADI recommended fixed value |
| **`0x9c`** | `0x30` | ADI Fixed Register | ADI recommended fixed value |
| **`0x9d`** | `0x61` | ADI Fixed Register | ADI recommended fixed value |
| **`0x9e`** | `0x18` / `0x14` | PLL & TMDS Lock Status *(Read-Only)* | `[4]=1` (DSI PLL Locked!), `[3]=1` (TMDS Clock Locked at 1080p), `[2]=1` (Clock Detect) |
| **`0xa2`** | `0xa4` | ADI Fixed Register | ADI recommended fixed value |
| **`0xa3`** | `0xa4` | ADI Fixed Register | ADI recommended fixed value |
| **`0xaf`** | `0x16` | HDCP / HDMI Mode | `[1]=1` (HDMI Mode enabled; 0=DVI), `[2]=1` (Frame encryption) |
| **`0xba`** | `0x70` | Clock Delay / Input Phase | `[7:5]=011` (Input clock delay compensation for DSI receiver) |
| **`0xd6`** | `0xd0` | **TMDS Output Enable Control** *(CRITICAL)* | `[7]=1`, `[6]=1`, `[4]=1` (**TMDS Transmitters Forced Active**). If TV input is cycled, chip resets this to `0x40` (disabling output). Restored by watchdog service. |
| **`0xde`** | `0x82` | ADI Fixed Register | ADI recommended fixed value |
| **`0xe0`** | `0xd0` | ADI Fixed Register | ADI recommended fixed value |
| **`0xe4`** | `0x40` | ADI Fixed Register | ADI recommended fixed value |
| **`0xe5`** | `0x80` | ADI Fixed Register | ADI recommended fixed value |
| **`0xf9`** | `0x00` | ADI Fixed Register | ADI recommended fixed value |

---

## 4. ADV7533 CEC / DSI Control Page (`0x3c`) Working Register Map

The second register page (`0x3c`) controls the MIPI-DSI receiver and internal timing generation:

| Register (Hex) | Working Value | Name / Description | Bitfield Breakdown & Meaning |
| :--- | :--- | :--- | :--- |
| **`0x00`** | `0x75` | Chip ID High *(Read-Only)* | Upper byte of ADV7533 identification |
| **`0x01`** | `0x33` | Chip ID Low *(Read-Only)* | Lower byte of ADV7533 identification (`75 33` = ADV7533) |
| **`0x03`** | `0x89` | HDMI Enable | `[7]=1` (HDMI Output Enabled), `[3]=1`, `[0]=1` |
| **`0x05`** | `0xc8` | CEC Fixed Sequence | ADI recommended fixed value |
| **`0x15`** | `0xd0` | CEC Fixed Sequence | ADI recommended fixed value |
| **`0x17`** | `0xd0` | CEC Fixed Sequence | ADI recommended fixed value |
| **`0x1c`** | `0x40` | **DSI Lane Count** *(CRITICAL)* | **`lanes << 4` encoding**: `1 lane = 0x10`, `2 lanes = 0x20`, `3 lanes = 0x30`, **`4 lanes = 0x40`**. *(Do NOT write `0xc0`; `0xc0` disables the receiver).* |
| **`0x24`** | `0x20` | CEC Fixed Sequence | ADI recommended fixed value |
| **`0x27`** | `0x0b` | Timing Generator Control | `0x0b` = **Internal Timing Generator Disabled** (Bypass mode: passes video timings directly from Kirin DPE). |
| **`0x55`** | `0x00` | Test Mode | `0x00` = Normal mode (test pattern generation disabled) |
| **`0x57`** | `0x11` | CEC Fixed Sequence | ADI recommended fixed value |

---

## 5. DSI Clocks & Video Timing Matrix

| Parameter | Standard 720p60 | Standard 1080p60 | Downstream HiKey960 Note |
| :--- | :--- | :--- | :--- |
| **Active Resolution** | 1280 x 720 | 1920 x 1080 | Progressive scan |
| **Frame Rate** | 60.00 Hz | 60.00 Hz | Standard consumer TV rate |
| **Pixel Clock (`pixel_clk`)** | **74.250 MHz** | **148.500 MHz** | Required by HDMI Spec 1.4b |
| **DPE Clock (`clk_ldi0`)** | 72.000 MHz | 144.000 MHz | Downstream HiKey960 PLL constraint (2.88 GHz / 20 / 40) |
| **DSI Lane Rate** | 441.6 Mbps / lane | 883.2 Mbps / lane | (Kirin 960 PHY multiplier) |
| **DSI Lane Byte Clock** | 55.200 MHz | 110.400 MHz | `lane_clock / 8` |
| **DSI Lanes Active** | 4 lanes | 4 lanes | Encoded in reg `0x1c` as `0x40` |

> [!WARNING]
> **Clock Discrepancy (144 MHz vs 148.5 MHz):**
> The Kirin 960 `clk_ppll2` runs at 2880 MHz. Dividing by 20 produces exactly **144.0 MHz** instead of the HDMI standard **148.5 MHz**.
> While flexible PC computer monitors lock onto 144 MHz without issue, strict consumer televisions (such as Philips / Samsung) measure the horizontal sync frequency and reject 144 MHz as an **"Unsupported Video Format"** (`Format wideo nieobsługiwany`).

---

## 6. Diagnostic & Recovery Commands

Run these commands directly on the board to inspect and verify hardware health:

```bash
# 1. Check if monitor is physically connected (HPD & MSEN)
# Expected output: 0xf0 (Bits [6:4] high)
i2cget -f -y 1 0x39 0x42

# 2. Check ADV7533 DSI PLL Lock status
# Expected output: 0x14 or 0x12 (Bit 4 = 1 means PLL is locked)
i2cget -f -y 1 0x39 0x9e

# 3. Check DSI Lane Count
# Expected output: 0x40 (4 lanes)
i2cget -f -y 1 0x3c 0x1c

# 4. Check HDMI Output Enable
# Expected output: 0x89 (HDMI active)
i2cget -f -y 1 0x3c 0x03

# 5. Full Register Refresh Script
/usr/local/bin/adv7533-refresh.sh
```
