// ASCII clock for the ESP32-2432S028R / S029R ("Cheap Yellow Display").
// Port of index.html: 53×20 character grid, solid block digits with a
// sweeping color gradient, animated wave/star backgrounds per face,
// date + weather along the bottom. Tap the screen to cycle faces / auto.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

#ifndef TZ_POSIX
#define TZ_POSIX "EST5EDT,M3.2.0,M11.1.0"
#endif
#ifndef AP_NAME
#define AP_NAME "ascii-clock"
#endif

// ---------- hardware ----------
static const int XPT_IRQ = 36, XPT_MOSI = 32, XPT_MISO = 39, XPT_CLK = 25, XPT_CS = 33;
static const int LDR_PIN = 34;
static const int BL_PIN = 21, BL_CH = 0;

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(XPT_CS, XPT_IRQ);

// ---------- grid ----------
static const int COLS = 53, ROWS = 20, CW = 6, CH = 12;
static const int SCREEN_W = 320, SCREEN_H = 240, SPR_H = 120;

// ---------- glyphs (5 wide, refined proportions, 9 tall) ----------
// Carefully crafted contours with chamfered corners, open counters, and optical balance.
static const char* G_DIGITS[10][9] = {
  {" ### ",
   "#   #",
   "#   #",
   "#   #",
   "#   #",
   "#   #",
   "#   #",
   "#   #",
   " ### "},

  {"  #  ",
   " ##  ",
   "  #  ",
   "  #  ",
   "  #  ",
   "  #  ",
   "  #  ",
   "  #  ",
   "#####"},

  {" ### ",
   "#   #",
   "    #",
   "    #",
   " ### ",
   "#    ",
   "#    ",
   "#    ",
   "#####"},

  {"#### ",
   "    #",
   "    #",
   "    #",
   " ### ",
   "    #",
   "    #",
   "    #",
   "#### "},

  {"#   #",
   "#   #",
   "#   #",
   "#   #",
   "#####",
   "    #",
   "    #",
   "    #",
   "    #"},

  {"#####",
   "#    ",
   "#    ",
   "#### ",
   "    #",
   "    #",
   "    #",
   "#   #",
   " ### "},

  {" ### ",
   "#   #",
   "#    ",
   "#### ",
   "#   #",
   "#   #",
   "#   #",
   "#   #",
   " ### "},

  {"#####",
   "    #",
   "    #",
   "   # ",
   "   # ",
   "  #  ",
   "  #  ",
   "  #  ",
   "  #  "},

  {" ### ",
   "#   #",
   "#   #",
   "#   #",
   " ### ",
   "#   #",
   "#   #",
   "#   #",
   " ### "},

  {" ### ",
   "#   #",
   "#   #",
   "#   #",
   " ####",
   "    #",
   "    #",
   "#   #",
   " ### "},
};
static const char* G_COLON[9] = {
  "  ",
  "  ",
  " #",
  " #",
  "  ",
  " #",
  " #",
  "  ",
  "  "
};
static const char* G_DASH[9]  = {
  "     ",
  "     ",
  "     ",
  "     ",
  "#####",
  "     ",
  "     ",
  "     ",
  "     "
};

static const char* const* glyphFor(char c, int& w) {
  if (c >= '0' && c <= '9') { w = 5; return G_DIGITS[c - '0']; }
  if (c == ':') { w = 2; return G_COLON; }
  w = 5; return G_DASH;
}

// ---------- color ----------
struct RGB { uint8_t r, g, b; };
static RGB rgb(uint32_t h) { return { (uint8_t)(h >> 16), (uint8_t)(h >> 8), (uint8_t)h }; }
static RGB mix(RGB a, RGB b, float u) {
  u = constrain(u, 0.0f, 1.0f);
  return { (uint8_t)(a.r + (b.r - a.r) * u), (uint8_t)(a.g + (b.g - a.g) * u), (uint8_t)(a.b + (b.b - a.b) * u) };
}
static RGB dim(RGB c, float a) {
  a = constrain(a, 0.0f, 1.0f);
  return { (uint8_t)(c.r * a), (uint8_t)(c.g * a), (uint8_t)(c.b * a) };
}
static uint16_t c565(RGB c) { return tft.color565(c.r, c.g, c.b); }
static RGB palette(const uint32_t* stops, int n, float u) {
  u -= floorf(u);
  float seg = u * (n - 1);
  int i = (int)seg;
  int j = min(i + 1, n - 1);
  return mix(rgb(stops[i]), rgb(stops[j]), seg - i);
}
static float hash2(int x, int y) {
  uint32_t h = ((uint32_t)x * 374761393u + (uint32_t)y * 668265263u) ^ 0x5bd1e995u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return (float)(h ^ (h >> 16)) / 4294967296.0f;
}

// ---------- faces ----------
enum Face { SUNRISE, WATER, NIGHT, SPACE, FACE_COUNT };
static const char* FACE_NAMES[FACE_COUNT] = { "SUNRISE", "WATER", "NIGHT", "SPACE" };
static const uint32_t PAL_SUNRISE[] = { 0xffcaa5, 0xff9973, 0xf06282, 0xd070e8, 0xffcaa5 };
static const uint32_t PAL_WATER[]   = { 0x8ee7f8, 0x42b4e6, 0x58d8c2, 0x62c0f0, 0x8ee7f8 };
static const uint32_t PAL_NIGHT[]   = { 0xeaf0fc, 0xb4c4e8, 0xd2dcf4, 0x98aed6, 0xeaf0fc };
static const uint32_t PAL_SPACE[]   = { 0xf472d0, 0x9b6ef3, 0x48caf5, 0x63e6bf, 0xf472d0 };
struct FaceDef { const uint32_t* pal; int n; bool textured; };
static const FaceDef FACES[FACE_COUNT] = {
  { PAL_SUNRISE, 5, false }, { PAL_WATER, 5, false }, { PAL_NIGHT, 5, false }, { PAL_SPACE, 5, true },
};

// Background cell for a face. Returns false for empty. `col` is pre-dimmed.
static bool bgCell(Face f, int x, int y, float t, char& ch, RGB& col) {
  switch (f) {
    case SUNRISE: {
      const int horizon = 13;
      if (y >= horizon) {
        float w = sinf(x * 0.24f + t * 1.3f + (y - horizon) * 0.85f);
        ch = w > 0.58f ? '~' : (w > -0.15f ? '-' : '.');
        float d = (float)(y - horizon) / (ROWS - horizon);
        col = dim(mix(rgb(0xe06830), rgb(0x602220), d), 0.55f);
        return true;
      }
      float s = sinf(x * 0.11f - t * 0.5f + y * 1.25f);
      if (s <= -0.3f) return false;
      ch = s > 0.72f ? '=' : (s > 0.18f ? '-' : '.');
      float d = (float)y / horizon;
      col = dim(mix(rgb(0x8c4eb5), rgb(0xf0687c), d), 0.38f + 0.28f * d);
      return true;
    }
    case WATER: {
      float w = sinf(x * 0.20f + t * 1.5f + y * 0.65f) + 0.45f * sinf(x * 0.45f - t * 0.9f + y * 0.35f);
      if (w <= -0.2f) return false;
      ch = w > 0.55f ? '~' : (w > 0.1f ? '-' : '.');
      float d = (float)y / ROWS;
      float a = 0.38f + 0.32f * (w / 1.5f);
      col = dim(mix(rgb(0x389ed0), rgb(0x103658), d), a);
      return true;
    }
    case NIGHT: {
      float n = hash2(x, y);
      if (n < 0.945f) return false;
      float tw = 0.5f + 0.5f * sinf(t * (1.2f + n * 2.5f) + n * 60.0f);
      ch = tw > 0.88f ? '+' : (tw > 0.45f ? '*' : '.');
      col = dim(mix(rgb(0x485285), rgb(0xcad6ff), tw), 0.42f + 0.55f * tw);
      return true;
    }
    case SPACE: {
      int layer = (x + y) % 3;
      int sx = ((int)floorf(x + t * (0.7f + layer * 0.8f))) % COLS;
      float n = hash2(sx, y);
      if (n < 0.91f) return false;
      static const uint32_t cols[] = { 0x8a50c0, 0x5078d0, 0xd065d0, 0x60d4e8 };
      ch = layer == 2 ? '*' : (layer == 1 ? '+' : '.');
      col = dim(rgb(cols[(int)(n * 40) % 4]), 0.45f + 0.35f * layer);
      return true;
    }
    default: return false;
  }
}

// ---------- layout ----------
struct Layout {
  int x0, width, y0, height;
  bool cell[ROWS][COLS];
  bool inBand(int x, int y) const {
    return y >= y0 - 1 && y <= y0 + height + 1 && x >= x0 - 2 && x < x0 + width + 2;
  }
};

static void buildLayout(const char* text, Layout& L) {
  memset(L.cell, 0, sizeof(L.cell));
  L.y0 = 4; L.height = 9;
  int n = strlen(text), w;
  int total = 0;
  for (int i = 0; i < n; i++) { glyphFor(text[i], w); total += w * 2; }
  // tighter gaps around the colon
  int gaps[8] = {0};
  for (int i = 0; i < n - 1; i++) gaps[i] = (text[i] == ':' || text[i + 1] == ':') ? 1 : 2;
  for (int i = 0; i < n - 1; i++) total += gaps[i];
  L.x0 = (COLS - total) / 2; L.width = total;
  int x = L.x0;
  for (int i = 0; i < n; i++) {
    const char* const* g = glyphFor(text[i], w);
    for (int r = 0; r < 9; r++)
      for (int c = 0; c < w; c++)
        if (g[r][c] == '#') {
          int cx = x + c * 2, cy = L.y0 + r;
          if (cx >= 0 && cx + 1 < COLS && cy < ROWS) { L.cell[cy][cx] = true; L.cell[cy][cx + 1] = true; }
        }
    x += w * 2 + (i < n - 1 ? gaps[i] : 0);
  }
}

// ---------- state ----------
static int mode = WATER;            // 0..3 = face, FACE_COUNT = auto
static Face face = WATER;
static bool wifiOk = false, timeOk = false;
static String weatherStr = "";
static float geoLat = 0, geoLon = 0;
static bool geoOk = false;
static unsigned long lastWeather = 0, lastTap = 0, lastFrame = 0, lastLdr = 0;
static float blLevel = 255;

static const char* DAYS[] = { "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT" };
static const char* MONTHS[] = { "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };

static Face daypartFace(const tm& tmv) {
  float h = tmv.tm_hour + tmv.tm_min / 60.0f;
  if (h >= 5 && h < 9) return SUNRISE;
  if (h >= 9 && h < 17) return WATER;
  if (h >= 17 && h < 22) return NIGHT;
  return SPACE;
}

// ---------- weather ----------
static const char* wmoText(int code) {
  if (code == 0) return "CLEAR";
  if (code == 1) return "MOSTLY CLEAR";
  if (code == 2) return "PARTLY CLOUDY";
  if (code == 3) return "OVERCAST";
  if (code == 45 || code == 48) return "FOG";
  if (code >= 51 && code <= 57) return "DRIZZLE";
  if (code >= 61 && code <= 67) return "RAIN";
  if (code >= 71 && code <= 77) return "SNOW";
  if (code >= 80 && code <= 82) return "SHOWERS";
  if (code == 85 || code == 86) return "SNOW SHOWERS";
  if (code >= 95) return "THUNDERSTORM";
  return "";
}

static bool fetchGeo() {
  HTTPClient http;
  http.setTimeout(6000);
  if (!http.begin("http://ip-api.com/json/?fields=status,lat,lon")) return false;
  int code = http.GET();
  bool ok = false;
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString()) && doc["status"] == "success") {
      geoLat = doc["lat"]; geoLon = doc["lon"]; ok = true;
    }
  }
  http.end();
  return ok;
}

static void fetchWeather() {
  if (!wifiOk) return;
  if (!geoOk) geoOk = fetchGeo();
  if (!geoOk) { weatherStr = "NO LOCATION"; return; }
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(8000);
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(geoLat, 4) +
               "&longitude=" + String(geoLon, 4) +
               "&current=temperature_2m,weather_code&temperature_unit=fahrenheit";
  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (!deserializeJson(doc, http.getString())) {
      int temp = (int)lroundf(doc["current"]["temperature_2m"].as<float>());
      int wmo = doc["current"]["weather_code"];
      weatherStr = String(temp) + "\xF8" "F  " + wmoText(wmo);
    }
  } else {
    weatherStr = "WEATHER ERR";
  }
  http.end();
}

// ---------- render ----------
static void drawDither(int px, int py) {
  for (int y = 0; y < CH; y += 2)
    for (int x = (y / 2) % 2; x < CW; x += 2)
      spr.drawPixel(px + x, py + y, TFT_BLACK);
}

static void renderFrame() {
  tm tmv;
  timeOk = wifiOk && getLocalTime(&tmv, 0);
  char timeStr[6];
  int sec = 0;
  bool pm = false;
  if (timeOk) {
    int h12 = tmv.tm_hour % 12; if (h12 == 0) h12 = 12;
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", h12, tmv.tm_min);
    sec = tmv.tm_sec; pm = tmv.tm_hour >= 12;
    if (mode == FACE_COUNT) face = daypartFace(tmv);
  } else {
    strcpy(timeStr, "--:--");
    sec = (millis() / 1000) % 60;
  }
  if (mode < FACE_COUNT) face = (Face)mode;

  static Layout L;
  buildLayout(timeStr, L);
  const FaceDef& F = FACES[face];
  float t = millis() / 1000.0f;

  for (int pass = 0; pass < SCREEN_H / SPR_H; pass++) {
    int off = pass * SPR_H;
    spr.fillSprite(TFT_BLACK);

    // background chars (rows above the bottom bar; skip the band behind digits)
    for (int y = 0; y < ROWS - 2; y++) {
      int py = y * CH - off;
      if (py + CH <= 0 || py >= SPR_H) continue;
      for (int x = 0; x < COLS; x++) {
        if (L.inBand(x, y)) continue;
        char ch; RGB col;
        if (bgCell(face, x, y, t, ch, col)) spr.drawChar(x * CW, py + 2, ch, c565(col), TFT_BLACK, 1);
      }
    }

    // digits: soft inner bevel, gradient sweeping with time
    for (int y = L.y0; y < L.y0 + L.height; y++) {
      int py = y * CH - off;
      if (py + CH <= 0 || py >= SPR_H) continue;
      for (int x = 0; x < COLS; x++) {
        if (!L.cell[y][x]) continue;
        float u = (float)(x - L.x0) / L.width + t * 0.08f + (y - L.y0) * 0.02f;
        RGB col = palette(F.pal, F.n, u);

        int px = x * CW;
        spr.fillRect(px, py, CW, CH, c565(col));

        // Sub-pixel edge styling for subtle bevel & illumination
        bool topEdge = (y == L.y0) || !L.cell[y - 1][x];
        bool botEdge = (y == L.y0 + L.height - 1) || !L.cell[y + 1][x];
        bool leftEdge = (x == 0) || !L.cell[y][x - 1];
        bool rightEdge = (x == COLS - 1) || !L.cell[y][x + 1];

        if (topEdge) {
          spr.drawFastHLine(px, py, CW, c565(dim(col, 1.25f)));
        }
        if (leftEdge) {
          spr.drawFastVLine(px, py, CH, c565(dim(col, 1.15f)));
        }
        if (botEdge) {
          spr.drawFastHLine(px, py + CH - 1, CW, c565(dim(col, 0.70f)));
        }
        if (rightEdge) {
          spr.drawFastVLine(px + CW - 1, py, CH, c565(dim(col, 0.75f)));
        }

        if (F.textured) drawDither(px, py);
      }
    }

    // hairline separator above seconds timeline
    {
      int sepY = (L.y0 + L.height) * CH + 3 - off;
      if (sepY >= 0 && sepY < SPR_H) {
        int xStart = (L.x0 - 1) * CW;
        int xLen = (L.width + 2) * CW;
        spr.drawFastHLine(xStart, sepY, xLen, c565(dim(rgb(0x8899aa), 0.15f)));
      }
    }

    // seconds line under the digits
    {
      int y = L.y0 + L.height + 1, py = y * CH - off;
      if (py + CH > 0 && py < SPR_H) {
        uint16_t dotDim = c565(dim(rgb(0xffffff), 0.18f));
        uint16_t markCol = c565(dim(palette(F.pal, F.n, 0.5f), 0.40f));
        for (int i = 0; i < L.width; i++) {
          int dotX = (L.x0 + i) * CW + 2;
          if (i == 0 || i == L.width / 2 || i == L.width - 1) {
            spr.fillRect(dotX, py + 4, 2, 4, markCol);
          } else {
            spr.fillRect(dotX, py + 5, 2, 2, dotDim);
          }
        }
        int sx = L.x0 + (sec * (L.width - 1)) / 59;
        RGB secCol = palette(F.pal, F.n, sec / 60.0f);
        // glowing head with soft halo
        spr.fillRect(sx * CW, py + 3, 6, 6, c565(dim(secCol, 0.40f)));
        spr.fillRect(sx * CW + 1, py + 4, 4, 4, c565(secCol));
        spr.drawPixel(sx * CW + 2, py + 5, TFT_WHITE);
        spr.drawPixel(sx * CW + 3, py + 5, TFT_WHITE);
      }
    }

    // bottom bar: hairline divider + elegant metadata
    {
      int divY = (ROWS - 1) * CH - off;
      if (divY >= 0 && divY < SPR_H) {
        spr.drawFastHLine(CW, divY, SCREEN_W - 2 * CW, c565(dim(rgb(0x607080), 0.20f)));
      }
      int y = ROWS - 1, py = y * CH - off;
      if (py + CH > 0 && py < SPR_H) {
        spr.setTextFont(1);
        uint16_t muted = c565(rgb(0x9aa8b8));
        char date[24] = "SYNCING";
        if (timeOk) snprintf(date, sizeof(date), "%s, %s %d", DAYS[tmv.tm_wday], MONTHS[tmv.tm_mon], tmv.tm_mday);
        spr.setTextColor(muted, TFT_BLACK);
        spr.setTextDatum(TL_DATUM);
        spr.drawString(date, 2 * CW, py + 2);

        String mid = String(timeOk ? (pm ? "PM  " : "AM  ") : "") + (mode == FACE_COUNT ? "AUTO \xB7 " : "\xB7 ") + FACE_NAMES[face];
        spr.setTextColor(c565(dim(palette(F.pal, F.n, 0.5f), 0.90f)), TFT_BLACK);
        spr.setTextDatum(TC_DATUM);
        spr.drawString(mid, SCREEN_W / 2, py + 2);

        String wx = wifiOk ? (weatherStr.length() ? weatherStr : "WEATHER...") : "OFFLINE";
        spr.setTextColor(muted, TFT_BLACK);
        spr.setTextDatum(TR_DATUM);
        spr.drawString(wx, SCREEN_W - 2 * CW, py + 2);
      }
    }

    spr.pushSprite(0, off);
  }
}

// ---------- boot screen ----------
static void bootMsg(const char* line1, const char* line2 = "") {
  tft.fillScreen(TFT_BLACK);
  tft.drawRoundRect(8, 8, SCREEN_W - 16, SCREEN_H - 16, 8, tft.color565(0x28, 0x34, 0x44));
  tft.setTextDatum(MC_DATUM);
  tft.setTextFont(2);
  tft.setTextColor(tft.color565(0x8e, 0xe7, 0xf8), TFT_BLACK);
  tft.drawString("A S C I I   C L O C K", SCREEN_W / 2, SCREEN_H / 2 - 24);
  tft.drawFastHLine(SCREEN_W / 2 - 60, SCREEN_H / 2 - 8, 120, tft.color565(0x30, 0x42, 0x56));
  tft.setTextFont(1);
  tft.setTextColor(tft.color565(0xb4, 0xc4, 0xd8), TFT_BLACK);
  tft.drawString(line1, SCREEN_W / 2, SCREEN_H / 2 + 10);
  tft.setTextColor(tft.color565(0x72, 0x82, 0x96), TFT_BLACK);
  tft.drawString(line2, SCREEN_W / 2, SCREEN_H / 2 + 28);
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

  touchSPI.begin(XPT_CLK, XPT_MISO, XPT_MOSI, XPT_CS);
  touch.begin(touchSPI);
  touch.setRotation(1);

  bootMsg("connecting wifi...");
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setAPCallback([](WiFiManager*) {
    bootMsg("join wifi \"" AP_NAME "\"", "then open 192.168.4.1");
  });
  wifiOk = wm.autoConnect(AP_NAME);

  if (wifiOk) {
    bootMsg("syncing time...");
    configTzTime(TZ_POSIX, "pool.ntp.org", "time.nist.gov");
    tm tmv;
    for (int i = 0; i < 40 && !getLocalTime(&tmv, 250); i++) {}
    fetchWeather();
    lastWeather = millis();
  } else {
    bootMsg("offline", "tap to cycle faces");
    delay(1200);
  }

  spr.setColorDepth(16);
  if (!spr.createSprite(SCREEN_W, SPR_H)) {
    spr.setColorDepth(8);
    spr.createSprite(SCREEN_W, SPR_H);
  }
  spr.setTextWrap(false);
}

void loop() {
  unsigned long now = millis();

  // touch: tap cycles sunrise → water → night → space → auto
  if (touch.tirqTouched() && touch.touched() && now - lastTap > 450) {
    mode = (mode + 1) % (FACE_COUNT + 1);
    lastTap = now;
  }

  // auto-dim from the light sensor (dark room → softer backlight)
  if (now - lastLdr > 500) {
    lastLdr = now;
    int ldr = analogRead(LDR_PIN);                     // higher = darker on the CYD
    float target = map(constrain(ldr, 300, 3800), 3800, 300, 70, 255);
    blLevel += (target - blLevel) * 0.2f;
    ledcWrite(BL_CH, (int)blLevel);
  }

  if (wifiOk && now - lastWeather > 15UL * 60UL * 1000UL) {
    lastWeather = now;
    fetchWeather();
  }

  if (now - lastFrame >= 100) {
    lastFrame = now;
    renderFrame();
  }
}
