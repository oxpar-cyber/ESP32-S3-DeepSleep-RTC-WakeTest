# ESP32-S3 Deep Sleep + RTC Wake Test

A diagnostic sketch that fully qualifies the low-power subsystem of an
ESP32-S3 (N16R8) development board: deep sleep entry, RTC timer wake,
EXT0 button wake, and RTC slow-memory retention.

Built and validated on an ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM).

---

## What It Tests

| # | Test | Method |
|---|------|--------|
| 1 | Reset reason reporting | `esp_reset_reason()` — distinguishes sleep wake from power-on/sw/brownout resets |
| 2 | RTC memory retention | Counters in `RTC_DATA_ATTR` memory compared across sleep cycles |
| 3 | Sleep duration accuracy | Wall clock via `gettimeofday()` (RTC-backed) captured before sleep and at the first statement after wake |
| 4 | RTC timer wake | `esp_sleep_enable_timer_wakeup()` — 10 s interval |
| 5 | EXT0 button wake | BOOT button on GPIO0, active LOW |

The board cycles: boot → print status → 5 s reading window → deep sleep 10 s →
repeat. After 5 wake cycles it parks and stays awake (solid green LED).

During the reading window you can press BOOT to wake the chip early and
verify the EXT0 path. During sleep, an RTC-domain pull-up on GPIO0 prevents
phantom button wakes (see [Findings](#findings)).

## Hardware

- ESP32-S3 N16R8 dev board (16 MB flash, 8 MB octal PSRAM)
- Onboard WS2812 RGB LED on **GPIO48**
- BOOT button on **GPIO0** (RTC-capable)

No external wiring required.

## Arduino IDE Settings

These matter — wrong settings cause test failures that look like hardware faults:

| Setting | Value | Why |
|---------|-------|-----|
| Board | ESP32S3 Dev Module | — |
| USB CDC On Boot | **Enabled** | Serial output over the USB-C port |
| PSRAM | **OPI PSRAM** | The "8R8" octal PSRAM won't detect otherwise |
| Flash Size | 16MB | Matches the N16 module |

Serial monitor: **115200 baud**

## Sample Output (validated run)

```
=============================================
   ESP32-S3 — Deep Sleep + RTC Wake Test v4
=============================================
  Boot #       : 6
  Reset reason : Wake from deep sleep
  Free heap    : 343212 bytes
  RTC memory   : PASS (boot=6, wakes=5)
  Configured   : 10000 ms
  Measured     : 10052 ms
  Deviation    : +52 ms -> PASS
  Wake source  : RTC timer (10 s elapsed)
```

## Findings

Things learned the hard way during this test — worth knowing before
building sleep-scheduled nodes on this class of board:

### 1. GPIO0 has no usable external pull-up on this board

The BOOT button pull-up lives in the **digital** power domain, which powers
down during deep sleep. If only the digital pull-up holds GPIO0 high, the pin
floats during sleep and falsely triggers EXT0 after a few seconds — the board
"wakes by button" with nobody touching it.

**Fix:** route the pin to the RTC domain and enable the RTC pull-up before
sleeping. The RTC rail stays powered during deep sleep:

```cpp
#include "driver/rtc_io.h"

rtc_gpio_init(GPIO_NUM_0);
rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
rtc_gpio_pullup_en(GPIO_NUM_0);
rtc_gpio_pulldown_dis(GPIO_NUM_0);
```

Without this, expect a phantom EXT0 wake roughly 2–3 seconds into every
sleep cycle.

### 2. Wake overhead is ~50 ms

Every wake pays ROM bootloader + app startup time before sketch code runs.
Measured: **+52 ms** of fixed overhead on a 10 s timer wake. Budget for this
when computing average current draw or heartbeat timing.

### 3. `esp_timer_get_time()` does not survive deep sleep

Deep sleep is effectively a reset — the esp_timer restarts at zero on every
wake, so naive `now - before` math yields negative "sleep durations" equal to
the previous cycle's awake time. Use `gettimeofday()` instead: it is backed
by the RTC, which keeps counting through deep sleep. Read the wake timestamp
as the **first statement** in `setup()`, before any serial-init delays.

### 4. Digital pull-ups die in deep sleep — RTC ones don't

General rule following from #1: any GPIO state you depend on *during* sleep
(input level, pull-up/down) must come from the RTC power domain.

## Project Structure

```
├── deepsleep_rtctest.ino    # the sketch
└── README.md
```

## Results Summary

| Subsystem | Result |
|-----------|--------|
| Deep sleep entry / exit | PASS (rst:0x5 DSLEEP confirmed at ROM level) |
| RTC timer wake (10 s) | PASS (+52 ms incl. bootloader, single-digit ms timer drift) |
| EXT0 button wake | PASS (after RTC pull-up fix) |
| RTC memory retention | PASS (10+ consecutive sleep cycles) |

## License

MIT
