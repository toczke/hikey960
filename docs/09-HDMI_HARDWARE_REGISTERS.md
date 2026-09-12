# HDMI Hardware Registers Reference (ADV7533 + DPE)

> **Doc integrity notice [2026-09-08]:** This document was created during Phase 0 of the VPU bring-up work order.
> All register values listed here are either `[VERIFIED ON HARDWARE]` (read/written from real board) or `[UNRESOLVED]` (conflicting information, pending a full register dump).
>
> A complete register dump of ADV7533 pages `0x39` and `0x3c` must be captured and saved to
> `docs/register-dumps/adv7533-live-<date>.txt` before resolving the `[UNRESOLVED]` items below.

---

## 1. Chip Identity

**Chip: ADV7533** (NOT ADV7535)

`[VERIFIED ON HARDWARE]` — Confirmed by direct I2C reads:

```bash
i2cget -f -y 1 0x3c 0x00   # → 0x75  (Chip Revision high byte)
i2cget -f -y 1 0x3c 0x01   # → 0x33  (Chip Revision low byte)
```

The value pair `0x75 0x33` uniquely identifies the **ADV7533**. The ADV7535 would return different bytes. Any prior documentation or script comment referring to "ADV7535" is incorrect and has been removed.

---

## 2. I2C Address Map

| Address | Function |
|---|---|
| `0x39` | Main / HDMI register page |
| `0x3c` | CEC / DSI-side register page |

---

## 3. Registers Currently Written by `hikey960-hdmi-init.sh`

All values below are `[VERIFIED ON HARDWARE]` — they are the live values written at boot and confirmed not to cause lockup or HDCP enable.

### Page `0x3c` (CEC/DSI side)

| Register | Value | Description | Source |
|---|---|---|---|
| `0x16` | `0x18` | PLL clock divider — configured for 4 DSI lanes | `hikey960-hdmi-init.sh` line 77, commit ec7993a2 |
| `0x55` | `0x00` | CEC clock divider reset | `hikey960-hdmi-init.sh` line 78 |
| `0x27` | `0x0b` | Timing generator bypass (use Kirin DPE timing source) | `hikey960-hdmi-init.sh` line 79 |

#### Register `0x1c` (DSI lane count) — `[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]`

- **Measured live value:** `0x40`
- **Bit encoding:** Bits [6:4] encode lane count: `lanes << 4` (so `4 << 4 = 0x40` for 4 lanes).
- **Resolution of prior conflict:** Prior documentation had an erroneous claim that upstream's `0x40` was a bug and should be `0xc0` (`(lanes-1)<<6`). Direct hardware measurement confirmed that the register reads `0x40` on the active, working display pipeline with TMDS locked. The `0xc0` claim has been removed as incorrect.

### Page `0x39` (Main/HDMI side)

| Register | Value | Description | Source |
|---|---|---|---|
| `0xd6` | `0x50` | TMDS transmitter ON + HPD detect override | `hikey960-hdmi-init.sh` line 84, commit ec7993a2 |
| `0xaf` | `0x02` | HDMI mode, HDCP encryption OFF | `hikey960-hdmi-init.sh` line 85, commit ec7993a2 |
| `0x16` | `0x20` | Input color format: RGB 4:4:4 | `hikey960-hdmi-init.sh` line 86 |
| `0x44` | `0x10` | AVI InfoFrame packet enable | `hikey960-hdmi-init.sh` line 87 |

#### Register `0x9e` (TMDS/PLL lock status) — `[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]`

- **Measured live value:** `0x14`
- **Bit breakdown:**
  - Bit 4 (`0x10`): TMDS clock detected
  - Bit 2 (`0x04`): ADV7533 internal PLL locked
- **Conclusion:** Confirms the ADV7533 PLL and TMDS clock generation are actively locked and operating under the deployed configuration (`0x3c 0x16=0x18`, `0x3c 0x1c=0x40`).

---

## 4. DPE (Display Processing Engine) Registers

`[VERIFIED ON HARDWARE — commit ec7993a2]`

Base address: `0xe8600000`

| Address | Value Written | Description |
|---|---|---|
| `0xe867d000` | `0x00ef0050` (720p) / `0x00bf0058` (1080p) | LDI HRZ_CTRL0: horizontal front/back porch |
| `0xe8601048` | `0x0000001f` (720p) / `0x00000021` (1080p) | DSI HSA (horizontal sync active) |
| `0xe860104c` | `0x00000099` (720p) / `0x0000006f` (1080p) | DSI HBP (horizontal back porch) |
| `0xe8601050` | `0x000004cb` (720p) / `0x00000672` (1080p) | DSI HLINE (total horizontal line length) |
| `0xe867d024` | `0x00002ec1` | LDI display mode — normal scan of fb0 |
| `0xe867d028` | `0x00000001` | LDI enable |

---

## 5. Safe Register Write Protocol

> **CRITICAL:** Do not issue `i2cset 0x39 0x41 0x50` — this is the power-down command and resets ALL registers including `0xaf`, causing the HDCP bit to be re-set and the TV/monitor to reject the HDMI stream. This bug existed in the original init script and was fixed in commit ec7993a2.

The correct sequence for initializing TMDS and disabling HDCP without power-cycling:
1. Write `0x3c 0x16 0x18` (PLL/lane config)
2. Write `0x3c 0x55 0x00` (CEC reset)
3. Write `0x3c 0x27 0x0b` (timing bypass)
4. Write `0x39 0xd6 0x50` (TMDS ON)
5. Write `0x39 0xaf 0x02` (HDMI mode, HDCP OFF)
6. Write `0x39 0x16 0x20` (RGB 4:4:4)
7. Write `0x39 0x44 0x10` (AVI InfoFrame)

---

## 6. Full Register Dump
`[VERIFIED ON HARDWARE — docs/register-dumps/adv7533-live-2026-09-12.txt]`

A complete dump of both ADV7533 pages (`0x39` and `0x3c`) was captured from the running board with HDMI attached and Weston desktop active on 2026-09-12.

Key findings:
1. **Lane Count (`0x3c 0x1c`):** Reads `0x40`. Confirms `lanes << 4` (`4 << 4 = 0x40`). The conflicting claim of `0xc0` has been removed.
2. **PLL/TMDS Lock (`0x39 0x9e`):** Reads `0x14` (Bit 4 = TMDS clock detected, Bit 2 = PLL locked). Confirms stable active clocking.
3. **Chip ID (`0x3c 0x00`, `0x01`):** Reads `0x75 0x33`, verifying the chip is ADV7533 (not ADV7535).
4. **HPD & State (`0x39 0x42`):** Reads `0xf0` (HPD high, monitor sense active).

The full raw dump is stored at:
[`docs/register-dumps/adv7533-live-2026-09-12.txt`](docs/register-dumps/adv7533-live-2026-09-12.txt)
