/*
 * Claude Usage Monitor — M5Stack Core2
 *
 * Two modes, selected by what you define in secrets.h:
 *
 *   ANTHROPIC_API_KEY defined  — calls Anthropic API directly, no proxy needed.
 *                                Shows per-minute request and token rate limits.
 *
 *   PROXY_URL defined          — fetches JSON from claude_proxy.py on your PC.
 *                                Shows 5h and 7d Claude Code subscription usage.
 *
 * Controls:
 *   Touch anywhere — manual refresh
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

// ── Layout (320 x 240 landscape) ─────────────────────────────────────────────
#define SCR_W    320
#define SCR_H    240
#define HDR_H     34
#define CARD_H    85
#define GAP        4
#define CARD1_Y  (HDR_H + GAP)
#define CARD2_Y  (CARD1_Y + CARD_H + GAP)
#define STAT_Y   (CARD2_Y + CARD_H + GAP)
#define CARD_X     5
#define CARD_W   310

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

void drawAll();  // forward declaration

// ── JSON helpers (used in proxy mode) ────────────────────────────────────────
static int jsonInt(const String& s, const char* key) {
    String k = String("\"") + key + "\":";
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
    String k = String("\"") + key + "\":\"";
    int idx = s.indexOf(k);
    if (idx < 0) return "";
    int p = idx + k.length();
    int e = s.indexOf('"', p);
    return (e > p) ? s.substring(p, e) : "";
}

static bool jsonBool(const String& s, const char* key) {
    String k = String("\"") + key + "\":";
    int idx = s.indexOf(k);
    if (idx < 0) return false;
    int p = idx + k.length();
    while (p < (int)s.length() && s[p] == ' ') p++;
    return s.substring(p, p + 4) == "true";
}

// ── Format helpers ────────────────────────────────────────────────────────────
static String fmt5h(int mins) {
    if (mins <= 0) return "now";
    int h = mins / 60, m = mins % 60;
    return h > 0 ? (String(h) + "h " + String(m) + "m") : (String(m) + "m");
}

static String fmt7d(int mins) {
    if (mins <= 0) return "now";
    int d = mins / 1440, h = (mins % 1440) / 60;
    return d > 0 ? (String(d) + "d " + String(h) + "h") : (String(h) + "h");
}

static String fmtCount(long n) {
    if (n >= 1000000L) return String((long)(n / 1000000L)) + "M";
    if (n >= 1000)     return String((long)(n / 1000)) + "K";
    return String((int)n);
}

#ifdef ANTHROPIC_API_KEY
static const char* moodText(int pct) {
    if (pct >= 95) return "* Maxed!";
    if (pct >= 80) return "* Heavy";
    if (pct >= 60) return "* Cooking...";
    if (pct >= 40) return "* Baking...";
    if (pct >= 20) return "* Light";
    if (pct >=  5) return "* Active";
    return "* Idle";
}
#else
static const char* moodText(int pct, const char* st) {
    if (strcmp(st, "limited") == 0) return "* Limited";
    if (pct < 5)  return "* Idle";
    if (pct < 25) return "* Light";
    if (pct < 50) return "* Baking...";
    if (pct < 75) return "* Cooking...";
    if (pct < 90) return "* Heavy";
    return "* Maxed!";
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
    // Direct mode: POST to Anthropic, read rate-limit headers
    WiFiClientSecure wclient;
    wclient.setInsecure();

    HTTPClient http;
    http.begin(wclient, API_URL);
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

        data.req_limit     = rl;
        data.req_remaining = rr;
        data.tok_limit     = tl;
        data.tok_remaining = tr;
        data.req_pct = (rl > 0) ? (int)(100.0f * (rl - rr) / rl) : 0;
        data.tok_pct = (tl > 0) ? (int)(100.0f * (tl - tr) / tl) : 0;
        data.ok = true;
    } else {
        data.ok = false;
    }
    http.end();

#else
    // Proxy mode: GET JSON from claude_proxy.py
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
        data.ok = jsonBool(body, "ok");
    } else {
        data.ok = false;
    }
    http.end();
#endif

    lastFetch = millis();
    fetching  = false;
}

// ── Draw: battery icon ────────────────────────────────────────────────────────
void drawBattery(int x, int y, int bat, bool charging) {
    const int bW = 44, bH = 22;
    const int nW =  5, nH = 10;
    const int bdr = 2;

    uint16_t col = (bat > 20) ? C_GREEN : C_RED;
    M5.Lcd.fillRect(x, y, bW + nW, bH, C_HEADER);
    M5.Lcd.fillRect(x + bW, y + (bH - nH) / 2, nW, nH, col);
    M5.Lcd.drawRoundRect(x, y, bW, bH, 3, col);
    int fillW = max(0, (int)((float)(bW - bdr * 2 - 2) * bat / 100.0f));
    fillW = min(fillW, bW - bdr * 2 - 2);
    if (fillW > 0)
        M5.Lcd.fillRect(x + bdr + 1, y + bdr + 1, fillW, bH - bdr * 2 - 2, col);
    char buf[6];
    snprintf(buf, sizeof(buf), charging ? "+%d" : "%d", bat);
    M5.Lcd.setTextColor(C_WHITE);
    M5.Lcd.drawCentreString(buf, x + bW / 2, y + (bH - 16) / 2, 2);
}

// ── Draw: Clawd mascot ────────────────────────────────────────────────────────
void drawClawd(int x, int y) {
    const int z = 2;
    M5.Lcd.fillRoundRect(x,         y + z * 2, z * 10, z * 6, z, C_SALMON);
    M5.Lcd.fillRect(x + z * 1, y,              z * 2,  z * 3,    C_SALMON);
    M5.Lcd.fillRect(x + z * 7, y,              z * 2,  z * 3,    C_SALMON);
    M5.Lcd.fillRect(x + z * 2, y + z * 3,      z * 2,  z * 2,    C_BG);
    M5.Lcd.fillRect(x + z * 6, y + z * 3,      z * 2,  z * 2,    C_BG);
    M5.Lcd.fillRect(x + z * 2, y + z * 7,      z * 2,  z * 3,    C_SALMON);
    M5.Lcd.fillRect(x + z * 6, y + z * 7,      z * 2,  z * 3,    C_SALMON);
}

// ── Draw: single usage card ───────────────────────────────────────────────────
void drawCard(int cy, int pct, const char* label, const char* subStr) {
    M5.Lcd.fillRoundRect(CARD_X, cy, CARD_W, CARD_H, 8, C_CARD);

    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", pct);
    M5.Lcd.setTextColor(C_WHITE, C_CARD);
    M5.Lcd.drawString(buf, CARD_X + 15, cy + 10, 4);

    const int bW = 80, bH = 22;
    int bX = CARD_X + CARD_W - bW - 12;
    int bY = cy + 10 + 2;
    M5.Lcd.fillRoundRect(bX, bY, bW, bH, 5, C_BADGE);
    M5.Lcd.setTextColor(C_OFFWHITE, C_BADGE);
    M5.Lcd.drawCentreString(label, bX + bW / 2, bY + 3, 2);

    const int barX = CARD_X + 15;
    const int barY = cy + 44;
    const int barW = CARD_W - 30;
    const int barH = 12;
    M5.Lcd.fillRoundRect(barX, barY, barW, barH, 4, C_BARBG);
    int filled = (int)((float)pct / 100.0f * (float)(barW - 2));
    filled = max(0, min(filled, barW - 2));
    if (filled > 0)
        M5.Lcd.fillRoundRect(barX + 1, barY + 1, filled, barH - 2, 3, C_GREEN);

    M5.Lcd.setTextColor(C_GRAY, C_CARD);
    M5.Lcd.drawString(subStr, CARD_X + 15, cy + 63, 1);
}

// ── Draw: full screen ─────────────────────────────────────────────────────────
void drawAll() {
    M5.Lcd.fillScreen(C_BG);

    M5.Lcd.fillRect(0, 0, SCR_W, HDR_H, C_HEADER);
    drawClawd(7, 7);
    M5.Lcd.setTextColor(C_WHITE, C_HEADER);
    M5.Lcd.drawString("Usage", 34, 5, 4);

    int bat = M5.Power.getBatteryLevel();
    drawBattery(SCR_W - 44 - 5 - 6, (HDR_H - 22) / 2, bat, M5.Power.isCharging());

    if (fetching) {
        M5.Lcd.setTextColor(C_GRAY);
        M5.Lcd.drawCentreString("Fetching...", SCR_W / 2, SCR_H / 2, 2);
        return;
    }

    if (!data.ok) {
        M5.Lcd.setTextColor(C_RED);
        M5.Lcd.drawCentreString("No data", SCR_W / 2, 100, 2);
        M5.Lcd.setTextColor(C_GRAY);
        if (!wifiOk) {
            M5.Lcd.drawCentreString("No WiFi connection", SCR_W / 2, 125, 1);
        } else {
            char ebuf[48];
            snprintf(ebuf, sizeof(ebuf), "HTTP %d", data.httpCode);
            M5.Lcd.drawCentreString(ebuf, SCR_W / 2, 125, 1);
#ifndef ANTHROPIC_API_KEY
            M5.Lcd.drawCentreString("Is claude_proxy.py running?", SCR_W / 2, 140, 1);
#endif
        }
        return;
    }

#ifdef ANTHROPIC_API_KEY
    char reqSub[40];
    snprintf(reqSub, sizeof(reqSub), "%d of %d remaining",
             data.req_remaining, data.req_limit);
    drawCard(CARD1_Y, data.req_pct, "Requests", reqSub);

    char tokSub[40];
    snprintf(tokSub, sizeof(tokSub), "%s of %s remaining",
             fmtCount(data.tok_remaining).c_str(),
             fmtCount(data.tok_limit).c_str());
    drawCard(CARD2_Y, data.tok_pct, "Tokens", tokSub);

    M5.Lcd.setTextColor(C_AMBER);
    M5.Lcd.drawCentreString(moodText(max(data.req_pct, data.tok_pct)),
                             SCR_W / 2, STAT_Y + 4, 2);
#else
    drawCard(CARD1_Y, data.session_pct, "Current",
             fmt5h(data.session_reset).c_str());

    drawCard(CARD2_Y, data.weekly_pct, "Weekly",
             fmt7d(data.weekly_reset).c_str());

    if (data.spike) {
        M5.Lcd.setTextColor(C_RED);
        char sbuf[32];
        snprintf(sbuf, sizeof(sbuf), "!! Rapid burn +%d%% !!", data.delta);
        M5.Lcd.drawCentreString(sbuf, SCR_W / 2, STAT_Y + 4, 2);
    } else {
        M5.Lcd.setTextColor(C_AMBER);
        M5.Lcd.drawCentreString(moodText(data.session_pct, data.status),
                                 SCR_W / 2, STAT_Y + 4, 2);
    }
#endif
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
            fetchData();
            drawAll();
        }
    }

    if (wifiOk && (millis() - lastFetch) > REFRESH_MS) {
        fetchData();
        drawAll();
    }
}
