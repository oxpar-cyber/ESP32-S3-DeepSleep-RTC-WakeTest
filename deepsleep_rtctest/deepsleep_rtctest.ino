/**********************************************************************
 * ESP32-S3 Deep Sleep + RTC Wake Test
 * --------------------------------------------------------------
 * Qualifies the low-power subsystem of an ESP32-S3 dev board:
 *
 *   1. Reset reason reporting (sleep wake vs power-on/sw/brownout)
 *   2. RTC slow-memory retention across deep sleep cycles
 *   3. Deep sleep duration accuracy vs the configured timer
 *   4. RTC timer wake (10 s)
 *   5. EXT0 button wake (BOOT on GPIO0, active LOW)
 *
 * Cycle:  boot -> print status -> 5 s reading window -> deep sleep 10 s
 *         ... repeat until 5 wake cycles are recorded, then park.
 *
 * Arduino IDE settings (these matter, see README):
 *   Board             : ESP32S3 Dev Module
 *   USB CDC On Boot   : Enabled
 *   PSRAM             : OPI PSRAM
 *   Flash Size        : 16MB
 *
 * Tested on: ESP32-S3 N16R8 (16 MB flash, 8 MB octal PSRAM)
 * Serial monitor    : 115200 baud
 *
 * SPDX-License-Identifier: MIT
 *********************************************************************/

#include <Arduino.h>
#include <esp_sleep.h>
#include <sys/time.h>       // gettimeofday(): RTC-backed wall clock
#include "driver/rtc_io.h"  // RTC-domain GPIO control for EXT0 pin

// ---------------- Configuration ----------------
const int      RGB_PIN      = 48;              // Onboard WS2812 (some clones: GPIO38)
const int      BTN_PIN      = 0;               // BOOT button (RTC-capable GPIO)
const uint64_t SLEEP_US     = 10ULL * 1000000ULL;  // Sleep interval: 10 seconds
const uint32_t TOL_MS       = 200;             // Timer wake tolerance (ms)
const uint32_t READOUT_MS   = 5000;            // Awake reading window before sleeping
const uint32_t MAX_CYCLES   = 5;               // Successful wake cycles before parking

// ---------------- RTC memory ----------------
// RTC_DATA_ATTR places these in the RTC slow-memory domain, which stays
// powered during deep sleep. They survive sleep/wake cycles but reset
// on any real reset (power cycle, EN button, reflash).
RTC_DATA_ATTR uint32_t        bootCounter   = 0;     // Total boots (cold + wake)
RTC_DATA_ATTR uint32_t        wakeCounter   = 0;     // Boots caused by a sleep-wake
RTC_DATA_ATTR struct timeval  sleptAt       = {0, 0}; // Wall clock at sleep entry

// ---------------- Deep sleep entry ----------------
// Never returns. Arms both wake sources and powers down.
void goDeepSleep() {
  // KEEP GPIO0 HIGH DURING SLEEP.
  // On boards without an external pull-up on GPIO0, the digital pull-up
  // powers down with the main rails during deep sleep and the pin floats,
  // causing phantom EXT0 wakes a few seconds into sleep. Routing the pad
  // to the RTC domain and enabling its pull-up keeps the pin high on the
  // RTC rail, which stays powered throughout deep sleep.
  rtc_gpio_init(GPIO_NUM_0);
  rtc_gpio_set_direction(GPIO_NUM_0, RTC_GPIO_MODE_INPUT_ONLY);
  rtc_gpio_pullup_en(GPIO_NUM_0);
  rtc_gpio_pulldown_dis(GPIO_NUM_0);

  // Record sleep entry time. gettimeofday() is backed by the RTC and keeps
  // counting through deep sleep; esp_timer_get_time() does NOT — it resets
  // to zero on every wake, so do not use it to measure sleep duration.
  gettimeofday(&sleptAt, NULL);

  // Arm both wake sources: whichever fires first wins.
  esp_sleep_enable_timer_wakeup(SLEEP_US);           // RTC timer, 10 s
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_0, 0);       // BOOT button, active LOW

  Serial.println(F("Sleeping now (RTC timer 10 s + EXT0 armed)..."));
  Serial.flush();          // Let the last lines leave before power-down
  esp_deep_sleep_start();  // Does not return
}

// ---------------- Helpers ----------------
const char *resetReasonStr(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON:   return "Power-on reset";
    case ESP_RST_SW:        return "Software reset";
    case ESP_RST_DEEPSLEEP: return "Wake from deep sleep";
    case ESP_RST_BROWNOUT:  return "Brownout reset";
    case ESP_RST_PANIC:     return "Panic reset";
    default:                return "Other";
  }
}

const char *wakeupCauseStr(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_TIMER: return "RTC timer (interval elapsed)";
    case ESP_SLEEP_WAKEUP_EXT0:  return "EXT0 (BOOT button pressed)";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "not a sleep wake";
    default:                      return "unknown";
  }
}

void blinkRgb(uint32_t count) {
  // One dim blue blink per completed wake cycle, capped at 5.
  for (uint32_t i = 0; i < count && i < 5; i++) {
    neopixelWrite(RGB_PIN, 0, 0, 80);
    delay(150);
    neopixelWrite(RGB_PIN, 0, 0, 0);
    delay(150);
  }
}

// ---------------- Main ----------------
void setup() {
  // Capture the wake timestamp FIRST, before any serial-init delays,
  // or they get charged to the measured sleep duration.
  // (Consists of ROM bootloader + app startup overhead: ~50 ms typical.)
  struct timeval wakeTime;
  gettimeofday(&wakeTime, NULL);

  Serial.begin(115200);
  delay(2500);  // Give the USB CDC serial monitor time to attach

  Serial.println();
  Serial.println(F("============================================="));
  Serial.println(F("   ESP32-S3 Deep Sleep + RTC Wake Test"));
  Serial.println(F("============================================="));

  bootCounter++;

  const esp_reset_reason_t reason = esp_reset_reason();
  const bool wokeFromSleep = (reason == ESP_RST_DEEPSLEEP);
  if (wokeFromSleep) wakeCounter++;

  Serial.printf("  Boot #       : %u\n", bootCounter);
  Serial.printf("  Reset reason : %s\n", resetReasonStr(reason));
  Serial.printf("  Free heap    : %u bytes\n", ESP.getFreeHeap());

  if (wokeFromSleep) {
    // ---- Check 1: RTC memory retention ----
    // Every sleep-wake boot must follow exactly one prior boot.
    const bool rtcOk = (bootCounter == wakeCounter + 1);
    Serial.printf("  RTC memory   : %s (boot=%u, wakes=%u)\n",
                  rtcOk ? "PASS" : "FAIL", bootCounter, wakeCounter);

    // ---- Check 2: measured sleep duration ----
    const int64_t sleptMs =
        ((int64_t)wakeTime.tv_sec - sleptAt.tv_sec) * 1000LL +
        ((int64_t)wakeTime.tv_usec - sleptAt.tv_usec) / 1000LL;
    const int64_t targetMs = (int64_t)(SLEEP_US / 1000LL);
    const int64_t errMs = sleptMs - targetMs;
    Serial.printf("  Configured   : %lld ms\n", (long long)targetMs);
    Serial.printf("  Measured     : %lld ms\n", (long long)sleptMs);
    Serial.printf("  Deviation    : %+lld ms -> %s\n", (long long)errMs,
                  (errMs > -(int64_t)TOL_MS && errMs < (int64_t)TOL_MS)
                      ? "PASS" : "outside tolerance");

    // ---- Check 3: wake source attribution ----
    const esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    Serial.printf("  Wake source  : %s\n", wakeupCauseStr(cause));

    // Diagnostic: an early "button" wake you didn't cause means GPIO0 is
    // floating during sleep (missing/weak external pull-up). The RTC
    // pull-up applied in goDeepSleep() should prevent this; if it still
    // occurs, investigate noise coupling on GPIO0.
    if (cause == ESP_SLEEP_WAKEUP_EXT0 && errMs < -1000) {
      Serial.println(F("  NOTE: woke 'by button' well before the timer."));
      Serial.println(F("        If you didn't press BOOT, GPIO0 is floating."));
    }
  } else {
    Serial.println(F("  Status       : cold boot (not a sleep wake)"));
  }

  blinkRgb(wakeCounter);

  Serial.println();
  Serial.println(F("  Reading window: 5 s before sleeping..."));
  Serial.println(F("  Leave the board untouched to test the timer, or"));
  Serial.println(F("  press BOOT during sleep to test the EXT0 path."));
  Serial.println();
  delay(READOUT_MS);

  if (bootCounter > MAX_CYCLES) {
    Serial.println(F("  5 wake cycles recorded — TEST COMPLETE. Parking."));
    Serial.println(F("  Wake sources disarmed; unplug or reflash to rerun."));
    neopixelWrite(RGB_PIN, 0, 80, 0);   // Solid green = test finished
    while (true) { delay(1000); }       // Stay awake; RTC memory now resets on next power cycle
  }

  goDeepSleep();
}

void loop() {
  // Intentionally empty: every wake restarts at setup() because deep
  // sleep is a reset, not a suspend.
}
