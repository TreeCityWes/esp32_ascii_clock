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

// Backlight PWM: Arduino-ESP32 core 3.x replaced the channel-based LEDC API
// with a pin-based one, so route writes through one helper.
static void setBacklight(int duty) {
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcWrite(BL_PIN, duty);
#else
  ledcWrite(BL_CH, duty);
#endif
}

TFT_eSPI tft;
TFT_eSprite spr(&tft);
SPIClass touchSPI(VSPI);
XPT2046_Touchscreen touch(XPT_CS, XPT_IRQ);

// ---------- grid ----------
static const int COLS = 53, ROWS = 20, CW = 6, CH = 12;
static const int SCREEN_W = 320, SCREEN_H = 240, SPR_H = 120;
static const int FIELD_ROWS = ROWS - 2;  // rows of animated field above the footer

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
struct FaceDef { const uint32_t* pal; int n; };
static const FaceDef FACES[FACE_COUNT] = {
  { PAL_SUNRISE, 5 }, { PAL_WATER, 5 }, { PAL_NIGHT, 5 }, { PAL_SPACE, 5 },
};

// ---------- ASCII field ----------
// Every face is a dense, flowing field of characters covering the whole grid.
// A cell's wave value v in [0,1] picks a glyph from the face's density ramp
// (light -> heavy) and sets its brightness, classic ASCII-art style.
static const char RAMP_SUNRISE[] = " .:-=+*#%@";
static const char RAMP_WATER[]   = " .,:~-=+*#%@";
static const char RAMP_NIGHT[]   = " .:-=";
static const char RAMP_CODE[]    = "01<>[]{}/\\|=+*#@";
static const char RAMP_DIGIT[]   = "#%@";
#define RAMP(r, v) rampChar(r, sizeof(r) - 1, v)

static float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static char rampChar(const char* ramp, int n, float v) { return ramp[(int)(clamp01(v) * (n - 1) + 0.5f)]; }

// Field cell for a face. Returns false for empty. `col` is pre-dimmed.
static bool fieldCell(Face f, int x, int y, float t, char& ch, RGB& col) {
  const int rows = FIELD_ROWS;
  switch (f) {
    case SUNRISE: {
      // plasma sky (violet -> rose) above a horizon of ember waves
      const int horizon = 12;
      float dx = (x - COLS / 2) * 0.47f, dy = (y - horizon) * 1.18f;
      float p = sinf(x * 0.17f + t * 0.55f) + sinf(y * 0.42f - t * 0.35f)
              + sinf(x * 0.09f + y * 0.21f + t * 0.45f)
              + sinf(sqrtf(dx * dx + dy * dy) * 0.36f - t * 0.8f);
      float v = clamp01((p + 4.0f) / 8.0f);
      if (y >= horizon) {
        float w = 0.5f + 0.5f * sinf(x * 0.26f + t * 1.4f + (y - horizon) * 0.8f);
        v = clamp01(0.55f * v + 0.45f * w);
        float d = (float)(y - horizon) / (rows - horizon);
        col = dim(mix(rgb(0xe07038), rgb(0x3a1616), d), 0.22f + 0.6f * v);
      } else {
        float d = (float)y / horizon;
        col = dim(mix(rgb(0x5a3a8c), rgb(0xe8647c), d), 0.18f + 0.62f * v);
      }
      ch = RAMP(RAMP_SUNRISE, v);
      return ch != ' ';
    }
    case WATER: {
      // interference of several wave trains; crests brighten toward ice-blue
      float w = sinf(x * 0.21f + t * 1.3f + y * 0.55f)
              + 0.6f * sinf(x * 0.45f - t * 0.85f + y * 0.3f)
              + 0.4f * sinf((x + y) * 0.14f + t * 0.5f)
              + 0.3f * sinf(y * 0.9f - t * 0.7f);
      float v = clamp01((w + 2.3f) / 4.6f);
      float depth = (float)y / rows;
      col = dim(mix(rgb(0x0a2a48), rgb(0x52d4f4), v * v), (0.3f + 0.7f * v) * (1.0f - 0.3f * depth));
      ch = RAMP(RAMP_WATER, v);
      return ch != ' ';
    }
    case NIGHT: {
      // slow nebula haze with twinkling stars on top
      float n = hash2(x, y);
      if (n > 0.955f) {
        float tw = 0.5f + 0.5f * sinf(t * (1.2f + n * 2.5f) + n * 60.0f);
        ch = tw > 0.85f ? '+' : (tw > 0.45f ? '*' : '.');
        col = dim(mix(rgb(0x485285), rgb(0xd8e2ff), tw), 0.45f + 0.55f * tw);
        return true;
      }
      float p = sinf(x * 0.12f + t * 0.25f) + sinf(y * 0.3f - t * 0.2f) + sinf(x * 0.07f + y * 0.15f + t * 0.3f);
      float v = clamp01((p + 3.0f) / 6.0f) * 0.6f;
      col = dim(mix(rgb(0x18204a), rgb(0x6c7cc0), v), 0.25f + 0.6f * v);
      ch = RAMP(RAMP_NIGHT, v);
      return ch != ' ';
    }
    case SPACE: {
      // cascading code rain: per-column streams of glyphs, brightness rolling in waves
      float speed = 0.5f + hash2(x, 7) * 1.3f;
      float len = 6.0f + hash2(x, 11) * 10.0f;
      float span = rows + len + 4.0f;
      float head = fmodf(t * speed * 3.0f + hash2(x, 13) * span, span) - len - 2.0f;
      float dist = head - y;
      float wave = 0.5f + 0.5f * sinf(x * 0.18f - t * 0.9f);
      if (dist < 0.0f || dist >= len) {
        if (hash2(x, y) < 0.93f) return false;
        ch = '.';
        col = dim(rgb(0x5060a0), 0.3f + 0.3f * wave);
        return true;
      }
      float b = 1.0f - dist / len;
      ch = RAMP_CODE[(int)(hash2(x, y + (int)(t * 4.0f)) * 16.0f) % 16];
      static const uint32_t cols[] = { 0x9b6ef3, 0x48caf5, 0xf472d0, 0x63e6bf };
      RGB base = rgb(cols[(int)(hash2(x, 3) * 4.0f) % 4]);
      if (dist < 1.0f) { ch = '@'; base = mix(base, rgb(0xffffff), 0.6f); }
      col = dim(base, (0.25f + 0.75f * b) * (0.55f + 0.45f * wave));
      return true;
    }
    default: return false;
  }
}

// ---------- layout ----------
struct Layout {
  int x0, width, y0, height;
  bool cell[ROWS][COLS];
  // The time sits on a framed "plate": the row above the digits and the
  // seconds-track row below it are its top/bottom rules. The digit block is
  // 50 of 53 columns wide, so the frame lives at pixel level and the whole
  // group is nudged by `shift` px to sit dead-centre on the panel.
  int plateTop() const { return y0 - 1; }
  int plateBot() const { return y0 + height + 1; }
  int shift() const { return (SCREEN_W - width * CW) / 2 - x0 * CW; }
  int frameL() const { return x0 * CW + shift() - 4; }
  int frameR() const { return (x0 + width) * CW + shift() + 3; }
  int frameT() const { return plateTop() * CH + CH / 2; }
  int frameB() const { return plateBot() * CH + CH / 2; }
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

    // ASCII field: every cell above the footer is a flowing character, except
    // on the plate, where the field drops to a faint ghost so the digits (heavy
    // glyphs over a tinted block, gradient sweeping with time) read against
    // near-black instead of against their own palette.
    const int shift = L.shift(), pTop = L.plateTop(), pBot = L.plateBot();
    for (int y = 0; y < FIELD_ROWS; y++) {
      int py = y * CH - off;
      if (py + CH <= 0 || py >= SPR_H) continue;
      bool onPlate = (y >= pTop && y <= pBot);
      for (int x = 0; x < COLS; x++) {
        if (L.cell[y][x]) {
          int px = x * CW + shift;
          float u = (float)(x - L.x0) / L.width + t * 0.08f + (y - L.y0) * 0.02f;
          RGB col = palette(F.pal, F.n, u);
          float v = 0.5f + 0.5f * sinf(x * 0.45f + y * 0.6f + t * 2.0f);
          uint16_t block = c565(dim(col, 0.30f));
          spr.fillRect(px, py, CW, CH, block);
          spr.drawChar(px, py + 2, RAMP(RAMP_DIGIT, v), c565(dim(col, 0.85f + 0.15f * v)), block, 1);
          continue;
        }
        char ch; RGB col;
        if (onPlate) {
          if (y == pTop || y == pBot) continue;                  // rule rows stay clear
          if (x < L.x0 || x >= L.x0 + L.width) continue;         // margins beside the frame
          if (!fieldCell(face, x, y, t, ch, col)) continue;
          spr.drawChar(x * CW + shift, py + 2, ch, c565(dim(col, 0.14f)), TFT_BLACK, 1);
          continue;
        }
        if (!fieldCell(face, x, y, t, ch, col)) continue;
        spr.drawChar(x * CW, py + 2, ch, c565(col), TFT_BLACK, 1);
      }
    }

    // plate frame: hairline rules in the face colour with brighter corner
    // marks; the bottom rule doubles as the seconds track.
    // (TFT_eSprite clips primitives, so we just offset by the pass.)
    {
      const int fl = L.frameL(), fr = L.frameR(), ft = L.frameT() - off, fb = L.frameB() - off;
      RGB accent = palette(F.pal, F.n, 0.5f);
      uint16_t rule = c565(dim(accent, 0.42f)), mark = c565(dim(accent, 0.95f));
      spr.drawFastHLine(fl, ft, fr - fl + 1, rule);
      spr.drawFastHLine(fl, fb, fr - fl + 1, rule);
      spr.drawFastVLine(fl, ft, fb - ft + 1, rule);
      spr.drawFastVLine(fr, ft, fb - ft + 1, rule);
      const int k = 5;  // corner mark length
      spr.drawFastHLine(fl, ft, k, mark); spr.drawFastVLine(fl, ft, k, mark);
      spr.drawFastHLine(fr - k + 1, ft, k, mark); spr.drawFastVLine(fr, ft, k, mark);
      spr.drawFastHLine(fl, fb, k, mark); spr.drawFastVLine(fl, fb - k + 1, k, mark);
      spr.drawFastHLine(fr - k + 1, fb, k, mark); spr.drawFastVLine(fr, fb - k + 1, k, mark);

      // seconds track along the bottom rule: quarter ticks + glowing head
      const int tx0 = L.x0 * CW + shift, tw = L.width * CW;
      for (int q = 0; q <= 4; q++) {
        int tx = tx0 + 1 + ((tw - 4) * q) / 4;
        spr.fillRect(tx, fb - 2, 2, 5, c565(dim(accent, q % 2 ? 0.45f : 0.75f)));
      }
      int sx = tx0 + 2 + ((tw - 10) * sec) / 59;
      RGB secCol = palette(F.pal, F.n, sec / 60.0f);
      spr.fillRect(sx - 1, fb - 3, 6, 7, c565(dim(secCol, 0.40f)));
      spr.fillRect(sx, fb - 2, 4, 5, c565(secCol));
      spr.drawPixel(sx + 1, fb, TFT_WHITE);
      spr.drawPixel(sx + 2, fb, TFT_WHITE);
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
        // date + AM/PM left · face center · weather right (condition drops back
        // to just the temperature if it would collide with the face label)
        char left[24] = "SYNCING";
        if (timeOk) snprintf(left, sizeof(left), "%s, %s %d  %s", DAYS[tmv.tm_wday], MONTHS[tmv.tm_mon], tmv.tm_mday, pm ? "PM" : "AM");
        spr.setTextColor(muted, TFT_BLACK);
        spr.setTextDatum(TL_DATUM);
        spr.drawString(left, 2 * CW, py + 2);

        String mid = String(mode == FACE_COUNT ? "AUTO " : "") + FACE_NAMES[face];
        int midW = mid.length() * CW, midX = (SCREEN_W - midW) / 2;
        spr.setTextColor(c565(dim(palette(F.pal, F.n, 0.5f), 0.90f)), TFT_BLACK);
        spr.setTextDatum(TL_DATUM);
        spr.drawString(mid, midX, py + 2);

        String wx = wifiOk ? (weatherStr.length() ? weatherStr : "WEATHER...") : "OFFLINE";
        int wxMax = (SCREEN_W - 2 * CW) - (midX + midW + 2 * CW);
        if ((int)wx.length() * CW > wxMax) { int cut = wx.indexOf("  "); if (cut > 0) wx = wx.substring(0, cut); }
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
#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
  ledcAttach(BL_PIN, 5000, 8);      // core 3.x: channel is implicit, keyed by pin
#else
  ledcSetup(BL_CH, 5000, 8);
  ledcAttachPin(BL_PIN, BL_CH);
#endif
  setBacklight(255);
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
    setBacklight((int)blLevel);
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
