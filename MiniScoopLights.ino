// Mini hood-scoop lights.
// Ignition on:  strip dark.
// Ignition off: red center-out fill, then a Knight Rider sweep that steps down over time:
//   stage 0 (0–90 min)    full brightness
//   stage 1 (90–150 min)  dimmed
//   stage 2 (150 min+)    dimmed, Long Term Low Power (LTLP): dark pause after every few sweeps

#include <FastLED.h>
#include <avr/wdt.h>

#define PIN_LED 6
#define PIN_IGN 5
#define LEDS_COUNT 21  // Test strip: 14, Car: 21
#define LED_BRIGHT 255
#define LED_BRIGHT_DIM 64                       // Dimmer while in stage 1 & 2
#define DIM_AFTER_MS (90UL * 60UL * 1000UL)     // 90 min full brightness → dim
#define PAUSE_AFTER_MS (150UL * 60UL * 1000UL)  // + 60 min dim → LTLP (pausing sweeps)
//#define DIM_AFTER_MS 10000UL                   // test: 10 s
//#define PAUSE_AFTER_MS 15000UL                 // test: 15 s
#define LONG_PAUSE_MS 3000   // 3 s dark pause in LTLP...
#define LTLP_SWEEPS   3      // ...after this many sweeps
#define IGN_DEBOUNCE_MS 500  // ignition must hold a new state this long before it counts
#define DRIVING_REFRESH_MS 500  // re-send "all off" while driving, in case of data glitches
#define KR_FRAME_US 2500UL   // 400 fps; the rate the sweep was tuned at (FastLED's WS2815 cap)

CRGB leds[LEDS_COUNT];

bool ignitionOn = false;
bool lastIgnition = false;
bool ignRaw = false;
unsigned long ignRawChangedAt = 0;
unsigned long drivingLastRefresh = 0;

unsigned long ignitionOffTime = 0;
uint8_t offStage = 0;  // 0=full, 1=dimmed, 2=Long Term Low Power (LTLP)

// LEDs from the center to one end, counting the middle LED (odd) or middle pair (even)
const int half = LEDS_COUNT / 2 + (LEDS_COUNT % 2);

// ===== KNIGHT RIDER SCANNER SETTINGS =====
float KR_speed = 0.1;   // LEDs per frame
uint8_t KR_tail = 5;
uint8_t KR_fade = 70;
CRGB KR_color = CRGB::Red;
// ======================================

float KR_pos = -5;
int KR_dir = 1;
bool KR_pauseEnabled = false;  // true only in LTLP
bool KR_paused = false;
unsigned long KR_lastPause = 0;
uint8_t KR_ltlpSweepCount = 0;
unsigned long KR_lastFrame = 0;

void setup() {
  // Clear reset flags and stop any watchdog left running by a WDT reset before
  // arming our own. (Optiboot already does this; the old Nano bootloader can't
  // recover from a WDT reset at all — burn Optiboot if the board uses it.)
  MCUSR = 0;
  wdt_disable();

  pinMode(PIN_IGN, INPUT_PULLUP);
  // Seed the debouncer with the real state so boot doesn't wait IGN_DEBOUNCE_MS
  ignRaw = digitalRead(PIN_IGN) == LOW;
  ignitionOn = ignRaw;
  wdt_enable(WDTO_8S);  // 8 s covers the longest blocking step (shutdown animation, ~1.4 s)
  FastLED.addLeds<WS2815, PIN_LED, GRB>(leds, LEDS_COUNT);
  FastLED.setBrightness(LED_BRIGHT);
  FastLED.clear(true);
}

void loop() {
  wdt_reset();

  // Debounce: only accept a new ignition state once it has held for IGN_DEBOUNCE_MS
  bool raw = digitalRead(PIN_IGN) == LOW;
  if (raw != ignRaw) {
    ignRaw = raw;
    ignRawChangedAt = millis();
  }
  if (millis() - ignRawChangedAt >= IGN_DEBOUNCE_MS) ignitionOn = ignRaw;

  if (ignitionOn && !lastIgnition) {
    resetToFullStage();
    FastLED.clear(true);
    drivingLastRefresh = millis();
  }
  if (!ignitionOn && lastIgnition) {
    playShutdownAnimation();
    resetToFullStage();
    ignitionOffTime = millis();
  }

  if (ignitionOn) {
    updateDriving();
  } else {
    unsigned long elapsed = millis() - ignitionOffTime;
    if (offStage == 0 && elapsed >= DIM_AFTER_MS) {
      FastLED.setBrightness(LED_BRIGHT_DIM);
      offStage = 1;
    }
    if (offStage == 1 && elapsed >= PAUSE_AFTER_MS) {
      KR_pauseEnabled = true;
      KR_ltlpSweepCount = 0;
      offStage = 2;
    }
    updateScanner(KR_color);
  }

  lastIgnition = ignitionOn;
}

// Back to stage 0: full brightness, no LTLP pausing.
void resetToFullStage() {
  FastLED.setBrightness(LED_BRIGHT);
  KR_pauseEnabled = false;
  KR_paused = false;
  KR_ltlpSweepCount = 0;
  offStage = 0;
}

// Blocking wait that keeps the watchdog fed.
void waitFeedingWatchdog(uint16_t ms) {
  unsigned long t = millis();
  while (millis() - t < ms) wdt_reset();
}

// Light `color` from the center outward, one LED pair per step.
void fillFromCenter(CRGB color, uint16_t durationMs) {
  // Odd count: both start on the single middle LED. Even count: start on the middle pair.
  int centerL = (LEDS_COUNT - 1) / 2;
  int centerR = LEDS_COUNT / 2;
  uint16_t stepMs = durationMs / half;
  for (int i = 0; i < half; i++) {
    leds[centerL - i] = color;
    leds[centerR + i] = color;
    FastLED.show();
    waitFeedingWatchdog(stepMs);
  }
}

// Hold a solid color for durationMs.
void fillSolidAndHold(CRGB color, uint16_t durationMs) {
  fill_solid(leds, LEDS_COUNT, color);
  FastLED.show();
  waitFeedingWatchdog(durationMs);
}

void playShutdownAnimation() {
  fillFromCenter(CRGB::Red, 1000);
  fillSolidAndHold(CRGB::Red, 400);
  // Strip is left solid red; updateScanner's per-frame fade blends it into the sweep
}

// Strip stays dark while driving. Non-blocking so the ignition input is sampled continuously.
void updateDriving() {
  if (millis() - drivingLastRefresh >= DRIVING_REFRESH_MS) {
    drivingLastRefresh = millis();
    FastLED.clear(true);
  }
}

// Called each time the sweep reaches an end. In LTLP, starts a dark pause every LTLP_SWEEPS.
void onScannerSweepEnd(unsigned long now) {
  if (KR_pauseEnabled && ++KR_ltlpSweepCount >= LTLP_SWEEPS) {
    KR_paused = true;
    KR_lastPause = now;
    KR_ltlpSweepCount = 0;
    FastLED.clear();  // this frame's show() blanks the strip; it stays dark for the pause
  }
}

// Knight Rider-style scanner: one frame per call, rate-limited to KR_FRAME_US.
void updateScanner(CRGB color) {
  // Fixed frame rate so sweep speed and fade don't depend on loop timing
  unsigned long nowUs = micros();
  if (nowUs - KR_lastFrame < KR_FRAME_US) return;
  KR_lastFrame = nowUs;

  unsigned long now = millis();

  if (KR_paused) {
    if (now - KR_lastPause < LONG_PAUSE_MS) return;
    KR_paused = false;
  }

  fadeToBlackBy(leds, LEDS_COUNT, KR_fade);

  KR_pos += KR_speed * KR_dir;
  int base = (int)floor(KR_pos);
  float frac = KR_pos - base;

  // Head: split across two LEDs by fractional position for smooth motion
  if (base >= 0 && base < LEDS_COUNT) {
    CRGB c = color;
    leds[base] += c.nscale8(255 * (1.0f - frac));
  }
  if (base + 1 >= 0 && base + 1 < LEDS_COUNT) {
    CRGB c = color;
    leds[base + 1] += c.nscale8(255 * frac);
  }

  // Glow on both sides of the head, dimmer with distance
  for (int i = 1; i <= KR_tail; i++) {
    uint8_t tailFade = 200 / (i + 1);
    int left = base - i;
    int right = base + 1 + i;
    if (left >= 0 && left < LEDS_COUNT) {
      CRGB c = color;
      leds[left] += c.nscale8(tailFade);
    }
    if (right >= 0 && right < LEDS_COUNT) {
      CRGB c = color;
      leds[right] += c.nscale8(tailFade);
    }
  }

  // Reverse once the glow has fully run off either end
  if (KR_pos > LEDS_COUNT + KR_tail) {
    KR_dir = -1;
    onScannerSweepEnd(now);
  } else if (KR_pos < -(float)KR_tail) {
    KR_dir = 1;
    onScannerSweepEnd(now);
  }

  blur1d(leds, LEDS_COUNT, 40);
  FastLED.show();
}
