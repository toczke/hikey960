# 40-Pin Low Speed (LS) Expansion Header

The HiKey960 features a 96Boards-compliant 40-pin Low Speed Expansion Connector. By default, standard GPIOs, UART, and I2C are fully supported in the mainline kernel. Advanced features like SPI and PWM require specific hardware considerations and Device Tree (DTB) patches.

## Hardware Specifications and Pinout

### ⚠️ Critical Voltage Warning
**All logic pins on the 40-pin Low Speed Expansion Connector operate strictly at 1.8V.**
Interfacing directly with 3.3V or 5V components (e.g., standard PC fans, 5V sensors) without a logic level shifter will permanently damage the Hi3660 SoC. Always use a 1.8V to 3.3V/5V bidirectional level shifter.

### Pinout Table

| PIN | 96Boards Signal | HiKey960 Signal | PIN | 96Boards Signal | HiKey960 Signal |
|:---:|:---|:---|:---:|:---|:---|
| 1 | GND | GND | 2 | GND | GND |
| 3 | UART0_CTS | UART3_CTS_N | 4 | PWR_BTN_N | PWRON_N |
| 5 | UART0_TxD | UART3_TXD | 6 | RST_BTN_N | EXP_RSTOUT_N |
| 7 | UART0_RxD | UART3_RXD | 8 | SPI0_SCLK | SPI2_CLK |
| 9 | UART0_RTS | UART3_RTS_N | 10 | SPI0_DIN | SPI2_DI |
| 11 | UART1_TxD | UART6_TXD | 12 | SPI0_CS | SPI2_CS_N |
| 13 | UART1_RxD | UART6_RXD | 14 | SPI0_DOUT | SPI2_DO |
| 15 | I2C0_SCL | I2C0_SCL | 16 | PCM_FS | GPIO_195_I2S0_XFS |
| 17 | I2C0_SDA | I2C0_SDA | 18 | PCM_CLK | GPIO_194_I2S0_XCLK |
| 19 | I2C1_SCL | I2C7_SCL | 20 | PCM_DO | GPIO_193_I2S0_DO |
| 21 | I2C1_SDA | I2C7_SDA | 22 | PCM_DI | GPIO_192_I2S0_DI |
| 23 | GPIO-A | GPIO_208 | 24 | GPIO-B | GPIO_209 |
| 25 | GPIO-C | GPIO_210 | 26 | GPIO-D | GPIO_211 |
| 27 | GPIO-E | GPIO_212 | 28 | GPIO-F | LCD_BL_PWM |
| 29 | GPIO-G | LCD_TE0 | 30 | GPIO-H | GPIO_040_LCD_RST_N |
| 31 | GPIO-I | GPIO_052_CAM0_RST_N | 32 | GPIO-J | GPIO_019 |
| 33 | GPIO-K | GPIO_075_CAM1_RST_N | 34 | GPIO-L | GPIO_021 |
| 35 | +1V8 | VOUT11_1V8/2V95 | 36 | SYS_DCIN | SYSDC_IN |
| 37 | +5V | SYS_5V | 38 | SYS_DCIN | SYSDC_IN |
| 39 | GND | GND | 40 | GND | GND |

## Interface Configuration

### SPI (Serial Peripheral Interface)
While the `spi2` controller (mapped to pins 8, 10, 12, 14) is initialized by the kernel, user-space access (`/dev/spidevX.X`) is disabled by default. 
To enable it, you must decompile the active DTB, locate the `spi@ff3b3000` node, and append a `spidev` subnode:

```dts
spi@ff3b3000 {
    status = "okay";
    spidev@0 {
        compatible = "rohm,dh2228fv";
        reg = <0x00>;
        spi-max-frequency = <10000000>;
    };
};
```
*Note: Ensure no duplicate `status` properties exist before recompiling the DTB.*

### PWM (Pulse Width Modulation)
`[VERIFIED ON HARDWARE — Linux 7.2.6, physical Kirin 960 silicon at root@192.168.0.165]`

Pin 28 (`LCD_BL_PWM` / `GPIO_182`) on the Low-Speed (LS) 40-pin expansion header is electrically wired to Channel 0 of the SoC's hardware PWM generator (`pwm@e8a04000`).

In upstream and vendor kernel trees, the Kirin 960 hardware PWM block was unmapped, and the PWM clock gate was missing from the CRG clock driver. We developed a modernized Linux 7.2 PWM controller driver (`drivers/pwm/pwm-hisi.c`), registered the 20 MHz parent clock (`clk_factor_ptp` / `HI3660_CLK_FACTOR_PTP`) and PWM clock gate (`HI3660_CLK_GATE_PWM` @ CRG offset `0x20` bit 0), and mapped the device tree node with pinmux `0x010 MUX_M1` (`pwm_pmx_func`) in `patches/0009-hikey960-pwm-controller-and-clock.patch`.

#### Hardware Register Map & Constraints:
* **Base Address:** `0xE8A04000` (Channels: Channel 0 @ Pin 28, Channel 1 internal).
* **Clock Source:** 20 MHz (`clk_div_320m` divided by 16). 1 tick = 50 ns.
* **Period / Duty Resolution:** 50 ns resolution up to >200 ms.
* **Bus Safety Rule:** Never access unmapped MMIO offsets inside `0xE8A04000` (e.g. `0x0C`), or access registers when the clock gate is disabled; doing so triggers an asynchronous AXI DECERR/SError bus fault. The driver keeps the clock safely enabled throughout its active lifecycle.

#### Userspace Usage (sysfs):
```bash
# Export channel 0 (Pin 28)
echo 0 > /sys/class/pwm/pwmchip0/export

# Set 1 kHz frequency (period: 1,000,000 ns) with 50% duty cycle (500,000 ns)
echo 1000000 > /sys/class/pwm/pwmchip0/pwm0/period
echo 500000 > /sys/class/pwm/pwmchip0/pwm0/duty_cycle

# Enable PWM output
echo 1 > /sys/class/pwm/pwmchip0/pwm0/enable

# Disable and unexport
echo 0 > /sys/class/pwm/pwmchip0/pwm0/enable
echo 0 > /sys/class/pwm/pwmchip0/unexport
```
*Note: Pin 28 logic level is 1.8V. Do NOT connect 3.3V or 5V loads directly.*

### I2C and UART
* **I2C:** Buses `i2c-0` and `i2c-1` (internal `i2c-7`) are active and automatically exported by the kernel.
* **UART:** Console (`ttyAMA6`) and secondary UART (`ttyAMA3`) are fully functional and require no additional configuration.

## Onboard Status LEDs (Sysfs GPIO)

The HiKey960 features 6 onboard diagnostic LEDs that are directly wired to the SoC's GPIO pins and exposed to Linux via the generic LED subsystem. These are highly useful for headless servers to monitor system state.

### Available LEDs
*   `green:user1` through `green:user4`
*   `yellow:wlan`
*   `blue:bt`

### Configuration and Triggers
You can dynamically configure these LEDs without Device Tree patches by writing to their respective sysfs paths (`/sys/class/leds/<led_name>/`).

**1. Hardware Triggers:**
By loading standard kernel modules (`modprobe ledtrig-heartbeat ledtrig-activity`), you can assign kernel events directly to the LEDs:
```bash
echo heartbeat > /sys/class/leds/green:user1/trigger
echo cpu > /sys/class/leds/green:user2/trigger
```

**2. Manual Userspace Control:**
For custom monitoring (e.g., thermal alarms or network ping watchdogs), disable the trigger and control the brightness directly (0 = Off, 1-255 = On):
```bash
echo none > /sys/class/leds/blue:bt/trigger
echo 1 > /sys/class/leds/blue:bt/brightness
```
*Note: The LEDs on this board are connected to simple binary GPIO pins without hardware PWM support. The `max_brightness` is 1, meaning software dimming is not supported.*
