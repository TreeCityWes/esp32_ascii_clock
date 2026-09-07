// Dedicated touch-free digital clock for ESP32-2432S028R / S029R.
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <TFT_eSPI.h>
#include <esp_sntp.h>
#include <time.h>
#include <atomic>
#include "clock_font.h"

#ifndef TZ_POSIX
#define TZ_POSIX "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef AP_NAME
#define AP_NAME "ascii-clock"
#endif

// ---------- hardware ----------
static const int XPT_CS = 33;
static const int LDR_PIN = 34;
static const int BL_PIN = 21, BL_CH = 0;

TFT_eSPI tft;
TFT_eSprite spr(&tft);
WiFiManager wm;

// Antialiased glyph masks are stored in flash, requiring no filesystem.
static const int SCREEN_W = 320, SCREEN_H = 240, SPR_H = 120;
static const int DIGIT_Y = 18, DIGIT_GAP = 6;
static std::atomic<time_t> lastSync{0};
static uint16_t timeInk[16], secondaryInk[16];

static const ClockGlyph& glyph(char ch, bool large) {
  int index = large ? (ch == ':' ? 10 : ch == '-' ? 11 : ch - '0') :
      12 + constrain((int)ch, 32, 126) - 32;
  return CLOCK_GLYPHS[index];
}

static int textWidth(const char* text, bool large) {
  int width = 0;
  for (int i = 0; text[i]; ++i) {
    if (large && i) width += DIGIT_GAP;
    width += glyph(text[i], large).advance;
  }
  return width;
}

static void drawText(const char* text, int x, int y, int off, bool large) {
  const uint16_t* ink = large ? timeInk : secondaryInk;
  for (int i = 0; text[i]; ++i) {
    const ClockGlyph& g = glyph(text[i], large);
    for (int row = max(0, off - y); row < min((int)g.h, off + SPR_H - y); ++row) {
      // Horizontal runs avoid a draw call per pixel and clip at the sprite seam.
      int col = 0;
      while (col < g.w) {
        const int start = col;
        const uint8_t alpha = pgm_read_byte(CLOCK_ALPHA + g.offset + row * g.w + col++);
        while (col < g.w && pgm_read_byte(CLOCK_ALPHA + g.offset + row * g.w + col) == alpha) ++col;
        if (alpha) spr.drawFastHLine(x + start, y + row - off, col - start, ink[alpha]);
      }
    }
    x += g.advance + (large ? DIGIT_GAP : 0);
  }
}

// ---------- state ----------
static bool wifiOk = false, timeOk = false, spriteOk = false;
static unsigned long lastFrame = 0, lastLdr = 0;
static float blLevel = 255;

static const char* DAYS[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char* MONTHS[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// ---------- render ----------
static void renderFrame() {
  tm tmv{};
  const time_t epoch = time(nullptr);
  timeOk = epoch >= 1704067200 && localtime_r(&epoch, &tmv) != nullptr;
  char digits[6] = "--:--";
  if (timeOk) {
    const int h12 = tmv.tm_hour % 12 ? tmv.tm_hour % 12 : 12;
    snprintf(digits, sizeof(digits), "%d:%02d", h12, tmv.tm_min);
  }
  const bool portal = wm.getConfigPortalActive();
  const time_t syncedAt = lastSync.load();
  const String viewKey = String(digits) + ":" + String(epoch / 86400) + ":" +
      String(wifiOk) + String(portal) + String(syncedAt && epoch - syncedAt > 3600);
  static String previousView;
  if (viewKey == previousView) return;
  previousView = viewKey;
  for (int off = 0; off < SCREEN_H; off += SPR_H) {
    spr.fillSprite(TFT_BLACK);
    // Initial setup gets readable instructions rather than tiny diagnostics.
    if (!timeOk && portal) {
      spr.setTextFont(2);
      spr.setTextSize(2);
      spr.setTextColor(TFT_WHITE, TFT_BLACK);
      spr.setTextDatum(TC_DATUM);
      spr.drawString("WI-FI SETUP", 160, 24 - off);
      spr.setTextSize(1);
      spr.drawString("Join this network on your phone", 160, 80 - off);
      spr.setTextSize(2);
      spr.drawString(AP_NAME, 160, 108 - off);
      spr.drawString("192.168.4.1", 160, 160 - off);
    } else {
      drawText(digits, (SCREEN_W - textWidth(digits, true)) / 2, DIGIT_Y, off, true);
      char date[20] = "Syncing";
      if (timeOk) snprintf(date, sizeof(date), "%s, %s %d",
                          DAYS[tmv.tm_wday], MONTHS[tmv.tm_mon], tmv.tm_mday);
      const time_t synced = lastSync.load();
      const char* footer = !wifiOk ? "Offline" :
          timeOk && synced && epoch - synced > 3600 ? "Sync due" : date;
      drawText(footer, 12, 200, off, false);
      if (timeOk) {
        const char* period = tmv.tm_hour >= 12 ? "PM" : "AM";
        drawText(period, SCREEN_W - 12 - textWidth(period, false), 200, off, false);
      }
    }
    spr.pushSprite(0, off);
  }
}

// ---------- boot screen ----------
static void bootMsg(const char* line1, const char* line2 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(0x9a, 0xf2, 0xff), TFT_BLACK);
  tft.drawString("digital clock", SCREEN_W / 2, SCREEN_H / 2 - 20);
  tft.setTextColor(tft.color565(0x9a, 0xa1, 0xac), TFT_BLACK);
  tft.drawString(line1, SCREEN_W / 2, SCREEN_H / 2 + 6);
  tft.drawString(line2, SCREEN_W / 2, SCREEN_H / 2 + 24);
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(1);
#ifdef INVERT_DISPLAY
  tft.invertDisplay(INVERT_DISPLAY);
#endif
  tft.fillScreen(TFT_BLACK);
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
  ledcWrite(BL_CH, 255);
  analogReadResolution(12);

  pinMode(XPT_CS, OUTPUT);
  digitalWrite(XPT_CS, HIGH);

  bootMsg("connecting wifi...");
  WiFi.setAutoReconnect(true);
  wm.setConfigPortalBlocking(false);
  wm.setConnectTimeout(10);
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) {
    bootMsg("join wifi \"" AP_NAME "\"", "then open 192.168.4.1");
  });
  wifiOk = wm.autoConnect(AP_NAME);

  sntp_set_time_sync_notification_cb([](struct timeval* tv) {
    lastSync.store(tv->tv_sec);
  });
  sntp_set_sync_interval(15UL * 60UL * 1000UL);
  configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov", "time.google.com");

  spr.setColorDepth(16);
  spriteOk = spr.createSprite(SCREEN_W, SPR_H) != nullptr;
  if (!spriteOk) {
    spr.setColorDepth(8);
    spriteOk = spr.createSprite(SCREEN_W, SPR_H) != nullptr;
  }
  if (!spriteOk) bootMsg("display memory error");
  spr.setTextWrap(false);
  for (int a = 0; a < 16; ++a) {
    timeInk[a] = tft.color565(245*a/15, 241*a/15, 232*a/15);
    secondaryInk[a] = tft.color565(214*a/15, 215*a/15, 211*a/15);
  }
}

void loop() {
  unsigned long now = millis();

  wm.process();
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiOk)
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov", "time.google.com");
  wifiOk = connected;
  static unsigned long lastReconnect = 0;
  if (!wifiOk && !wm.getConfigPortalActive() && now - lastReconnect >= 30000) {
    lastReconnect = now;
    WiFi.reconnect();
  }

  // auto-dim from the light sensor (dark room → softer backlight)
  if (now - lastLdr > 500) {
    lastLdr = now;
    int ldr = analogRead(LDR_PIN);                     // higher = darker on the CYD
    float target = map(constrain(ldr, 300, 3800), 3800, 300, 70, 255);
    blLevel += (target - blLevel) * 0.2f;
    ledcWrite(BL_CH, (int)blLevel);
  }

  if (spriteOk && now - lastFrame >= 100) {
    lastFrame = now;
    renderFrame();
  }
  delay(2);
}
