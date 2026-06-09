/**
 * CYD NTP Clock — ESP32-2432S028 (ILI9341, 320×240)
 *
 * SETUP
 *   • First boot: calibration screen runs automatically, then WiFi config portal.
 *   • Hold BOOT button (GPIO 0) at power-on   → force config portal.
 *   • Hold BOOT button 3 s during normal run   → config portal.
 *   Portal SSID: CYD-Clock   Password: esp32clock
 *   Browse to 192.168.4.1 to set WiFi, NTP server, POSIX timezone, brightness.
 *
 * TOUCH CONTROLS
 *   • Tap / hold left  third  → decrease brightness (−20 per 300 ms, min 10)
 *   • Tap / hold right third  → increase brightness (+20 per 300 ms, max 255)
 *   • Hold centre zone  5 s   → re-run touch calibration (orange countdown bar)
 *
 * POSIX timezone examples
 *   UTC          UTC0
 *   US Eastern   EST5EDT,M3.2.0,M11.1.0
 *   US Pacific   PST8PDT,M3.2.0,M11.1.0
 *   UK           GMT0BST,M3.5.0/1,M10.5.0
 *   CET+DST      CET-1CEST,M3.5.0,M10.5.0/3
 *   Bangkok      ICT-7
 *   Tokyo        JST-9
 */

#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <SPI.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <sys/time.h>
#include <math.h>
#include <esp_wifi.h>
#include <esp_bt.h>

// ── Schedule ────────────────────────────────────────────────────────
struct Schedule {
    uint8_t wkH,  wkM;    // play time start    (default 20:00)
    uint8_t bedH, bedM;   // story time start   (default 21:00)
    uint8_t slpH, slpM;   // lights-out / sleep (default 22:00)
};struct SchedMsg { const char* text; uint16_t color; };
// ── Touch SPI (VSPI — separate bus from display) ──────────────────
#define TOUCH_CS   33
#define TOUCH_CLK  25
#define TOUCH_MISO 39
#define TOUCH_MOSI 32
SPIClass        touchSPI(VSPI);
XPT2046_Touchscreen ts(TOUCH_CS, /*irq=*/255);

// ── Hardware ──────────────────────────────────────────────────────
#define BOOT_BTN  0
#define BL_PIN    21
#define BL_CHAN   0
// TOUCH_CS=33, TOUCH_MOSI=32, TOUCH_MISO=39, TOUCH_CLK=25 → via build flags

// ── Portal AP ────────────────────────────────────────────────────
#define AP_SSID  "CYD-Clock"
#define AP_PASS  "esp32clock"

// ── Timing ───────────────────────────────────────────────────────
#define NTP_RESYNC_MS  21600000UL  // re-sync NTP every 6 hours
#define LOOP_MS        33          // main loop tick (~30 fps)
#define FF_TICK        (LOOP_MS / 100.0f)  // scale factor for per-tick animation values
#define TIME_KERN      6            // extra px between each time character

// ── Palette ──────────────────────────────────────────────────────
#define C_BG    TFT_BLACK
#define C_TIME  0x07FF   // cyan
#define C_DOW   0xFFE0   // yellow
#define C_DATE  TFT_WHITE
#define C_BAR   0x07E0   // green (used in portal/calibration text)
#define C_BARG  0x2104   // dark grey
#define C_GREY  0x7BEF   // medium grey

// ── Rainbow palette (used by animation) ──────────────────────────
static const uint16_t kRainbow[7] = {
    0xF800, // red
    0xFD20, // orange
    0xFFE0, // yellow
    0x07E0, // green
    0x07FF, // cyan
    0x001F, // blue
    0xF81F, // magenta
};

// ── Animation zone ────────────────────────────────────────────────
#define ANIM_Y   215   // top y of animation strip
#define ANIM_H    25   // height (215-239)
#define BALL_R     9   // smiley ball radius

// ── Layout (landscape 320×240) ────────────────────────────────────
// Row centres (MC_DATUM):
//   DOW   y=18   (Font4, 26 px)
//   TIME  y=120  (Helvetica Neue Bold 90pt smooth AA — HH:MM, solid white)
//   DATE  y=202  (Font4, 26 px)
//   ANIM  y=215-239 (bouncing smiley + rainbow trail — shows seconds)

// ── Globals ──────────────────────────────────────────────────────
TFT_eSPI       tft;
TFT_eSprite    g_animSpr(&tft);   // off-screen buffer for the animation strip
TFT_eSprite    g_timeSpr(&tft);   // off-screen buffer for the time band (rolling)
Preferences prefs;

char    g_ntp[64];
char    g_tz[64];
uint8_t g_bl;

uint32_t  g_lastSync = 0;
struct tm g_prev     = {};
bool      g_fresh    = true;

// ── Touch ─────────────────────────────────────────────────────────
struct TouchCal { int16_t xMin, xMax, yMin, yMax; };
TouchCal g_tcal      = {};
bool     g_calValid  = false;
bool     g_touchAct    = false;   // finger is currently down
uint32_t g_touchStart  = 0;       // millis when current touch began
uint32_t g_lastBLTap   = 0;       // millis of last brightness step
uint32_t g_blTouchAt   = 0;       // millis of last touch interaction
uint32_t g_blShowUntil = 0;       // millis when brightness overlay expires
uint32_t g_blSaveAt    = 0;       // millis when to write brightness to prefs
bool     g_blOverlay   = false;   // DOW row is showing brightness overlay
const uint32_t BL_OVERLAY_MS = 5000; // keep overlay visible for 5 s of inactivity

Schedule g_sched;

// ── Time roll animation state ─────────────────────────────────────
bool     g_rolling   = false;
uint32_t g_rollStart = 0;
char     g_rollOld[8] = {};
char     g_rollNew[8] = {};
int      g_timeY     = 0;   // top y of the time band on screen
int      g_timeCharH = 0;   // pixel height of one row of time text
int      g_timeTrimT = 0;   // blank px at top of sprite above actual digit pixels
int      g_timeTrimB = 0;   // blank px at bottom of sprite below actual digit pixels
const char* g_dowText = nullptr;  // last text drawn in DOW row (ptr into literal)
uint16_t    g_dowColor = 0;       // last color drawn in DOW row

// ─────────────────────────────────────────────────────────────────
// Preferences
// ─────────────────────────────────────────────────────────────────
void prefsLoad() {
    prefs.begin("clk", true);
    String ntp    = prefs.getString("ntp", "pool.ntp.org");
    String tz     = prefs.getString("tz",  "UTC0");
    g_bl          = (uint8_t)prefs.getUInt("bl",    200);
    g_sched.wkH   = (uint8_t)prefs.getUInt("wkH",    20);
    g_sched.wkM   = (uint8_t)prefs.getUInt("wkM",     0);
    g_sched.bedH  = (uint8_t)prefs.getUInt("bedH",   21);
    g_sched.bedM  = (uint8_t)prefs.getUInt("bedM",    0);
    g_sched.slpH  = (uint8_t)prefs.getUInt("slpH",   22);
    g_sched.slpM  = (uint8_t)prefs.getUInt("slpM",    0);
    prefs.end();
    strncpy(g_ntp, ntp.c_str(), sizeof(g_ntp) - 1);
    strncpy(g_tz,  tz.c_str(),  sizeof(g_tz)  - 1);
}

void prefsSave() {
    prefs.begin("clk", false);
    prefs.putString("ntp",  g_ntp);
    prefs.putString("tz",   g_tz);
    prefs.putUInt("bl",     g_bl);
    prefs.putUInt("bedH",   g_sched.bedH);
    prefs.putUInt("bedM",   g_sched.bedM);
    prefs.putUInt("slpH",   g_sched.slpH);
    prefs.putUInt("slpM",   g_sched.slpM);
    prefs.putUInt("wkH",    g_sched.wkH);
    prefs.putUInt("wkM",    g_sched.wkM);
    prefs.end();
}

// ─────────────────────────────────────────────────────────────────
// Schedule helpers
// ─────────────────────────────────────────────────────────────────
void parseHHMM(const char* s, uint8_t &h, uint8_t &m) {
    int hh = 0, mm = 0;
    if (sscanf(s, "%d:%d", &hh, &mm) == 2) {
        h = (uint8_t)constrain(hh, 0, 23);
        m = (uint8_t)constrain(mm, 0, 59);
    }
}

static inline int toMin(uint8_t h, uint8_t m) { return h * 60 + m; }

SchedMsg getSchedMsg(const struct tm* t) {
    int now  = t->tm_hour * 60 + t->tm_min;
    int play = toMin(g_sched.wkH,  g_sched.wkM);
    int story = toMin(g_sched.bedH, g_sched.bedM);
    int sleep = toMin(g_sched.slpH, g_sched.slpM);

    if (now >= play && now < story)
        return { "Fun Time, Minmin!", 0xFFE0 };       // yellow
    if (now >= story && now < sleep)
        return { "Story Time, Minmin! <3", 0xF81F }; // pink
    if (now >= sleep)
        return { "Good night, Minmin!", 0x07FF };     // cyan
    return { NULL, 0 };
}

// ─────────────────────────────────────────────────────────────────
// Touch calibration
// ─────────────────────────────────────────────────────────────────
void touchCalLoad() {
    prefs.begin("clk", true);
    size_t n = prefs.getBytes("tcal", &g_tcal, sizeof(g_tcal));
    prefs.end();
    g_calValid = (n == sizeof(g_tcal));
}

void touchCalSave() {
    prefs.begin("clk", false);
    prefs.putBytes("tcal", &g_tcal, sizeof(g_tcal));
    prefs.end();
}

// Map a raw XPT2046 reading to a screen coordinate using stored calibration.
bool getTouchXY(uint16_t &sx, uint16_t &sy) {
    if (!ts.touched()) return false;
    TS_Point p = ts.getPoint();
    if (p.z < 100) return false;   // too light — ignore noise
    sx = (uint16_t)constrain(map(p.x, g_tcal.xMin, g_tcal.xMax, 0, 319), 0, 319);
    sy = (uint16_t)constrain(map(p.y, g_tcal.yMin, g_tcal.yMax, 0, 239), 0, 239);
    return true;
}

// Draw a small cross-hair target at (x, y).
static void drawTarget(int x, int y) {
    tft.drawLine(x - 12, y, x + 12, y, TFT_WHITE);
    tft.drawLine(x, y - 12, x, y + 12, TFT_WHITE);
    tft.drawCircle(x, y, 6, TFT_WHITE);
}

// Blocking wait for one tap; returns raw XPT2046 point.
static TS_Point waitTap() {
    while (ts.touched()) delay(10);          // wait for lift-off first
    while (!ts.touched()) delay(10);         // wait for press
    delay(40);                               // settle
    TS_Point p = ts.getPoint();
    while (ts.touched()) delay(10);          // wait for release
    return p;
}

void runCalibration() {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4); tft.setTextColor(C_DOW, C_BG);
    tft.drawString("Touch Calibration", 160, 50);
    tft.setTextFont(2); tft.setTextColor(C_DATE, C_BG);
    tft.drawString("Tap each cross-hair with your stylus.", 160, 100);
    delay(1800);

    // Point 1 — top-left
    tft.fillScreen(C_BG);
    drawTarget(20, 20);
    tft.setTextFont(2); tft.setTextColor(C_GREY, C_BG); tft.setTextDatum(MC_DATUM);
    tft.drawString("Tap the cross-hair", 160, 130);
    TS_Point p1 = waitTap();

    // Point 2 — bottom-right
    tft.fillScreen(C_BG);
    drawTarget(300, 220);
    tft.setTextFont(2); tft.setTextColor(C_GREY, C_BG); tft.setTextDatum(MC_DATUM);
    tft.drawString("Tap the cross-hair", 160, 110);
    TS_Point p2 = waitTap();

    // Extrapolate raw values to full screen edges.
    // map() handles inverted axes automatically (xMin may be > xMax).
    g_tcal.xMin = (int16_t)map(0,   20, 300, p1.x, p2.x);
    g_tcal.xMax = (int16_t)map(319, 20, 300, p1.x, p2.x);
    g_tcal.yMin = (int16_t)map(0,   20, 220, p1.y, p2.y);
    g_tcal.yMax = (int16_t)map(239, 20, 220, p1.y, p2.y);
    g_calValid = true;
    touchCalSave();

    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4); tft.setTextColor(C_BAR, C_BG);
    tft.drawString("Calibration saved!", 160, 120);
    delay(1200);
}

// Draws a brightness indicator in the DOW row (y 0-35).
// Stays visible for 5 s of inactivity; drawClock skips that row while active.
void drawBlOverlay() {
    char buf[28];
    snprintf(buf, sizeof buf, "< dim   %d%%   brighten >", g_bl * 100 / 255);
    tft.fillRect(0, 0, 320, 36, C_BG);
    tft.setTextFont(2);
    tft.setTextDatum(MC_DATUM);
    tft.setTextColor(TFT_YELLOW, C_BG);
    tft.drawString(buf, 160, 18);
    g_blOverlay   = true;
    g_blTouchAt   = millis();
    g_blShowUntil = millis() + BL_OVERLAY_MS;
}

// ─────────────────────────────────────────────────────────────────
// Backlight (PWM)
// ─────────────────────────────────────────────────────────────────
void blInit() {
    ledcSetup(BL_CHAN, 5000, 8);
    ledcAttachPin(BL_PIN, BL_CHAN);
}
void blSet(uint8_t v) { ledcWrite(BL_CHAN, v); }

// ─────────────────────────────────────────────────────────────────
// NTP
// ─────────────────────────────────────────────────────────────────
void ntpSync() {
    configTzTime(g_tz, g_ntp);
    struct tm tmp;
    for (int i = 0; i < 20 && !getLocalTime(&tmp, 500); i++);
    g_lastSync = millis();
}

// ─────────────────────────────────────────────────────────────────
// Splash screens
// ─────────────────────────────────────────────────────────────────
void scrPortal() {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4); tft.setTextColor(C_DOW,  C_BG);
    tft.drawString("WiFi Setup", 160, 40);
    tft.setTextFont(2); tft.setTextColor(C_DATE, C_BG);
    tft.drawString("Join this network on your phone:", 160, 88);
    tft.setTextFont(4); tft.setTextColor(C_BAR,  C_BG);
    tft.drawString(AP_SSID, 160, 118);
    tft.setTextFont(2); tft.setTextColor(C_DATE, C_BG);
    tft.drawString("Password:  " AP_PASS, 160, 156);
    tft.drawString("Then open  192.168.4.1", 160, 178);
    tft.setTextColor(C_GREY, C_BG);
    tft.drawString("Portal closes automatically after 3 min", 160, 215);
}

void scrConnecting() {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4); tft.setTextColor(C_DATE, C_BG);
    tft.drawString("Connecting to WiFi...", 160, 100);
    tft.setTextFont(2); tft.setTextColor(C_GREY, C_BG);
    tft.drawString("Hold BOOT 3 s to enter setup", 160, 160);
}

void scrSyncing() {
    tft.fillScreen(C_BG);
    tft.setTextDatum(MC_DATUM);
    tft.setTextFont(4); tft.setTextColor(C_DATE, C_BG);
    tft.drawString("Syncing time...", 160, 120);
}

// ─────────────────────────────────────────────────────────────────
// Clock face
// ─────────────────────────────────────────────────────────────────
static const char* kDOW[7]  = { "Sunday","Monday","Tuesday","Wednesday",
                                  "Thursday","Friday","Saturday" };
static const char* kMON[12] = { "Jan","Feb","Mar","Apr","May","Jun",
                                  "Jul","Aug","Sep","Oct","Nov","Dec" };

// ── Firefly background animation ───────────────────────────────────────────────
// Fireflies drift through the two black gaps above and below the time digits.
// Each one breathes in/out via a slow sine wave and wanders with organic noise.
#define FF_COUNT 80
struct Firefly {
    float   x, y;
    float   vx, vy;
    float   phase;       // 0..2π — drives glow brightness
    float   phaseRate;   // radians per frame
    float   ox, oy;      // previous position (for erasing)
    bool    drawn;       // true if a pixel lives at (ox,oy)
    uint8_t col;         // index into kRainbow
    int16_t yMin, yMax;  // vertical band this firefly is confined to
};
static Firefly g_ff[FF_COUNT];
static bool    g_ffInit = false;

static float ffRand(uint32_t &s) {
    s = s * 1664525UL + 1013904223UL;
    return (float)(s & 0x7FFF) / 32768.0f;
}

// Dim an RGB565 colour: shift each channel right by 'sh' bits.
static inline uint16_t dimCol(uint16_t c, uint8_t sh) {
    return (uint16_t)((((c >> 11) & 0x1F) >> sh) << 11)
         | (uint16_t)((((c >>  5) & 0x3F) >> sh) <<  5)
         | (uint16_t)( ((c        & 0x1F) >> sh));
}

void initFireflies() {
    // Safe zones: black gaps that drawClock never overwrites
    //   Zone 0 (top)    — between DOW row (clears 0..35) and TIME sprite
    //   Zone 1 (bottom) — between TIME sprite bottom and DATE clearRect (y=185)
    //   IMPORTANT: both zones must be entirely outside the digit *pixels* inside
    //   the sprite. The sprite's blank top/bottom margins (g_timeTrimT/B) are
    //   safe because they contain only C_BG and firefly erases don't damage them.
    // Keep the fireflies closer to the time band and bias the lower band a bit
    // so it doesn't look sparse compared with the top gap.
    int16_t z0min = 37;
    int16_t z0max = min((int16_t)(g_timeY + g_timeTrimT - 2), (int16_t)(g_timeY + 12));
    int16_t z1min = max((int16_t)(g_timeY + g_timeCharH - g_timeTrimB - 12),
                        (int16_t)(g_timeY + g_timeCharH - 12));
    int16_t z1max = 184;
    if (z0max < z0min + 2) z0max = z0min + 2;
    if (z1min > z1max - 2) z1min = z1max - 2;

    const int topCount = 28;

    uint32_t seed = 0xCAFEBABEUL;
    for (int i = 0; i < FF_COUNT; i++) {
        Firefly &f  = g_ff[i];
        bool    top = (i < topCount);
        f.yMin      = top ? z0min : z1min;
        f.yMax      = top ? z0max : z1max;
        f.x         = ffRand(seed) * 316.0f + 2.0f;
        f.y         = f.yMin + ffRand(seed) * (f.yMax - f.yMin);
        f.vx        = (ffRand(seed) - 0.5f) * 4.0f * FF_TICK;
        f.vy        = (ffRand(seed) - 0.5f) * 1.4f * FF_TICK;
        f.phase     = ffRand(seed) * 6.2832f;
        f.phaseRate = (0.03f + ffRand(seed) * 0.05f) * FF_TICK;  // period ~7–21 s
        f.col       = (uint8_t)((int)(ffRand(seed) * 6.99f));
        f.drawn     = false;
        f.ox = f.x;  f.oy = f.y;
    }
    g_ffInit = true;
}

void drawFireflies() {
    if (!g_ffInit) initFireflies();
    uint32_t ms = millis();
    for (int i = 0; i < FF_COUNT; i++) {
        Firefly &f = g_ff[i];

        // Erase previous glow (these zones are always black, safe to overwrite)
        if (f.drawn) {
            tft.fillCircle((int)f.ox, (int)f.oy, 2, C_BG);
            f.drawn = false;
        }

        // Organic drift — slowly perturb velocity with slow sine noise
        f.vx += sinf(ms * 0.00097f + i * 1.618f) * 0.13f * FF_TICK;
        f.vy += sinf(ms * 0.00113f + i * 2.718f) * 0.07f * FF_TICK;
        f.vx = constrain(f.vx, -4.0f * FF_TICK, 4.0f * FF_TICK);
        f.vy = constrain(f.vy, -1.4f * FF_TICK, 1.4f * FF_TICK);
        f.x += f.vx;
        f.y += f.vy;

        // Wrap x so fireflies drift off one side and reappear on the other
        if (f.x < 2.0f)   f.x += 316.0f;
        if (f.x > 318.0f) f.x -= 316.0f;
        // Soft bounce at zone top/bottom edges
        if (f.y < f.yMin) { f.y = (float)f.yMin; f.vy =  fabsf(f.vy) + 0.05f * FF_TICK; }
        if (f.y > f.yMax) { f.y = (float)f.yMax; f.vy = -(fabsf(f.vy) + 0.05f * FF_TICK); }

        // Breathing glow: firefly is dark for ~half its cycle
        f.phase += f.phaseRate;
        if (f.phase > 6.2832f) f.phase -= 6.2832f;
        float bri = sinf(f.phase);
        if (bri <= 0.05f) continue;  // dark phase — skip drawing

        uint16_t base = kRainbow[f.col];
        uint16_t col;
        int      r;
        if      (bri < 0.35f) { col = dimCol(base, 2); r = 1; }  // faint 1 px
        else if (bri < 0.70f) { col = dimCol(base, 1); r = 1; }  // medium 1 px
        else                  { col = base;             r = 2; }  // full glow 2 px

        tft.fillCircle((int)f.x, (int)f.y, r, col);
        f.ox = f.x;  f.oy = f.y;
        f.drawn = true;
    }
}

// Bouncing rainbow smiley ball traversing left→right over 60 s.
void drawAnim(int sec, float sub) {
    float p     = constrain((sec + sub) / 60.0f, 0.0f, 1.0f);
    int   xMin  = BALL_R + 4;
    int   xMax  = 319 - BALL_R - 4;
    int   ballX = xMin + (int)(p * (xMax - xMin));
    // ballY is relative to the sprite (0 = top of ANIM zone)
    int   ballY = ANIM_H / 2
                  + (int)(3.5f * sinf((sec + sub) * 0.31416f));

    // Draw everything into off-screen sprite, then push — eliminates flicker
    g_animSpr.fillSprite(C_BG);

    // Rainbow dotted trail behind the ball
    for (int x = xMin; x < ballX - BALL_R - 2; x += 9) {
        uint16_t col = kRainbow[((x - xMin) / 9) % 7];
        int ty = ANIM_H / 2
                 + (int)(3.5f * sinf(((x - xMin) / (float)(xMax - xMin))
                                     * 60.0f * 0.31416f));
        g_animSpr.fillCircle(x, ty, 3, col);
    }

    // Ball body — colored with current rainbow hue
    uint16_t ballCol = kRainbow[(int)(p * 6.99f) % 7];
    g_animSpr.fillCircle(ballX, ballY, BALL_R, ballCol);
    // Highlight
    g_animSpr.fillCircle(ballX - 3, ballY - 3, 2, TFT_WHITE);
    // Eyes
    g_animSpr.fillCircle(ballX - 3, ballY - 2, 1, TFT_BLACK);
    g_animSpr.fillCircle(ballX + 3, ballY - 2, 1, TFT_BLACK);
    // Smile (3 dots)
    g_animSpr.drawPixel(ballX - 3, ballY + 3, TFT_BLACK);
    g_animSpr.drawPixel(ballX,     ballY + 4, TFT_BLACK);
    g_animSpr.drawPixel(ballX + 3, ballY + 3, TFT_BLACK);

    g_animSpr.pushSprite(0, ANIM_Y);
    drawFireflies();
}

void drawClock(const struct tm* t, float sub, bool force) {
    char buf[32];

    // Day of week / schedule message (skipped while brightness overlay is visible)
    // Only redraw when the displayed text actually changes — avoids a flash every minute.
    if (!g_blOverlay) {
        SchedMsg sm = getSchedMsg(t);
        const char* newText  = sm.text ? sm.text : kDOW[t->tm_wday];
        uint16_t    newColor = sm.text ? sm.color : (uint16_t)C_DOW;
        if (force || newText != g_dowText || newColor != g_dowColor) {
            tft.fillRect(0, 0, 320, 36, C_BG);
            tft.loadFont("ui", LittleFS);
            tft.setTextDatum(MC_DATUM);
            tft.setTextColor(newColor, C_BG);
            tft.drawString(newText, 160, 18);
            tft.unloadFont();
            g_dowText  = newText;
            g_dowColor = newColor;
        }
    }

    // Time  HH:MM  — rolls upward on minute change (sprite blit, no flicker)
    if (force || t->tm_min != g_prev.tm_min) {
        snprintf(g_rollNew, sizeof g_rollNew, "%02d:%02d", t->tm_hour, t->tm_min);
        if (!force) {
            // Start roll: save old string and kick off animation
            snprintf(g_rollOld, sizeof g_rollOld, "%02d:%02d",
                     g_prev.tm_hour, g_prev.tm_min);
            g_rollStart = millis();
            g_rolling   = true;
        } else {
            // Force / first draw — show immediately with no animation
            g_rolling = false;
            g_timeSpr.fillSprite(C_BG);
            g_timeSpr.setTextDatum(TL_DATUM);
            g_timeSpr.setTextColor(TFT_WHITE, C_BG);
            // Compute total width with extra kerning
            int totalW = 0;
            for (int i = 0; g_rollNew[i]; i++) {
                char c[2] = { g_rollNew[i], '\0' };
                totalW += g_timeSpr.textWidth(c);
            }
            totalW += (strlen(g_rollNew) - 1) * TIME_KERN;
            int x = 160 - totalW / 2;
            for (int i = 0; g_rollNew[i]; i++) {
                char c[2] = { g_rollNew[i], '\0' };
                g_timeSpr.drawString(c, x, 0);
                x += g_timeSpr.textWidth(c) + TIME_KERN;
            }
            g_timeSpr.pushSprite(0, g_timeY);
        }
    }
    if (g_rolling) {
        float progress = min((float)(millis() - g_rollStart) / 450.0f, 1.0f);
        // Ease-out quad: fast start, gentle landing
        float ease = 1.0f - (1.0f - progress) * (1.0f - progress);
        int   oy   = -(int)(ease * g_timeCharH);   // old char scrolls off top
        int   ny   = g_timeCharH + oy;             // new char rises from below

        g_timeSpr.fillSprite(C_BG);
        g_timeSpr.setTextDatum(TL_DATUM);
        g_timeSpr.setTextColor(TFT_WHITE, C_BG);

        // Center the string then iterate character-by-character
        int totalW = 0;
        for (int i = 0; g_rollNew[i]; i++) {
            char c[2] = { g_rollNew[i], '\0' };
            totalW += g_timeSpr.textWidth(c);
        }
        totalW += (strlen(g_rollNew) - 1) * TIME_KERN;
        int x = 160 - totalW / 2;
        for (int i = 0; g_rollNew[i]; i++) {
            char nc[2] = { g_rollNew[i], '\0' };
            int  cw    = g_timeSpr.textWidth(nc);
            if (g_rollOld[i] != g_rollNew[i]) {
                // This digit changed — roll it independently
                char oc[2] = { g_rollOld[i], '\0' };
                g_timeSpr.drawString(oc, x, oy);
                g_timeSpr.drawString(nc, x, ny);
            } else {
                // Unchanged digit — draw stationary at centre
                g_timeSpr.drawString(nc, x, 0);
            }
            x += cw + TIME_KERN;
        }

        g_timeSpr.pushSprite(0, g_timeY);

        if (progress >= 1.0f) g_rolling = false;
    }

    // Date
    if (force || t->tm_mday != g_prev.tm_mday || t->tm_mon != g_prev.tm_mon) {
        tft.fillRect(0, 185, 320, 30, C_BG);   // stop at ANIM_Y
        tft.loadFont("ui", LittleFS);
        tft.setTextDatum(MC_DATUM);
        tft.setTextColor(C_DATE, C_BG);
        snprintf(buf, sizeof buf, "%s %d, %d",
                 kMON[t->tm_mon], t->tm_mday, 1900 + t->tm_year);
        tft.drawString(buf, 160, 199);
        tft.unloadFont();
    }

    // Animation — updated every tick
    drawAnim(t->tm_sec, sub);

    g_prev = *t;
}

// ─────────────────────────────────────────────────────────────────
// Config portal  (called from loop too)
// ─────────────────────────────────────────────────────────────────
void runPortal() {
    WiFiManager wm;
    char blBuf[5];
    snprintf(blBuf, sizeof blBuf, "%d", g_bl);
    char bedBuf[6], slpBuf[6], wkBuf[6];
    snprintf(bedBuf, sizeof bedBuf, "%02d:%02d", g_sched.bedH, g_sched.bedM);
    snprintf(slpBuf, sizeof slpBuf, "%02d:%02d", g_sched.slpH, g_sched.slpM);
    snprintf(wkBuf,  sizeof wkBuf,  "%02d:%02d", g_sched.wkH,  g_sched.wkM);

    WiFiManagerParameter pNtp("ntp", "NTP Server", g_ntp, 63);
    WiFiManagerParameter pTz ("tz",
        "Timezone (POSIX — e.g. UTC0 | ICT-7 | JST-9 | EST5EDT,M3.2.0,M11.1.0)",
        g_tz, 63);
    WiFiManagerParameter pBl ("bl",  "Brightness (0-255)", blBuf, 4);
    WiFiManagerParameter pWk ("wk",  "Minmin play time   (HH:MM)", wkBuf,  5);
    WiFiManagerParameter pBed("bed", "Minmin story time  (HH:MM)", bedBuf, 5);
    WiFiManagerParameter pSlp("slp", "Minmin lights out  (HH:MM)", slpBuf, 5);

    wm.addParameter(&pNtp);
    wm.addParameter(&pTz);
    wm.addParameter(&pBl);
    wm.addParameter(&pBed);
    wm.addParameter(&pSlp);
    wm.addParameter(&pWk);
    wm.setConfigPortalTimeout(180);
    wm.setTitle("CYD Clock");
    wm.setAPCallback([](WiFiManager*) { scrPortal(); });

    scrPortal();
    wm.startConfigPortal(AP_SSID, AP_PASS);

    strncpy(g_ntp, pNtp.getValue(), sizeof(g_ntp) - 1);
    strncpy(g_tz,  pTz.getValue(),  sizeof(g_tz)  - 1);
    g_bl = (uint8_t)constrain(atoi(pBl.getValue()), 0, 255);
    parseHHMM(pBed.getValue(), g_sched.bedH, g_sched.bedM);
    parseHHMM(pSlp.getValue(), g_sched.slpH, g_sched.slpM);
    parseHHMM(pWk.getValue(),  g_sched.wkH,  g_sched.wkM);
    prefsSave();
    blSet(g_bl);

    scrSyncing();
    ntpSync();
    tft.fillScreen(C_BG);
    g_fresh = true;
}

// ─────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);

    // Power saving: disable Bluetooth (never needed) and run CPU at 80 MHz
    btStop();
    esp_bt_controller_disable();
    setCpuFrequencyMhz(80);

    pinMode(BOOT_BTN, INPUT_PULLUP);

    tft.init();
    tft.setRotation(1);
    tft.fillScreen(C_BG);

    // Init backlight AFTER tft.init() so LEDC is the last to own pin 21.
    // (TFT_eSPI would call digitalWrite on TFT_BL and detach LEDC if we init earlier.)
    blInit();
    blSet(120);                        // dim while connecting

    g_animSpr.createSprite(320, ANIM_H);  // allocate animation buffer once

    // Load anti-aliased smooth font and measure height to size the time sprite.
    // Requires data/clock.vlw uploaded via: pio run --target uploadfs
    LittleFS.begin();
    g_timeSpr.loadFont("clock", LittleFS);
    g_timeCharH = g_timeSpr.fontHeight();  // = ascent + descent from VLW
    // gFont.maxAscent starts at gFont.ascent and only increases — it never
    // reflects how far below the ascent line the tallest glyph actually starts.
    // Scan glyph arrays directly: topmost pixel = ascent - max(gdY).
    {
        int maxGdY = 0, maxBot = 0;
        for (int i = 0; i < (int)g_timeSpr.gFont.gCount; i++) {
            if (g_timeSpr.gHeight[i] == 0) continue;  // space / empty glyph
            if ((int)g_timeSpr.gdY[i] > maxGdY) maxGdY = g_timeSpr.gdY[i];
            int bot = (int)g_timeSpr.gFont.ascent - g_timeSpr.gdY[i]
                      + g_timeSpr.gHeight[i];
            if (bot > maxBot) maxBot = bot;
        }
        g_timeTrimT = (int)g_timeSpr.gFont.ascent - maxGdY;
        g_timeTrimB = g_timeCharH - maxBot;
        if (g_timeTrimT < 0) g_timeTrimT = 0;
        if (g_timeTrimB < 0) g_timeTrimB = 0;
    }
    g_timeY     = 120 - g_timeCharH / 2;
    g_timeSpr.createSprite(320, g_timeCharH);  // allocate once

    // Touch controller on its own VSPI bus
    touchSPI.begin(TOUCH_CLK, TOUCH_MISO, TOUCH_MOSI, TOUCH_CS);
    ts.begin(touchSPI);
    ts.setRotation(1);

    prefsLoad();
    blSet(g_bl);

    // WiFi + initial setup portal
    {
        WiFiManager wm;
        char blBuf[5];
        snprintf(blBuf, sizeof blBuf, "%d", g_bl);
        char bedBuf[6], slpBuf[6], wkBuf[6];
        snprintf(bedBuf, sizeof bedBuf, "%02d:%02d", g_sched.bedH, g_sched.bedM);
        snprintf(slpBuf, sizeof slpBuf, "%02d:%02d", g_sched.slpH, g_sched.slpM);
        snprintf(wkBuf,  sizeof wkBuf,  "%02d:%02d", g_sched.wkH,  g_sched.wkM);

        WiFiManagerParameter pNtp("ntp", "NTP Server", g_ntp, 63);
        WiFiManagerParameter pTz ("tz",
            "Timezone (POSIX — e.g. UTC0 | ICT-7 | EST5EDT,M3.2.0,M11.1.0)",
            g_tz, 63);
        WiFiManagerParameter pBl ("bl",  "Brightness (0-255)", blBuf, 4);
        WiFiManagerParameter pWk ("wk",  "Minmin play time   (HH:MM)", wkBuf,  5);
        WiFiManagerParameter pBed("bed", "Minmin story time  (HH:MM)", bedBuf, 5);
        WiFiManagerParameter pSlp("slp", "Minmin lights out  (HH:MM)", slpBuf, 5);

        wm.addParameter(&pNtp);
        wm.addParameter(&pTz);
        wm.addParameter(&pBl);
        wm.addParameter(&pBed);
        wm.addParameter(&pSlp);
        wm.addParameter(&pWk);
        wm.setConnectTimeout(20);
        wm.setConfigPortalTimeout(180);
        wm.setTitle("CYD Clock");
        wm.setAPCallback([](WiFiManager*) { scrPortal(); });

        WiFi.setSleep(WIFI_PS_MAX_MODEM);  // max modem sleep between beacons

        if (digitalRead(BOOT_BTN) == LOW) {
            scrPortal();
            wm.startConfigPortal(AP_SSID, AP_PASS);
        } else {
            scrConnecting();
            wm.autoConnect(AP_SSID, AP_PASS);
        }

        strncpy(g_ntp, pNtp.getValue(), sizeof(g_ntp) - 1);
        strncpy(g_tz,  pTz.getValue(),  sizeof(g_tz)  - 1);
        g_bl = (uint8_t)constrain(atoi(pBl.getValue()), 0, 255);
        parseHHMM(pBed.getValue(), g_sched.bedH, g_sched.bedM);
        parseHHMM(pSlp.getValue(), g_sched.slpH, g_sched.slpM);
        parseHHMM(pWk.getValue(),  g_sched.wkH,  g_sched.wkM);
        prefsSave();
        blSet(g_bl);
    }

    scrSyncing();
    ntpSync();

    touchCalLoad();
    if (!g_calValid) runCalibration();

    tft.fillScreen(C_BG);
    g_fresh = true;
}

// ─────────────────────────────────────────────────────────────────
void loop() {
    uint32_t now = millis();

    // Boot button held → config portal (3 s countdown shown as red bar)
    if (digitalRead(BOOT_BTN) == LOW) {
        uint32_t t0 = millis();
        while (digitalRead(BOOT_BTN) == LOW) {
            uint32_t held = millis() - t0;
            if (held >= 3000) { runPortal(); return; }
            int px = (int)(held * 316 / 3000);
            tft.fillRect(0,      ANIM_Y, 320,       ANIM_H, C_BG);
            tft.fillRect(2,      ANIM_Y + 8, px,    8,      TFT_RED);
            tft.fillRect(2 + px, ANIM_Y + 8, 316 - px, 8,  C_BARG);
            tft.drawRect(1,      ANIM_Y + 7, 318,   10,     C_GREY);
            delay(30);
        }
        // Released before 3 s — redraw animation
        g_fresh = true;
    }

    // ── Touch: dim / brighten / re-calibrate ─────────────────────
    {
        uint16_t tx = 0, ty = 0;
        bool touched = g_calValid && getTouchXY(tx, ty);
        bool centreHold = false;
        uint32_t now_ms = millis();

        if (touched) {
            if (!g_touchAct) {
                g_touchStart = now_ms;
                g_touchAct   = true;
                g_lastBLTap  = 0;
            }
            g_blTouchAt = now_ms;
            g_blShowUntil = now_ms + BL_OVERLAY_MS;
            uint32_t held = now_ms - g_touchStart;

            if (tx < 107) {
                // Left third — dim
                if (now_ms - g_lastBLTap >= 300) {
                    g_lastBLTap = now_ms;
                    g_bl = (uint8_t)max(10, (int)g_bl - 20);
                    blSet(g_bl);
                    drawBlOverlay();
                    g_blSaveAt = now_ms + 3000;
                }
            } else if (tx > 213) {
                // Right third — brighten
                if (now_ms - g_lastBLTap >= 300) {
                    g_lastBLTap = now_ms;
                    g_bl = (uint8_t)min(255, (int)g_bl + 20);
                    blSet(g_bl);
                    drawBlOverlay();
                    g_blSaveAt = now_ms + 3000;
                }
            } else {
                centreHold = true;
                // Centre — hold 5 s to re-calibrate (orange countdown bar)
                int px = (int)(min(held, (uint32_t)5000) * 316UL / 5000UL);
                tft.fillRect(0,      ANIM_Y, 320,       ANIM_H, C_BG);
                tft.fillRect(2,      ANIM_Y + 8, px,    8,      TFT_ORANGE);
                tft.fillRect(2 + px, ANIM_Y + 8, 316 - px, 8,  C_BARG);
                tft.drawRect(1,      ANIM_Y + 7, 318,   10,     C_GREY);
                if (held >= 5000) {
                    g_touchAct = false;
                    runCalibration();
                    tft.fillScreen(C_BG);
                    g_fresh = true;
                    return;
                }
            }
        } else {
            if (g_touchAct) {
                g_touchAct = false;
                if (centreHold) g_fresh = true;  // only the centre-hold path needs a refresh
            }
        }

        // Expire brightness overlay after 5 s of no touch interaction.
        if (g_blOverlay && now_ms >= g_blShowUntil) {
            g_blOverlay = false;
            g_dowText   = nullptr;  // force the next DOW redraw
            g_dowColor  = 0;
        }

        // Deferred brightness save (3 s after last touch)
        if (g_blSaveAt && now_ms >= g_blSaveAt) {
            g_blSaveAt = 0;
            prefsSave();
        }
    }

    // Hourly NTP resync
    if (now - g_lastSync >= NTP_RESYNC_MS) {
        ntpSync();
    }

    // Get local time
    struct tm t;
    if (!getLocalTime(&t, 0)) { delay(LOOP_MS); return; }

    // Sub-second fraction for smooth bar animation
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    float sub = (float)tv.tv_usec / 1e6f;

    drawClock(&t, sub, g_fresh);
    g_fresh = false;

    delay(LOOP_MS);
}
