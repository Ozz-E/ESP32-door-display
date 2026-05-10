#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "TFT_eSPI.h"
#include "driver.h"

#ifdef EPAPER_ENABLE
EPaper epaper = EPaper();
#endif

// ── WiFi credentials ──────────────────────────────────────
const char* WIFI_SSID     = "YOUR_SSID";
const char* WIFI_PASSWORD = "YOUR_PASSWORD";

// ── Location for Open-Meteo ───────────────────────────────
const float LATITUDE  = 51.5074;  // London
const float LONGITUDE = -0.1278;

// ── NTP ───────────────────────────────────────────────────
// Using POSIX timezone string for correct automatic BST/GMT switching.
// FIX: Previously used hardcoded DST_OFFSET_SEC=3600 which caused a
// permanent -60 min offset. This approach handles transitions correctly.
const char* NTP_SERVER   = "pool.ntp.org";
const char* POSIX_TZ_STR = "GMT0BST,M3.5.0/1,M10.5.0";

// ── Pins ──────────────────────────────────────────────────
const int PIN_KEY0  = 2;  // toggle battery label
const int PIN_KEY1  = 3;  // force weather refresh
const int PIN_KEY2  = 5;  // serial debug dump
const int PIN_ADC   = 1;
const int PIN_ADC_EN = 6;

// ── Timing ────────────────────────────────────────────────
const unsigned long WEATHER_INTERVAL_MS = 15UL * 60 * 1000;
const unsigned long CLOCK_INTERVAL_MS   = 60 * 1000;
const int PARTIAL_LIMIT = 5;

// ── RTC memory — survives deep sleep ──────────────────────
RTC_DATA_ATTR bool  bikeCharging = false;
RTC_DATA_ATTR int   wakeCount    = 0;
RTC_DATA_ATTR bool  firstBoot    = true;

// Weather cached in RTC so the screen can redraw without a fetch on every wake
RTC_DATA_ATTR char  rtcWeatherDesc[32] = "---";
RTC_DATA_ATTR float rtcTempC           = 0;
RTC_DATA_ATTR float rtcFeelsLikeC      = 0;
RTC_DATA_ATTR char  rtcRainWindow[48]  = "No rain today";

// Working copies used during a wake cycle
String weatherDesc;
float  tempC      = 0;
float  feelsLikeC = 0;
String rainWindow;

// ── WMO code → description ────────────────────────────────
String wmoToDesc(int code) {
  if (code == 0)  return "Clear";
  if (code == 1)  return "Mostly clear";
  if (code == 2)  return "Partly cloudy";
  if (code == 3)  return "Overcast";
  if (code <= 49) return "Foggy";
  if (code <= 59) return "Drizzle";
  if (code == 61) return "Light rain";
  if (code == 63) return "Moderate rain";
  if (code == 65) return "Heavy rain";
  if (code <= 69) return "Freezing rain";
  if (code <= 79) return "Snow";
  if (code == 80) return "Light showers";
  if (code == 81) return "Showers";
  if (code == 82) return "Heavy showers";
  if (code <= 84) return "Snow showers";
  return "Thunderstorm";
}

// ── Battery ───────────────────────────────────────────────
float readBatteryVoltage() {
  pinMode(PIN_ADC_EN, OUTPUT);
  digitalWrite(PIN_ADC_EN, HIGH);
  delay(10);
  analogReadResolution(12);
  int raw = analogRead(PIN_ADC);
  digitalWrite(PIN_ADC_EN, LOW);
  return (raw / 4096.0) * 7.16;
}

int batteryPercent(float v) {
  if (v >= 4.2) return 100;
  if (v <= 3.0) return 0;
  return (int)((v - 3.0) / (4.2 - 3.0) * 100.0);
}

// ── WiFi ──────────────────────────────────────────────────
bool connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected: " + WiFi.localIP().toString());
    return true;
  }
  Serial.println("WiFi FAILED — check SSID/password");
  return false;
}

// ── Weather fetch ─────────────────────────────────────────
void fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!connectWiFi()) {
      Serial.println("Weather fetch skipped: no WiFi");
      return;
    }
  }

  String url = String("https://api.open-meteo.com/v1/forecast")
    + "?latitude="  + String(LATITUDE, 4)
    + "&longitude=" + String(LONGITUDE, 4)
    + "&hourly=temperature_2m,apparent_temperature,weathercode,precipitation_probability"
    + "&forecast_days=1&timezone=auto";

  Serial.println("Fetching weather...");
  HTTPClient http;
  http.begin(url);
  int httpCode = http.GET();
  Serial.printf("HTTP code: %d\n", httpCode);

  if (httpCode != 200) {
    Serial.println("Weather fetch failed");
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();
  Serial.printf("Payload length: %d bytes\n", payload.length());

  DynamicJsonDocument doc(16384);
  DeserializationError err = deserializeJson(doc, payload);

  if (err) {
    Serial.printf("JSON error: %s\n", err.c_str());
    return;
  }

  struct tm t;
  getLocalTime(&t);
  int h = t.tm_hour;

  JsonArray temps  = doc["hourly"]["temperature_2m"];
  JsonArray feels  = doc["hourly"]["apparent_temperature"];
  JsonArray codes  = doc["hourly"]["weathercode"];
  JsonArray precip = doc["hourly"]["precipitation_probability"];

  tempC      = temps[h].as<float>();
  feelsLikeC = feels[h].as<float>();
  weatherDesc = wmoToDesc(codes[h].as<int>());

  Serial.printf("Weather: %s %.1fC feels %.1fC\n",
                weatherDesc.c_str(), tempC, feelsLikeC);

  int rainStart = -1, rainEnd = -1;
  for (int i = h; i < 24; i++) {
    int  wcode = codes[i].as<int>();
    int  prob  = precip[i].as<int>();
    bool rainy = (prob >= 10) && (
      (wcode >= 51 && wcode <= 67) ||
      (wcode >= 80 && wcode <= 82) ||
      (wcode >= 95));
    if (rainy) {
      if (rainStart == -1) rainStart = i;
      rainEnd = i;
    } else if (rainStart != -1) {
      break;
    }
  }

  rainWindow = (rainStart == -1)
    ? "No rain today"
    : String("Rain ") + String(rainStart) + ":00-" + String(rainEnd + 1) + ":00";

  Serial.println("Rain: " + rainWindow);
}

// ── Daily inspiration ────────────────────────────────────────
const char* quotes[] = {
  "Slay! Just Slay!",
  "Add your example here"
};
const int NUM_QUOTES = sizeof(quotes) / sizeof(quotes[0]);

String getDailyQuote() {
  struct tm t;
  if (!getLocalTime(&t)) return "";
  return quotes[t.tm_mday % NUM_QUOTES];
}

// ── Time string ───────────────────────────────────────────
String getTimeString() {
  struct tm t;
  if (!getLocalTime(&t)) return "--:--";
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", t.tm_hour, t.tm_min);
  return String(buf);
}

// ── SSD1680 full buffer clear ─────────────────────────────
void clearEpaperFull() {
  uint8_t* buf = (uint8_t*)epaper.frameBuffer(1);
  if (buf) memset(buf, 0xFF, 296 * 128 / 8);
}

// ── Draw display ──────────────────────────────────────────
void drawDisplay(bool fullRefresh) {
#ifdef EPAPER_ENABLE
  float  bv  = readBatteryVoltage();
  int    bpc = batteryPercent(bv);
  String now = getTimeString();

  epaper.setRotation(1);
  clearEpaperFull();

  epaper.setTextColor(TFT_BLACK, TFT_WHITE);

  // ── Top bar ──
  epaper.setTextSize(2);
  epaper.setCursor(4, 4);
  epaper.print(bikeCharging ? "Battery charging" : "Battery downstairs");

  // ── Battery Status ──
  char batBuf[6];
  snprintf(batBuf, sizeof(batBuf), "%d%%", bpc);
  // Right-align: measure width (each char ~12px wide at textSize 2)
  int batWidth = strlen(batBuf) * 12;
  epaper.setCursor(292 - batWidth, 4);
  epaper.print(batBuf);

  epaper.drawLine(0, 26, 296, 26, TFT_BLACK);

  // ── Weather ──
  epaper.setTextSize(2);
  epaper.setCursor(4, 34);
  epaper.print(weatherDesc);

  char tempBuf[20];
  snprintf(tempBuf, sizeof(tempBuf), "%dC (%dC)",
           (int)round(tempC), (int)round(feelsLikeC));
  epaper.setCursor(172, 34);
  epaper.print(tempBuf);

  epaper.setCursor(4, 54);
  epaper.print(rainWindow);

  epaper.drawLine(0, 76, 296, 76, TFT_BLACK);

  // ── Quote + clock ──
  epaper.setTextSize(2);
  epaper.setCursor(4, 80);
  epaper.print(getDailyQuote());

  epaper.setTextSize(3);
  epaper.setCursor(202, 100);
  epaper.print(now);

  epaper.update()
#endif
}

// ── Go to sleep ───────────────────────────────────────────
void goToSleep() {
  Serial.println("Sleeping...");
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);

  // Wake on timer every 60 seconds for clock update
  esp_sleep_enable_timer_wakeup(SLEEP_INTERVAL_US);

  // Wake on any button press (EXT1 watches multiple GPIO pins)
  // Bitmask: GPIO2 = bit 2, GPIO3 = bit 3, GPIO5 = bit 5
  uint64_t buttonMask = (1ULL << PIN_KEY0) | (1ULL << PIN_KEY1) | (1ULL << PIN_KEY2);
  esp_sleep_enable_ext1_wakeup(buttonMask, ESP_EXT1_WAKEUP_ALL_LOW);

  Serial.flush();
  esp_deep_sleep_start();
}

// ── Setup — moved everything away from the loop and enabled sleep/wake cycle to preserve battery
void setup() {
  setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
  tzset();
  Serial.begin(115200);
  delay(200);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  // Load cached weather from RTC into working strings
  weatherDesc = String(rtcWeatherDesc);
  tempC       = rtcTempC;
  feelsLikeC  = rtcFeelsLikeC;
  rainWindow  = String(rtcRainWindow);

  // ── Determine what this wake needs to do ──────────────────
  bool doWeatherFetch = false;
  bool doDebugDump    = false;

  if (firstBoot || cause == ESP_SLEEP_WAKEUP_UNDEFINED) {
    firstBoot      = true;
    doWeatherFetch = true;
    wakeCount      = 0;
    Serial.println("\n=== Door Display booting ===");

  } else if (cause == ESP_SLEEP_WAKEUP_EXT1) {
    uint64_t pinMask = esp_sleep_get_ext1_wakeup_status();
    Serial.printf("Button wake, mask=0x%llx\n", pinMask);

    if (pinMask & (1ULL << PIN_KEY0)) {
      bikeCharging = !bikeCharging;
      Serial.printf("Battery: %s\n", bikeCharging ? "charging" : "downstairs");
    } else if (pinMask & (1ULL << PIN_KEY1)) {
      doWeatherFetch = true;
      wakeCount      = 0;
    } else if (pinMask & (1ULL << PIN_KEY2)) {
      doDebugDump = true;
    }

  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    wakeCount++;
    Serial.printf("Timer wake #%d\n", wakeCount);
    if (wakeCount >= WEATHER_EVERY_N_WAKES) {
      doWeatherFetch = true;
      wakeCount      = 0;
    }
  }

  // ── Initialise display ────────────────────────────────────
  pinMode(PIN_KEY0, INPUT_PULLUP);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);

#ifdef EPAPER_ENABLE
  epaper.begin();
  epaper.setRotation(1);
  clearEpaperFull();
  if (firstBoot) {
    epaper.update();  // blank screen on cold boot before drawing
  }
#endif

  // ── WiFi + NTP + weather if needed ───────────────────────
  if (doWeatherFetch) {
    if (connectWiFi()) {
      if (firstBoot) {
        configTime(0, 0, NTP_SERVER);
        setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
        tzset();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo, 10000)) {
            Serial.printf("Hour: %d, tm_isdst: %d\n", timeinfo.tm_hour, timeinfo.tm_isdst);
        }

        Serial.println("Waiting for NTP sync...");
        // Block up to 10 seconds for a valid NTP time instead of a blind delay
        if (!getLocalTime(&timeinfo, 10000)) {
          Serial.println("WARNING: NTP sync failed, time may be wrong!");
        } else {
          Serial.println("NTP synced. Time: " + getTimeString());
        }
      }
      fetchWeather();
      // Reload working strings after fetch updated RTC copies
      weatherDesc = String(rtcWeatherDesc);
      tempC       = rtcTempC;
      feelsLikeC  = rtcFeelsLikeC;
      rainWindow  = String(rtcRainWindow);
    }
  }

  // ── Debug dump ────────────────────────────────────────────
  if (doDebugDump) {
    float bv = readBatteryVoltage();
    Serial.printf("[DBG] Battery: %.2fV (%d%%)\n", bv, batteryPercent(bv));
    Serial.printf("[DBG] Weather: %s %.1fC (feels %.1fC)\n",
                  weatherDesc.c_str(), tempC, feelsLikeC);
    Serial.println("[DBG] Rain: " + rainWindow);
    Serial.println("[DBG] Time: " + getTimeString());
    Serial.printf("[DBG] bikeCharging: %s\n", bikeCharging ? "true" : "false");
    Serial.printf("[DBG] wakeCount: %d / %d\n", wakeCount, WEATHER_EVERY_N_WAKES);
  }

  // ── Draw and sleep ────────────────────────────────────────
  drawDisplay();
  firstBoot = false;
  goToSleep();
}

// loop() is never reached — the device always sleeps at the end of setup()
void loop() {}
