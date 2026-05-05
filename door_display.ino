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
const char* NTP_SERVER   = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 0;     // UTC
const int   DST_OFFSET_SEC = 3600;  // UK daylight saving

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

// ── State ─────────────────────────────────────────────────
bool ledState     = false;
bool bikeCharging = false;
unsigned long lastWeatherMs = 0;
unsigned long lastClockMs   = 0;
int partialCount = 0;

String weatherDesc = "---";
float tempC      = 0;
float feelsLikeC = 0;
String rainWindow = "No rain today";

unsigned long lastKey0Ms = 0;
unsigned long lastKey1Ms = 0;
unsigned long lastKey2Ms = 0;
const unsigned long DEBOUNCE_MS = 50;

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

// ── Daily 1 liners ────────────────────────────────────────
const char* quotes[] = {
  "Slay! Just Slay!",
  "You are on fire today",
  "So freaking hot",
  "Yes you can",
  "Giiiiirl, looking fine",
  "You are my favourite distraction",
  "Sexy? Me? I'm too shy!",
  "Looking stunning today",
  "Drop it like it's hot",
  "Main character energy only.",
  "Stop it, you're making the sun jealous.",
  "Warning: Contents are extremely hot.",
  "Go get 'em, Tiger.",
  "Looking like a whole damn snack.",
  "Queen behavior, honestly.",
  "The world isn't ready for you today.",
  "Who gave you permission to be this cute?",
  "Keep that same energy, you're killing it.",
  "CEO of looking gorgeous.",
  "Pure magic, that's what you are.",
  "Is it hot in here or is it your outfit?",
  "Manifesting a perfect day for you",
  "Serving looks, as per usual.",
  "You're the plot twist I always wanted.",
  "Absolutely iconic.",
  "Keep shining, the world needs your glow.",
  "Me? Obsessed with you?",
  "Go off, Queen!",
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

  if (bpc < 20) {
    epaper.setCursor(4, 24);
    epaper.print("[LOW BATTERY]");
  }

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

  // Partial refresh not yet working — fix in future
  if (fullRefresh) {
    epaper.update();
  } else {
    epaper.updataPartial(0, 0, 296, 28);
  }
#endif
}

// ── Fix fuzzy gate line at top (not yet working) ──────────
void fixGateLine() {
  epaper.writecommand(0x01);
  epaper.writedata(0x23);
  epaper.writedata(0x01);
  epaper.writedata(0x00);
}

// ── Setup ─────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Door Display booting ===");

  pinMode(PIN_KEY0, INPUT_PULLUP);
  pinMode(PIN_KEY1, INPUT_PULLUP);
  pinMode(PIN_KEY2, INPUT_PULLUP);

#ifdef EPAPER_ENABLE
  epaper.begin();
  fixGateLine();
  epaper.setRotation(1);
  clearEpaperFull();
  epaper.update();
  Serial.printf("Display init: W=%d H=%d\n", epaper.width(), epaper.height());
  Serial.println("Display OK");
#endif

  if (connectWiFi()) {
    configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);
    Serial.println("Waiting for NTP...");
    delay(2000);
    Serial.println("Time: " + getTimeString());
    fetchWeather();
  }

  drawDisplay(true);

  lastWeatherMs = millis();
  lastClockMs   = millis();
  Serial.println("Boot complete");
}

// ── Loop ──────────────────────────────────────────────────
void loop() {
  unsigned long now = millis();

  // KEY0: toggle e-bike battery label
  if (digitalRead(PIN_KEY0) == LOW && (now - lastKey0Ms) > DEBOUNCE_MS) {
    lastKey0Ms    = now;
    bikeCharging  = !bikeCharging;
    partialCount++;
    bool doFull = (partialCount >= PARTIAL_LIMIT);
    if (doFull) partialCount = 0;
    drawDisplay(true);
    while (digitalRead(PIN_KEY0) == LOW);
  }

  // KEY1: force weather refresh
  if (digitalRead(PIN_KEY1) == LOW && (now - lastKey1Ms) > DEBOUNCE_MS) {
    lastKey1Ms = now;
    fetchWeather();
    lastWeatherMs = now;
    partialCount  = 0;
    drawDisplay(true);
    while (digitalRead(PIN_KEY1) == LOW);
  }

  // KEY2: serial debug dump
  if (digitalRead(PIN_KEY2) == LOW && (now - lastKey2Ms) > DEBOUNCE_MS) {
    lastKey2Ms = now;
    float bv   = readBatteryVoltage();
    Serial.printf("[DBG] Battery: %.2fV (%d%%)\n", bv, batteryPercent(bv));
    Serial.printf("[DBG] Weather: %s %.1fC (feels %.1fC)\n",
                  weatherDesc.c_str(), tempC, feelsLikeC);
    Serial.println("[DBG] Rain: " + rainWindow);
    Serial.println("[DBG] Time: " + getTimeString());
    while (digitalRead(PIN_KEY2) == LOW);
  }

  // Clock tick every minute
  if (now - lastClockMs >= CLOCK_INTERVAL_MS) {
    lastClockMs = now;
    partialCount++;
    bool doFull = (partialCount >= PARTIAL_LIMIT);
    if (doFull) partialCount = 0;
    drawDisplay(doFull);
  }

  // Weather refresh every 15 minutes
  if (now - lastWeatherMs >= WEATHER_INTERVAL_MS) {
    lastWeatherMs = now;
    fetchWeather();
    partialCount = 0;
    drawDisplay(true);
  }
}
