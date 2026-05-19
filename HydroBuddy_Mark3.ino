/*
 * HydroBuddy_Mark3
 * HomeKit-connected automatic plant watering controller
 *
 * Hardware: Waveshare ESP32-S3-Nano
 *           SSD1306 128×64 OLED (I2C on A4/A5)
 *           Reed switch reservoir sensor (A0)
 *           PBS-33B-BK momentary button (A1)
 *           IRLZ44N MOSFET pump driver (D2)
 *
 * Libraries required:
 *   - HomeSpan          (HomeKit framework)
 *   - Adafruit SSD1306
 *   - Adafruit GFX Library
 *   - ArduinoJson
 *   - HTTPClient        (built into ESP32 Arduino core)
 *
 * HomeKit pairing code: 472-53-618
 *   Enter this in the Apple Home app when adding the accessory.
 *
 * OTA: upload to hydrobuddy-mark3.local  password: hydrobuddy123
 */

#define HOMEKIT_MINIMAL
#include <HomeSpan.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "secrets.h"   // defines WIFI_SSID, WIFI_PASSWORD, LOCATION_LAT/LON (gitignored)

// ── Device identity ───────────────────────────────────────────────────────────
#define OTA_HOSTNAME   "hydrobuddy-mark3"
#define OTA_PASSWORD   "hydrobuddy123"   // ArduinoOTA upload password
#define SKETCH_VERSION "1.0"
#define PAIRING_CODE   "47253618"        // HomeKit PIN: 472-53-618

// ── Easy-adjust pump timing ───────────────────────────────────────────────────
#define TEMP_THRESHOLD_F     75     // °F – at or above this, use the longer duration
#define PUMP_DURATION_NORMAL 60     // seconds per cycle on a cool day
#define PUMP_DURATION_HOT    90     // seconds per cycle on a hot day
const unsigned long REED_DEBOUNCE_MS = 5000;  // ms reed must stay HIGH to confirm empty

// ── Pin definitions (Waveshare ESP32-S3-Nano) ────────────────────────────────
// Nano A-pin → GPIO:  A0=1  A1=2  A4=11 (SDA)  A5=12 (SCL)
// Nano D-pin → GPIO:  D2=5
#define PIN_REED    A0   // Reed switch: LOW = reservoir OK, HIGH = empty
#define PIN_BUTTON  A1   // PBS-33B-BK: active LOW, internal pull-up
#define PIN_PUMP    D2   // IRLZ44N MOSFET gate

#define OLED_SDA    A4
#define OLED_SCL    A5
#define OLED_ADDR   0x3C
#define SCREEN_W    128
#define SCREEN_H    64

// ── Globals ───────────────────────────────────────────────────────────────────
Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// Pump state
bool          pumpRunning  = false;
bool          pumpIsManual = false;   // true = button hold, false = HomeKit cycle
unsigned long pumpStartMs  = 0;

// Reservoir state
// reservoirEmpty is only set after REED_DEBOUNCE_MS sustained HIGH on PIN_REED.
// pumpBlockedUntilManual is set (Option A) when the reed fires mid-cycle;
// it stays set even after refill — only a manual button hold clears it.
bool          reservoirEmpty          = false;
bool          reedCurrentlyHigh       = false;
unsigned long reedHighStartMs         = 0;
bool          pumpBlockedUntilManual  = false;

// Button state
bool btnLastLow = false;

// Pointer to HomeKit On characteristic; set in WaterSwitch(), used in loop()
// to push state corrections back to HomeKit after auto-stop or reservoir block.
SpanCharacteristic* hkOn      = nullptr;
bool                hkWantsOn = false;   // tracks latest HomeKit On request

// Display refresh (200 ms cadence keeps HomeSpan responsive)
unsigned long lastDisplayMs = 0;

time_t        lastPumpTime  = 0;   // Unix timestamp of last pump run; 0 = never
float         todayHighF    = 0;   // today's forecast high °F; 0 = not yet fetched
unsigned long lastWeatherMs = 0;   // millis() of last fetchWeather() call
int           pumpDurSec    = PUMP_DURATION_NORMAL;  // set each time pump starts

// ── Forward declarations ──────────────────────────────────────────────────────
void fetchWeather();
void startPump(bool manual);
void stopPump();
void handleButton(unsigned long now);
void handleReedSwitch(unsigned long now);
void updateDisplay();
void drawNormalScreen();
void drawReservoirEmpty();
void drawWiFiBars(int x, int y);
void drawOTAProgress(unsigned int progress, unsigned int total);

// ── HomeKit Switch service ────────────────────────────────────────────────────
// Switch On  → run pump for PUMP_DURATION_NORMAL or PUMP_DURATION_HOT seconds, then report Off.
// Switch Off → stop pump immediately.
struct WaterSwitch : Service::Switch {

  SpanCharacteristic* power;

  WaterSwitch() {
    power = new Characteristic::On(false);
    hkOn  = power;   // expose to global scope so loop() can push corrections
  }

  boolean update() override {
    bool on = power->getNewVal();
    hkWantsOn = on;

    if (on) {
      startPump(false);   // HomeKit-triggered cycle
      // If startPump() returned without running (reservoir empty / blocked),
      // hkWantsOn stays true; loop() will detect pump didn't start and push Off.
    } else {
      stopPump();
    }
    return true;
  }
};

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  // Hardware init
  pinMode(PIN_REED,   INPUT_PULLUP);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_PUMP,   OUTPUT);
  digitalWrite(PIN_PUMP, LOW);   // pump off on boot

  Wire.begin(OLED_SDA, OLED_SCL);
  Wire.setClock(100000);   // 100 kHz – more compatible with generic SSD1306 boards

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println(F("OLED init failed – check wiring"));
    while (true) delay(100);
  }
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(0xFF);   // max contrast
  display.setTextColor(SSD1306_WHITE);

  // Show splash while HomeSpan connects to WiFi
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(8, 18);
  display.print("Connecting to WiFi");
  display.setCursor(34, 34);
  display.print("Please wait...");
  display.display();

  // ── HomeSpan configuration ──
  homeSpan.setHostNameSuffix("");                        // use OTA_HOSTNAME exactly, no suffix
  homeSpan.setSketchVersion(SKETCH_VERSION);
  homeSpan.setWifiCredentials(WIFI_SSID, WIFI_PASSWORD); // from secrets.h
  homeSpan.enableOTA(OTA_PASSWORD);
  homeSpan.setPairingCode(PAIRING_CODE);
  homeSpan.setLogLevel(0);   // 0 = minimal serial output

  homeSpan.begin(Category::Switches, "HydroBuddy Mark 3", OTA_HOSTNAME);

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Manufacturer("GarageDesignCo");
      new Characteristic::Model("ESP32-S3-Nano");
      new Characteristic::SerialNumber("HB3-001");
      new Characteristic::FirmwareRevision(SKETCH_VERSION);

  new WaterSwitch();

  // Hook into HomeSpan's ArduinoOTA for display progress bar
  ArduinoOTA.onStart([]() {
    stopPump();   // safe state before flash
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    drawOTAProgress(progress, total);
  });

  // Sync wall-clock time via NTP; required for lastPumpTime display.
  // POSIX string handles PST (UTC-8) / PDT (UTC-7) DST transitions automatically.
  configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  homeSpan.poll();   // drives WiFi reconnect, OTA, and HomeKit – must run first

  unsigned long now = millis();

  // 1. Reed switch – confirmed empty after REED_DEBOUNCE_MS sustained HIGH
  handleReedSwitch(now);

  // 2. Manual button – pump runs while button is held
  handleButton(now);

  // 3. HomeKit cycle auto-stop after pumpDurSec seconds
  if (pumpRunning && !pumpIsManual &&
      (now - pumpStartMs >= (unsigned long)pumpDurSec * 1000UL)) {
    stopPump();
    hkWantsOn = false;
    if (hkOn) hkOn->setVal(false);   // push Off state back to HomeKit
  }

  // 4. Correct HomeKit if it thinks On but pump never started (reservoir blocked)
  if (hkWantsOn && !pumpRunning) {
    hkWantsOn = false;
    if (hkOn) hkOn->setVal(false);
  }

  // 5. Refresh weather forecast every 6 hours; also fires once after WiFi first connects
  if (WiFi.status() == WL_CONNECTED &&
      (lastWeatherMs == 0 || now - lastWeatherMs >= 6UL * 3600UL * 1000UL)) {
    fetchWeather();
  }

  // 6. Refresh display at ~5 Hz
  if (now - lastDisplayMs >= 200) {
    lastDisplayMs = now;
    updateDisplay();
  }
}

// ── Pump control ──────────────────────────────────────────────────────────────
void startPump(bool manual) {
  if (reservoirEmpty) return;   // hard block: no pump without water

  // Option A: after a mid-cycle reed interruption, only a manual hold can resume.
  // This persists across reservoir refills so the user must be physically present.
  if (pumpBlockedUntilManual && !manual) return;
  pumpBlockedUntilManual = false;   // manual hold acknowledged; clear the block

  pumpDurSec   = (todayHighF >= TEMP_THRESHOLD_F) ? PUMP_DURATION_HOT : PUMP_DURATION_NORMAL;
  pumpStartMs  = millis();
  lastPumpTime = time(nullptr);
  pumpRunning  = true;
  pumpIsManual = manual;
  digitalWrite(PIN_PUMP, HIGH);
}

void stopPump() {
  digitalWrite(PIN_PUMP, LOW);
  pumpRunning  = false;
  pumpIsManual = false;
}

// ── Weather fetch (Open-Meteo, no API key required) ───────────────────────────
void fetchWeather() {
  char url[220];
  snprintf(url, sizeof(url),
    "http://api.open-meteo.com/v1/forecast"
    "?latitude=%.4f&longitude=%.4f"
    "&daily=temperature_2m_max&temperature_unit=fahrenheit"
    "&timezone=America%%2FLos_Angeles&forecast_days=1",
    (float)LOCATION_LAT, (float)LOCATION_LON);

  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, http.getString());
    if (!err) {
      todayHighF    = doc["daily"]["temperature_2m_max"][0].as<float>();
      lastWeatherMs = millis();
    }
  }
  http.end();
}

// ── Reed switch – 5-second sustained trigger ──────────────────────────────────
// Prevents false empties from brief float bounce or electrical noise.
// Once confirmed empty: stops pump, sets Option A block, notifies HomeKit.
// Block clears only when the user manually holds the button after refilling.
void handleReedSwitch(unsigned long now) {
  bool isHigh = (digitalRead(PIN_REED) == HIGH);   // HIGH = empty

  if (!isHigh) {
    // Reservoir is OK – reset timer and clear empty flag.
    // pumpBlockedUntilManual intentionally NOT cleared here (Option A).
    reedCurrentlyHigh = false;
    reedHighStartMs   = 0;
    reservoirEmpty    = false;
    return;
  }

  // Reed reads HIGH (empty)
  if (!reedCurrentlyHigh) {
    reedCurrentlyHigh = true;
    reedHighStartMs   = now;
    return;
  }

  // Already tracking – check if threshold reached
  if (!reservoirEmpty && (now - reedHighStartMs >= REED_DEBOUNCE_MS)) {
    reservoirEmpty = true;
    if (pumpRunning) {
      // Mid-cycle interruption: stop pump and require manual hold to resume
      stopPump();
      pumpBlockedUntilManual = true;
      hkWantsOn = false;
      if (hkOn) hkOn->setVal(false);   // notify HomeKit the switch is now Off
    }
  }
}

// ── Button – manual pump hold ─────────────────────────────────────────────────
// Press and hold = pump runs; release = pump stops.
// Also clears the Option A block (pumpBlockedUntilManual) on press,
// provided the reservoir is refilled (reservoirEmpty == false).
void handleButton(unsigned long now) {
  bool isLow = (digitalRead(PIN_BUTTON) == LOW);

  if (isLow && !btnLastLow) {         // falling edge = press
    btnLastLow = true;
    if (!pumpRunning) {
      startPump(true);   // manual = true clears pumpBlockedUntilManual if reservoir OK
    }
  } else if (!isLow && btnLastLow) {  // rising edge = release
    btnLastLow = false;
    if (pumpIsManual) stopPump();
  }
}

// ── Display routing ───────────────────────────────────────────────────────────
void updateDisplay() {
  // Reservoir empty warning takes over the screen unless pump is also running
  // (the pump check handles the brief window during mid-cycle reed trigger).
  if (reservoirEmpty && !pumpRunning) {
    drawReservoirEmpty();
  } else {
    drawNormalScreen();
  }
}

// ── Screen: Normal operation ──────────────────────────────────────────────────
// Row 0 (y= 0): HomeKit switch state             + WiFi signal bars (right)
// Row 1 (y=16): Pump status + elapsed seconds
// Row 2 (y=32): Today's forecast high temp + pump duration
// Row 3 (y=48): Last watered timestamp
void drawNormalScreen() {
  display.clearDisplay();
  display.setTextSize(1);

  // Row 0: HomeKit switch state
  bool hkState = hkOn ? (bool)hkOn->getVal() : false;
  display.setCursor(0, 0);
  display.print(hkState ? "HK: On " : "HK: Off");
  drawWiFiBars(110, 0);   // 4-bar indicator, right-aligned

  // Row 1: Pump status
  display.setCursor(0, 16);
  if (pumpRunning) {
    int elapsed = (int)((millis() - pumpStartMs) / 1000);
    char buf[22];
    snprintf(buf, sizeof(buf), "Pump: Running %3ds", elapsed);
    display.print(buf);
  } else {
    display.print("Pump: Idle");
  }

  // Row 2: Today's forecast high + pump duration
  display.setCursor(0, 32);
  char dayBuf[24];   // worst case: "Day High: 100F (90s)" = 20 + null
  if (todayHighF > 0) {
    snprintf(dayBuf, sizeof(dayBuf), "Day High: %dF (%ds)", (int)roundf(todayHighF), pumpDurSec);
  } else {
    snprintf(dayBuf, sizeof(dayBuf), "Day High: -- (%ds)", pumpDurSec);
  }
  display.print(dayBuf);

  // Row 3: Last watered timestamp
  // Format: "Last: 5/18 2:09p"
  display.setCursor(0, 48);
  if (lastPumpTime == 0) {
    display.print("Last: --");
  } else {
    struct tm* t = localtime(&lastPumpTime);
    char buf[20];   // worst case: "Last: 12/31 12:59p" = 18 + null
    int hour = t->tm_hour;
    const char* ampm = (hour >= 12) ? "p" : "a";
    if (hour == 0) hour = 12;
    else if (hour > 12) hour -= 12;
    snprintf(buf, sizeof(buf), "Last: %d/%d %d:%02d%s",
      t->tm_mon + 1, t->tm_mday, hour, t->tm_min, ampm);
    display.print(buf);
  }

  display.display();
}

// ── WiFi signal bars ──────────────────────────────────────────────────────────
// Draws four ascending bars at (x, y), bottom-aligned to a 8px row.
// Filled bars = signal strength; hollow bars = unused range.
void drawWiFiBars(int x, int y) {
  if (WiFi.status() != WL_CONNECTED) {
    display.setCursor(x + 2, y);
    display.print("--");
    return;
  }

  int rssi = WiFi.RSSI();
  int bars;
  if      (rssi >= -67) bars = 4;   // excellent
  else if (rssi >= -70) bars = 3;   // good
  else if (rssi >= -80) bars = 2;   // fair
  else                  bars = 1;   // poor

  // Bar dimensions: 3px wide, heights 2/4/6/8, 1px gap, bottom-aligned at y+7
  for (int i = 0; i < 4; i++) {
    int barH = 2 + i * 2;
    int barX = x + i * 4;
    int barY = y + 7 - barH;
    if (i < bars) {
      display.fillRect(barX, barY, 3, barH, SSD1306_WHITE);
    } else {
      display.drawRect(barX, barY, 3, barH, SSD1306_WHITE);
    }
  }
}

// ── Screen: Reservoir empty warning ──────────────────────────────────────────
void drawReservoirEmpty() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 3);
  display.print("! EMPTY !");
  display.setTextSize(1);
  display.setCursor(4, 32);
  display.print("Reservoir is empty.");
  display.setCursor(10, 46);
  display.print("Refill to resume.");
  display.display();
}

// ── Screen: OTA progress bar ──────────────────────────────────────────────────
void drawOTAProgress(unsigned int progress, unsigned int total) {
  int pct  = (total > 0) ? (int)((progress * 100UL) / total) : 0;
  int barW = (SCREEN_W - 4) * pct / 100;

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(28, 6);
  display.print("OTA Update...");
  display.drawRect(2, 24, SCREEN_W - 4, 12, SSD1306_WHITE);
  if (barW > 0) display.fillRect(2, 24, barW, 12, SSD1306_WHITE);
  display.setTextSize(2);
  char buf[5];
  snprintf(buf, sizeof(buf), "%3d%%", pct);
  display.setCursor(28, 42);
  display.print(buf);
  display.display();
}
