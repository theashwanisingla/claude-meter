/*
 * Claude Usage Monitor — M5Stack Core2
 *
 * Three views. Tap left edge to go back, right edge to go forward,
 * center to refresh.
 *
 *   View 0: Overview  — usage bars (what you see most)
 *   View 1: Activity  — recent usage chart
 *   View 2: Device    — WiFi, battery, sync info
 *
 * Credentials and mode in secrets.h (gitignored).
 */

#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include "secrets.h"

#ifdef ANTHROPIC_API_KEY
#include <WiFiClientSecure.h>
#endif

// ── Config ────────────────────────────────────────────────────────────────────
const unsigned long REFRESH_MS = 30UL * 1000;
const int           NUM_VIEWS  = 3;
const int           HIST_SIZE  = 15;

#ifdef ANTHROPIC_API_KEY
static const char* API_URL = "https://api.anthropic.com/v1/messages";
#endif

// ── Colors (RGB565) ───────────────────────────────────────────────────────────
const uint16_t C_BG       = 0x0000;
const uint16_t C_HEADER   = 0x10A2;
const uint16_t C_CARD     = 0x1082;
const uint16_t C_WHITE    = 0xFFFF;
const uint16_t C_GRAY     = 0x7BEF;
const uint16_t C_BADGE    = 0x2945;
const uint16_t C_BARBG    = 0x18C3;
const uint16_t C_GREEN    = 0x4BC7;
const uint16_t C_AMBER    = 0xFDA0;
const uint16_t C_RED      = 0xF800;
const uint16_t C_SALMON   = 0xCB8B;
const uint16_t C_OFFWHITE = 0xC618;

// ── Layout (320 x 240) ────────────────────────────────────────────────────────
#define SCR_W   320
#define SCR_H   240
#define HDR_H    34
#define CARD_H   85
#define GAP       4
#define CARD1_Y (HDR_H + GAP)
#define CARD2_Y (CARD1_Y + CARD_H + GAP)
#define STAT_Y  (CARD2_Y + CARD_H + GAP)
#define CARD_X    5
#define CARD_W  310

// ── Data ──────────────────────────────────────────────────────────────────────
struct UsageData {
    bool ok       = false;
    int  httpCode = 0;
#ifdef ANTHROPIC_API_KEY
    int  req_pct       = 0;
    int  req_remaining = 0;
    int  req_limit     = 0;
    int  tok_pct       = 0;
    long tok_remaining = 0;
    long tok_limit     = 0;
#else
    int  session_pct   = 0;
    int  session_reset = 0;
    int  weekly_pct    = 0;
    int  weekly_reset  = 0;
    char status[16]    = "unknown";
    int  delta         = 0;
    bool spike         = false;
#endif
};

UsageData     data;
bool          wifiOk    = false;
bool          fetching  = false;
unsigned long lastFetch = 0;
int           curView   = 0;

int histBuf[HIST_SIZE];
int histCount = 0;
int histHead  = 0;

void drawAll();

// ── History ───────────────────────────────────────────────────────────────────
void histPush(int pct) {
    histBuf[histHead] = pct;
    histHead = (histHead + 1) % HIST_SIZE;
    if (histCount < HIST_SIZE) histCount++;
}

int histGet(int i) {
    int start = (histHead - histCount + HIST_SIZE * 2) % HIST_SIZE;
    return histBuf[(start + i) % HIST_SIZE];
}

// ── JSON helpers ──────────────────────────────────────────────────────────────
static int jsonInt(const String& s, const char* key) {
    String k = "\"" + String(key) + "\":";
    int idx = s.indexOf(k);
    if (idx < 0) return 0;
    int p = idx + k.length();
    while (p < (int)s.length() && s[p] == ' ') p++;
    int e = p;
    if (s[e] == '-') e++;
    while (e < (int)s.length() && isdigit(s[e])) e++;
    return s.substring(p, e).toInt();
}
static String jsonStr(const String& s, const char* key) {
    String k = "\"" + String(key) + "\":\"";
    int idx = s.indexOf(k);
    if (idx < 0) return "";
    int p = idx + k.length();
    int e = s.indexOf('"', p);
    return (e > p) ? s.substring(p, e) : "";
}
static bool jsonBool(const String& s, const char* key) {
    String k = "\"" + String(key) + "\":";
    int idx = s.indexOf(k);
    if (idx < 0) return false;
    int p = idx + k.length();
    while (p < (int)s.length() && s[p] == ' ') p++;
    return s.substring(p, p + 4) == "true";
}

// ── Format helpers ────────────────────────────────────────────────────────────
static String fmt5h(int m) {
    if (m <= 0) return "now";
    int h = m / 60, r = m % 60;
    return h > 0 ? String(h) + "h " + String(r) + "m" : String(r) + "m";
}
static String fmt7d(int m) {
    if (m <= 0) return "now";
    int d = m / 1440, h = (m % 1440) / 60;
    return d > 0 ? String(d) + "d " + String(h) + "h" : String(h) + "h";
}
static String fmtN(long n) {
    if (n >= 1000000L) return String((long)(n / 1000000L)) + "M";
    if (n >= 1000)     return String((long)(n / 1000)) + "K";
    return String((int)n);
}
static String syncAgo() {
    if (!lastFetch) return "never";
    unsigned long s = (millis() - lastFetch) / 1000;
    if (s < 5)  return "just now";
    if (s < 60) return String((int)s) + "s ago";
    return String((int)(s / 60)) + "m ago";
}

#ifdef ANTHROPIC_API_KEY
static const char* mood(int pct) {
    if (pct >= 95) return "Maxed!";
    if (pct >= 80) return "Heavy";
    if (pct >= 60) return "Cooking...";
    if (pct >= 40) return "Baking...";
    if (pct >= 20) return "Light";
    if (pct >=  5) return "Active";
    return "Idle";
}
#else
static const char* mood(int pct, const char* st) {
    if (!strcmp(st, "limited")) return "Limited";
    if (pct < 5)  return "Idle";
    if (pct < 25) return "Light";
    if (pct < 50) return "Baking...";
    if (pct < 75) return "Cooking...";
    if (pct < 90) return "Heavy";
    return "Maxed!";
}
#endif

// ── WiFi ──────────────────────────────────────────────────────────────────────
void connectWiFi() {
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    for (int i = 0; i < 24 && WiFi.status() != WL_CONNECTED; i++) delay(500);
    wifiOk = (WiFi.status() == WL_CONNECTED);
}

// ── Fetch ─────────────────────────────────────────────────────────────────────
void fetchData() {
    if (!wifiOk) return;
    fetching = true;
    drawAll();

#ifdef ANTHROPIC_API_KEY
    WiFiClientSecure wc;
    wc.setInsecure();
    HTTPClient http;
    http.begin(wc, API_URL);
    http.addHeader("x-api-key", ANTHROPIC_API_KEY);
    http.addHeader("anthropic-version", "2023-06-01");
    http.addHeader("Content-Type", "application/json");
    static const char* hdrs[] = {
        "anthropic-ratelimit-requests-limit",
        "anthropic-ratelimit-requests-remaining",
        "anthropic-ratelimit-tokens-limit",
        "anthropic-ratelimit-tokens-remaining",
    };
    http.collectHeaders(hdrs, 4);
    String body = "{\"model\":\"claude-haiku-4-5-20251001\",\"max_tokens\":1,"
                  "\"messages\":[{\"role\":\"user\",\"content\":\".\"}]}";
    int code = http.POST(body);
    data.httpCode = code;
    if (code > 0 && code < 500) {
        int  rl = http.header("anthropic-ratelimit-requests-limit").toInt();
        int  rr = http.header("anthropic-ratelimit-requests-remaining").toInt();
        long tl = http.header("anthropic-ratelimit-tokens-limit").toInt();
        long tr = http.header("anthropic-ratelimit-tokens-remaining").toInt();
        data.req_limit = rl; data.req_remaining = rr;
        data.tok_limit = tl; data.tok_remaining = tr;
        data.req_pct = rl > 0 ? (int)(100.0f * (rl - rr) / rl) : 0;
        data.tok_pct = tl > 0 ? (int)(100.0f * (tl - tr) / tl) : 0;
        data.ok = true;
        histPush(data.req_pct);
    } else {
        data.ok = false;
    }
    http.end();

#else
    HTTPClient http;
    http.begin(PROXY_URL);
    http.setTimeout(10000);
    int code = http.GET();
    data.httpCode = code;
    if (code == 200) {
        String body = http.getString();
        data.session_pct   = jsonInt(body, "s");
        data.session_reset = jsonInt(body, "sr");
        data.weekly_pct    = jsonInt(body, "w");
        data.weekly_reset  = jsonInt(body, "wr");
        String st = jsonStr(body, "st");
        strncpy(data.status, st.c_str(), sizeof(data.status) - 1);
        data.delta = jsonInt(body, "delta");
        data.spike = jsonBool(body, "spike");
        data.ok    = jsonBool(body, "ok");
        if (data.ok) histPush(data.session_pct);
    } else {
        data.ok = false;
    }
    http.end();
#endif

    lastFetch = millis();
    fetching  = false;
}

// ── Draw: header with view name, page dots, battery ──────────────────────────
static const char* VIEW_NAMES[] = { "Overview", "Activity", "Device" };

void drawHeader() {
    M5.Lcd.fillRect(0, 0, SCR_W, HDR_H, C_HEADER);

    // Clawd mascot
    const int z = 2, mx = 7, my = 7;
    M5.Lcd.fillRoundRect(mx,       my + z*2, z*10, z*6, z, C_SALMON);
    M5.Lcd.fillRect(mx + z*1, my,            z*2,  z*3,    C_SALMON);
    M5.Lcd.fillRect(mx + z*7, my,            z*2,  z*3,    C_SALMON);
    M5.Lcd.fillRect(mx + z*2, my + z*3,      z*2,  z*2,    C_BG);
    M5.Lcd.fillRect(mx + z*6, my + z*3,      z*2,  z*2,    C_BG);
    M5.Lcd.fillRect(mx + z*2, my + z*7,      z*2,  z*3,    C_SALMON);
    M5.Lcd.fillRect(mx + z*6, my + z*7,      z*2,  z*3,    C_SALMON);

    // View name
    M5.Lcd.setTextColor(C_WHITE, C_HEADER);
    M5.Lcd.drawString(VIEW_NAMES[curView], 34, 5, 4);

    // Page dots (fixed position between title and battery)
    for (int i = 0; i < NUM_VIEWS; i++) {
        int dx = 192 + i * 10;
        int dy = HDR_H / 2;
        if (i == curView) M5.Lcd.fillCircle(dx, dy, 3, C_WHITE);
        else              M5.Lcd.drawCircle(dx, dy, 3, C_GRAY);
    }

    // Battery icon
    int bat = M5.Power.getBatteryLevel();
    bool chg = M5.Power.isCharging();
    const int bW = 44, bH = 22, nW = 5, nH = 10, bdr = 2;
    int bx = SCR_W - bW - nW - 5, by = (HDR_H - bH) / 2;
    uint16_t bc = bat > 20 ? C_GREEN : C_RED;
    M5.Lcd.fillRect(bx, by, bW + nW, bH, C_HEADER);
    M5.Lcd.fillRect(bx + bW, by + (bH - nH) / 2, nW, nH, bc);
    M5.Lcd.drawRoundRect(bx, by, bW, bH, 3, bc);
    int fw = max(0, min((int)((float)(bW - bdr*2 - 2) * bat / 100.0f), bW - bdr*2 - 2));
    if (fw > 0) M5.Lcd.fillRect(bx + bdr + 1, by + bdr + 1, fw, bH - bdr*2 - 2, bc);
    char b[6];
    snprintf(b, 6, chg ? "+%d" : "%d", bat);
    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.drawCentreString(b, bx + bW / 2, by + (bH - 16) / 2, 2);
}

// ── Draw: standard card with progress bar ────────────────────────────────────
void drawCard(int cy, int pct, const char* label, const char* sub) {
    M5.Lcd.fillRoundRect(CARD_X, cy, CARD_W, CARD_H, 8, C_CARD);
    char buf[8];
    snprintf(buf, 8, "%d%%", pct);
    M5.Lcd.setTextColor(C_WHITE, C_CARD);
    M5.Lcd.drawString(buf, CARD_X + 15, cy + 10, 4);
    const int bW = 80, bH = 22;
    int bX = CARD_X + CARD_W - bW - 12, bY = cy + 12;
    M5.Lcd.fillRoundRect(bX, bY, bW, bH, 5, C_BADGE);
    M5.Lcd.setTextColor(C_OFFWHITE, C_BADGE);
    M5.Lcd.drawCentreString(label, bX + bW / 2, bY + 3, 2);
    const int barX = CARD_X + 15, barY = cy + 44, barW = CARD_W - 30, barH = 12;
    M5.Lcd.fillRoundRect(barX, barY, barW, barH, 4, C_BARBG);
    int filled = max(0, min((int)((float)pct / 100.0f * (barW - 2)), barW - 2));
    if (filled > 0) M5.Lcd.fillRoundRect(barX + 1, barY + 1, filled, barH - 2, 3, C_GREEN);
    M5.Lcd.setTextColor(C_GRAY, C_CARD);
    M5.Lcd.drawString(sub, CARD_X + 15, cy + 63, 1);
}

// ── View 0: Overview ──────────────────────────────────────────────────────────
void drawOverview() {
#ifdef ANTHROPIC_API_KEY
    char s1[40], s2[40];
    snprintf(s1, 40, "%d of %d remaining", data.req_remaining, data.req_limit);
    snprintf(s2, 40, "%s of %s remaining",
             fmtN(data.tok_remaining).c_str(), fmtN(data.tok_limit).c_str());
    drawCard(CARD1_Y, data.req_pct, "Requests", s1);
    drawCard(CARD2_Y, data.tok_pct, "Tokens", s2);
    M5.Lcd.setTextColor(C_AMBER);
    M5.Lcd.drawCentreString(mood(max(data.req_pct, data.tok_pct)), SCR_W / 2, STAT_Y + 4, 2);
#else
    drawCard(CARD1_Y, data.session_pct, "Last 5h",
             ("Resets in " + fmt5h(data.session_reset)).c_str());
    drawCard(CARD2_Y, data.weekly_pct, "This week",
             ("Resets in " + fmt7d(data.weekly_reset)).c_str());
    if (data.spike) {
        M5.Lcd.setTextColor(C_RED);
        char sbuf[32];
        snprintf(sbuf, 32, "!! Sudden jump +%d%% !!", data.delta);
        M5.Lcd.drawCentreString(sbuf, SCR_W / 2, STAT_Y + 4, 2);
    } else {
        M5.Lcd.setTextColor(C_AMBER);
        M5.Lcd.drawCentreString(mood(data.session_pct, data.status), SCR_W / 2, STAT_Y + 4, 2);
    }
    M5.Lcd.setTextColor(C_GRAY);
    M5.Lcd.drawRightString("Proxy", SCR_W - 6, STAT_Y + 12, 1);
#endif
}

// ── View 1: Activity chart ────────────────────────────────────────────────────
void drawActivity() {
    M5.Lcd.setTextColor(C_OFFWHITE);
    M5.Lcd.drawCentreString("Recent Activity", SCR_W / 2, HDR_H + 8, 2);

    if (histCount == 0) {
        M5.Lcd.setTextColor(C_GRAY);
        M5.Lcd.drawCentreString("Waiting for data...", SCR_W / 2, SCR_H / 2, 2);
        return;
    }

    const int cx = 15, cy = HDR_H + 28;
    const int cw = SCR_W - 30, ch = SCR_H - cy - 22;
    const int bw = cw / HIST_SIZE - 2;

    for (int i = 0; i < histCount; i++) {
        int val = histGet(i);
        int bh  = (int)((float)val / 100.0f * ch);
        int bx  = cx + i * (bw + 2);
        int by  = cy + ch - bh;
        M5.Lcd.fillRect(bx, cy, bw, ch, C_BARBG);
        if (bh > 0) {
            uint16_t col = val >= 90 ? C_RED : val >= 60 ? C_AMBER : C_GREEN;
            M5.Lcd.fillRect(bx, by, bw, bh, col);
        }
    }

    M5.Lcd.setTextColor(C_GRAY);
    M5.Lcd.drawString("older", cx, SCR_H - 16, 1);
    M5.Lcd.drawRightString("now", cx + cw, SCR_H - 16, 1);
    char lbl[24];
    int mins = histCount * 30 / 60;
    if (mins < 1) snprintf(lbl, 24, "last %ds",    histCount * 30);
    else          snprintf(lbl, 24, "last ~%d min", mins);
    M5.Lcd.drawCentreString(lbl, SCR_W / 2, SCR_H - 16, 1);
}

// ── View 2: Device info ───────────────────────────────────────────────────────
void drawDevice() {
    int y = HDR_H + 8;
    const int rh = 46, px = CARD_X + 14;

    auto drawRow = [&](const char* title, const char* val, uint16_t col) {
        M5.Lcd.fillRoundRect(CARD_X, y, CARD_W, rh - 4, 6, C_CARD);
        M5.Lcd.setTextColor(C_GRAY, C_CARD);
        M5.Lcd.drawString(title, px, y + 5, 1);
        M5.Lcd.setTextColor(col, C_CARD);
        M5.Lcd.drawString(val, px, y + 20, 2);
        y += rh;
    };

    if (wifiOk) {
        int rssi = WiFi.RSSI();
        const char* sig = rssi > -55 ? "Strong" : rssi > -65 ? "Good" : rssi > -75 ? "Fair" : "Weak";
        char wv[40];
        snprintf(wv, 40, "%s  (%s)", WIFI_SSID, sig);
        drawRow("WiFi", wv, C_GREEN);
    } else {
        drawRow("WiFi", "Not connected", C_RED);
    }

    int bat = M5.Power.getBatteryLevel();
    char bv[24];
    snprintf(bv, 24, "%d%%%s", bat, M5.Power.isCharging() ? "  Charging" : "");
    drawRow("Battery", bv, bat > 20 ? C_GREEN : C_RED);

    char sv[48];
    snprintf(sv, 48, "Every 30s   Last: %s", syncAgo().c_str());
    drawRow("Sync", sv, C_OFFWHITE);

#ifdef ANTHROPIC_API_KEY
    drawRow("Mode", "Direct API", C_AMBER);
#else
    drawRow("Mode", "Proxy", C_AMBER);
#endif
}

// ── Draw: full screen ─────────────────────────────────────────────────────────
void drawAll() {
    M5.Lcd.fillScreen(C_BG);
    drawHeader();

    if (fetching) {
        M5.Lcd.setTextColor(C_GRAY);
        M5.Lcd.drawCentreString("Syncing...", SCR_W / 2, SCR_H / 2, 2);
        return;
    }

    if (!data.ok && curView != 2) {
        M5.Lcd.setTextColor(C_RED);
        M5.Lcd.drawCentreString("No data", SCR_W / 2, 100, 2);
        M5.Lcd.setTextColor(C_GRAY);
        if (!wifiOk) {
            M5.Lcd.drawCentreString("No WiFi connection", SCR_W / 2, 125, 1);
        } else {
            char eb[48];
            snprintf(eb, 48, "HTTP %d", data.httpCode);
            M5.Lcd.drawCentreString(eb, SCR_W / 2, 125, 1);
#ifndef ANTHROPIC_API_KEY
            M5.Lcd.drawCentreString("Is claude_proxy.py running?", SCR_W / 2, 140, 1);
#endif
        }
        return;
    }

    switch (curView) {
        case 0: drawOverview(); break;
        case 1: drawActivity(); break;
        case 2: drawDevice();   break;
    }
}

// ── Setup ─────────────────────────────────────────────────────────────────────
void setup() {
    M5.begin();
    M5.Lcd.setBrightness(200);
    M5.Lcd.fillScreen(C_BG);
    M5.Lcd.setTextColor(C_AMBER);
    M5.Lcd.drawCentreString("Claude Usage", SCR_W / 2, 90, 4);
    M5.Lcd.setTextColor(C_GRAY);
    M5.Lcd.drawCentreString("Connecting to WiFi...", SCR_W / 2, 130, 2);
    connectWiFi();
    if (wifiOk) fetchData();
    drawAll();
}

// ── Loop ──────────────────────────────────────────────────────────────────────
void loop() {
    M5.update();

    if (M5.Touch.getCount() > 0) {
        auto t = M5.Touch.getDetail(0);
        if (t.wasPressed()) {
            int tx = t.x;
            if      (tx < 80  && curView > 0)           { curView--; drawAll(); }
            else if (tx > 240 && curView < NUM_VIEWS - 1){ curView++; drawAll(); }
            else if (tx >= 80 && tx <= 240)              { fetchData(); drawAll(); }
        }
    }

    if (wifiOk && (millis() - lastFetch) > REFRESH_MS) {
        fetchData();
        drawAll();
    }
}
