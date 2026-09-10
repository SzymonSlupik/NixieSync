/*
 * NixieSync - NTP time-set module for a nixie clock built around an ST62T65C6
 *
 * Target : Seeed XIAO ESP32-C3
 * Core   : arduino-esp32 3.x
 * Deps   : none beyond the ESP32 core
 *
 * Clock behaviour this is built for (confirmed on the hardware):
 *   - 24 h display with seconds tubes; resets to 00:00:00, repeatably
 *   - HOUR+  press: +1 hour, disturbs neither minutes nor seconds
 *   - MIN+   press: +1 minute AND zeroes the seconds
 *
 * That asymmetry is what the whole planner rests on. Hours are decoupled from
 * everything, so a DST correction is just N hour presses fired whenever.
 * Minute presses freeze the clock's minute (each restarts the 60 s seconds
 * window), so a minute train ending exactly on a true minute boundary sets
 * minutes and seconds together.
 *
 * Hardware: four low-side AO3400A FETs. The clock pulls its HOUR, MIN and
 * RESET lines up through 1 kohm and its switch common is ground, so pulling a
 * line down is exactly a button press. Q3 on the RESET pin for ~50 ms is
 * confirmed to return the clock to 00:00:00.
 *
 * See NixieSync-PCB-layout.pdf for the schematic, and README.md for the
 * M == 0 special case in planFullSet(), which is not obvious.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <esp_sntp.h>
#include <esp_mac.h>

// WPA2-Enterprise. The API was renamed between arduino-esp32 2.x (esp_wpa2.h,
// esp_wifi_sta_wpa2_ent_*) and 3.x (esp_eap_client.h, esp_eap_client_*). This
// targets 3.x; the 2.x branch is best-effort.
#if __has_include(<esp_eap_client.h>)
  #include <esp_eap_client.h>
  #define EAP_IDENTITY(p, n)   esp_eap_client_set_identity((const unsigned char *)(p), (n))
  #define EAP_USERNAME(p, n)   esp_eap_client_set_username((const unsigned char *)(p), (n))
  #define EAP_PASSWORD(p, n)   esp_eap_client_set_password((const unsigned char *)(p), (n))
  #define EAP_CA(p, n)         esp_eap_client_set_ca_cert((const unsigned char *)(p), (n))
  #define EAP_CA_CLEAR()       esp_eap_client_clear_ca_cert()
  #define EAP_NO_TIME_CHECK(b) esp_eap_client_set_disable_time_check(b)
  #define EAP_ENABLE()         esp_wifi_sta_enterprise_enable()
  #define EAP_DISABLE()        esp_wifi_sta_enterprise_disable()
#else
  #include <esp_wpa2.h>
  #define EAP_IDENTITY(p, n)   esp_wifi_sta_wpa2_ent_set_identity((const unsigned char *)(p), (n))
  #define EAP_USERNAME(p, n)   esp_wifi_sta_wpa2_ent_set_username((const unsigned char *)(p), (n))
  #define EAP_PASSWORD(p, n)   esp_wifi_sta_wpa2_ent_set_password((const unsigned char *)(p), (n))
  #define EAP_CA(p, n)         esp_wifi_sta_wpa2_ent_set_ca_cert((const unsigned char *)(p), (n))
  #define EAP_CA_CLEAR()       esp_wifi_sta_wpa2_ent_clear_ca_cert()
  #define EAP_NO_TIME_CHECK(b) esp_wifi_sta_wpa2_ent_set_disable_time_check(b)
  #define EAP_ENABLE()         esp_wifi_sta_wpa2_ent_enable()
  #define EAP_DISABLE()        esp_wifi_sta_wpa2_ent_disable()
#endif
#include <sys/time.h>
#include <time.h>
#include <limits.h>

// ---------------------------------------------------------------- pin map --
// ESP32-C3: D1=GPIO3 D2=GPIO4 D3=GPIO5 D4=GPIO6 D5=GPIO7 D10=GPIO10.
// D0/D8/D9 are strapping pins and are deliberately unused.
static const int PIN_RST_SENSE = D1;   // divider from the ST62 RESET pin
static const int PIN_DRV_HOUR  = D2;   // Q1, HIGH = HOUR line pulled low
static const int PIN_DRV_MIN   = D3;   // Q2, HIGH = MIN line pulled low
static const int PIN_DRV_RST   = D4;   // Q3, HIGH = clock held in reset
static const int PIN_LED       = D5;
static const int PIN_BUTTON    = D10;  // active low, internal pull-up

static const char *AP_PASSWORD = nullptr;  // nullptr = open AP, so the captive
                                           // portal pops reliably on iOS
static const uint32_t STA_CONNECT_WINDOW_MS = 300000;  // the router boots much
                                                       // slower than we do
static const uint32_t STA_ATTEMPT_MS = 20000;
// If association fails outright we fall back to the captive portal, but an
// unattended clock must not sit there for ever: reboot and retry the whole
// sequence, unless someone is actually using the portal.
// The portal is never opened automatically when credentials are stored: an AP
// that appears whenever the network is down is an attack surface, and a deauth
// would be enough to summon it. It is reached by holding the button instead,
// and it closes again on its own.
static const uint32_t PORTAL_LIFE_MS = 600000;    // 10 min
static const uint32_t PORTAL_IDLE_MS = 180000;    // 3 min since last request
static const uint32_t STA_RETRY_MS   = 30000;     // background re-association
static const uint32_t BTN_PORTAL_MS  = 1500;      // hold to open the portal
static const uint32_t BTN_WIPE_MS    = 8000;      // hold to erase credentials
static const uint32_t BTN_DEBOUNCE_MS = 25;       // level must hold this long
static const int64_t  US_MIN = 60000000LL;

// ------------------------------------------------------------------ config --
static const uint32_t CFG_MAGIC = 0x4E585935;  // "NXY5"
static const uint16_t CFG_VER   = 5;

static const uint8_t AUTH_PSK  = 0;
static const uint8_t AUTH_PEAP = 1;

struct Config {
  uint32_t magic;
  uint16_t ver;
  uint16_t pad;

  char ssid[33];
  char pass[65];          // WPA2-PSK passphrase

  uint8_t authMode;       // AUTH_PSK or AUTH_PEAP
  char eapIdentity[64];   // outer / anonymous identity; blank = use eapUser
  char eapUser[64];       // inner MSCHAPv2 username
  char eapPass[64];
  char ntp1[64];
  char ntp2[64];
  char tz[64];

  uint16_t resetHoldMs;   // 50 ms; the ST6 adds its own startup delay
  int16_t  powerOnLagMs;  // reset released this early to offset MCU startup
  uint16_t pulseOnMs;
  uint16_t pulseOffMs;
  uint16_t postResetMs;   // settle between reset release and the first pulse

  uint8_t  senseEnable;   // watch the clock's RESET line for foreign resets
  uint8_t  senseInverted; // 1 = buffered via Q4 (pin HIGH = clock in reset)
  uint8_t  dstUseReset;   // 1 = full reset for DST, 0 = hour presses only
  uint8_t  dailyEnable;   // optional belt-and-braces resync
  uint8_t  dailyHour;
  uint8_t  dailyMin;
  uint8_t  dryRun;
};

// Structs used in function signatures must be declared before the first
// function definition: the Arduino IDE injects generated prototypes at exactly
// that point, so a struct defined further down produces "does not name a type".
struct Plan {
  bool    valid = false;
  String  err;
  bool    doReset = false;
  int     pulseH = 0, pulseM = 0;
  int64_t assertUs = 0, releaseUs = 0;
  int64_t hourFirstUs = 0, minFirstUs = 0;
  int64_t endUs = 0;
  time_t  refEpoch = 0;
};

static Config cfg;
static String  caCert;      // optional PEM for RADIUS server validation
static Preferences prefs;
static WebServer server(80);
static DNSServer dns;

static bool   portalMode  = false;
static bool   syncing     = false;
static time_t lastSyncAt  = 0;
static String lastResult  = "not run yet";
static long   lastOffset  = LONG_MIN;
static int    lastSyncDay = -1;

// Set at boot, cleared once the clock has been set. NTP can easily take longer
// than a single wait would allow: the AP may be up in a minute while the WAN
// link takes several more, so association alone does not mean we have time yet.
static bool bootSyncPending = true;
static uint32_t portalStartedAt = 0;
static uint32_t lastClientMs = 0;
static bool     staUp = false;
static bool     webUp = false;
static uint32_t staNextTry = 0;
static uint8_t  btnPattern = 0;   // 0 none, 1 portal armed, 2 wipe armed
static uint32_t foreignResets = 0;

static bool pendFull     = false;
static bool pendAdj      = false;
static int  pendAdjAhead = 0;   // hours the clock currently reads AHEAD of truth

static void defaults(Config &c) {
  memset(&c, 0, sizeof(c));
  c.magic = CFG_MAGIC;
  c.ver   = CFG_VER;
  strlcpy(c.ntp1, "pool.ntp.org", sizeof(c.ntp1));
  strlcpy(c.ntp2, "time.google.com", sizeof(c.ntp2));
  strlcpy(c.tz, "CET-1CEST,M3.5.0,M10.5.0/3", sizeof(c.tz));  // Europe/Warsaw
  c.resetHoldMs  = 50;
  c.powerOnLagMs = 0;
  c.pulseOnMs    = 120;
  c.pulseOffMs   = 180;
  c.postResetMs  = 300;
  c.authMode      = AUTH_PSK;
  c.senseEnable   = 1;
  c.senseInverted = 1;   // Q4 buffer stage; 0 only for a bare divider
  c.dstUseReset  = 0;
  c.dailyEnable  = 0;
  c.dailyHour    = 3;
  c.dailyMin     = 30;     // 02:30 does not exist on the spring-forward night
  c.dryRun       = 1;      // ships safe; turn off after bench validation
}

static void loadConfig() {
  defaults(cfg);
  prefs.begin("nixiesync", true);
  Config t;
  size_t n = prefs.getBytes("cfg", &t, sizeof(t));
  caCert = prefs.getString("ca", "");
  prefs.end();
  if (n == sizeof(t) && t.magic == CFG_MAGIC && t.ver == CFG_VER) cfg = t;
}

// Enough to attempt an association. Without this the portal would open on
// every boot when only half the enterprise fields had been filled in.
static bool credentialsPresent() {
  if (!strlen(cfg.ssid)) return false;
  if (cfg.authMode == AUTH_PEAP) return strlen(cfg.eapUser) && strlen(cfg.eapPass);
  return true;
}
static void saveConfig() {
  prefs.begin("nixiesync", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}
static void wipeConfig() {
  prefs.begin("nixiesync", false);
  prefs.clear();
  prefs.end();
}

// ------------------------------------------------------------ time helpers --
static int64_t nowUs() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// Absolute-deadline sleep: every edge is scheduled against a wall-clock
// deadline, never a relative delay, so error cannot accumulate over the train.
static void sleepUntil(int64_t targetUs) {
  for (;;) {
    int64_t d = targetUs - nowUs();
    if (d <= 0) return;
    // runPlan() can block here for the best part of a minute. Chunk the wait so
    // the LED keeps blinking: a solid-off LED for 30 s is indistinguishable
    // from a dead module, which is why presses during a sync felt ignored.
    if (d > 2000) digitalWrite(PIN_LED, (millis() / 120) & 1);
    if (d > 200000)     delay(100);
    else if (d > 20000) delay((uint32_t)((d - 15000) / 1000));
    else if (d > 2000)  delay(1);
    else                delayMicroseconds(50);
  }
}

static int64_t nextMinuteBoundaryUs(int64_t notBeforeUs) {
  int64_t sec = notBeforeUs / 1000000;
  if (notBeforeUs % 1000000) sec++;
  return ((sec + 59) / 60) * 60 * 1000000LL;
}

static void localHM(time_t t, int &h, int &m) {
  struct tm tmv;
  localtime_r(&t, &tmv);
  h = tmv.tm_hour;
  m = tmv.tm_min;
}

static bool timeValid() { return time(nullptr) > 1700000000; }

// ESP32's newlib names the field __tm_gmtoff and only aliases it to tm_gmtoff
// under _DEFAULT_SOURCE, so derive the offset from the two broken-down times.
// mktime() is avoided deliberately: gmtime_r() leaves tm_isdst = 0, which would
// make mktime return the standard offset rather than the current one — exactly
// the distinction the DST watcher depends on.
static long utcOffset() {
  time_t t = time(nullptr);
  struct tm lt, gt;
  localtime_r(&t, &lt);
  gmtime_r(&t, &gt);

  long diff = (lt.tm_hour - gt.tm_hour) * 3600L +
              (lt.tm_min  - gt.tm_min)  * 60L +
              (lt.tm_sec  - gt.tm_sec);

  int dday = lt.tm_yday - gt.tm_yday;
  if (dday == 1 || dday < -1)      diff += 86400L;
  else if (dday == -1 || dday > 1) diff -= 86400L;
  return diff;
}

static String fmtEpoch(time_t t) {
  if (t <= 0) return "-";
  struct tm tmv;
  localtime_r(&t, &tmv);
  char b[32];
  strftime(b, sizeof(b), "%Y-%m-%d %H:%M:%S", &tmv);
  return String(b);
}

// The ST62 drives its own RESET pin low when the LVD, watchdog or POR fires,
// so a low reading means the clock is in reset — whoever caused it.
static bool clockInReset() { return digitalRead(PIN_RST_SENSE) == LOW; }

// ------------------------------------------------------------- sync engine --

/*
 * Full set. The clock is at 00:00:00 the moment reset is released.
 *   pulseH = hour(T_end)    - hour presses are pure +1 and perturb nothing
 *   pulseM = minute(T_end)  - the clock's minute is still 00 at the first
 *                             minute press, since the hour train is <= 6.9 s
 *
 * M > 0: one contiguous train, hours then minutes, scheduled backwards so the
 *        last minute press lands on T_end and zeroes the seconds there.
 * M == 0: no minute press exists to zero the seconds, so release reset exactly
 *        on T_end instead and run the hour train afterwards — hour presses do
 *        not disturb minutes or seconds, so it does not matter when they occur.
 */
static Plan planFullSet() {
  Plan p;
  p.doReset = true;

  const int64_t perPulse = (int64_t)(cfg.pulseOnMs + cfg.pulseOffMs) * 1000;
  const int64_t hold     = (int64_t)cfg.resetHoldMs * 1000;
  const int64_t settle   = (int64_t)cfg.postResetMs * 1000;
  const int64_t lag      = (int64_t)cfg.powerOnLagMs * 1000;
  const int64_t now      = nowUs();

  if (perPulse >= 55000000LL) {
    p.err = "pulse period too long: the clock's minute would roll mid-train";
    return p;
  }

  const int64_t earliest = now + 1500000LL + hold + settle +
                           (int64_t)83 * perPulse;

  for (int k = 0; k < 4; k++) {
    const int64_t tEnd = nextMinuteBoundaryUs(earliest) + (int64_t)k * US_MIN;
    int H, M;
    localHM((time_t)(tEnd / 1000000), H, M);

    int64_t hourFirst, minFirst, release;
    if (M > 0) {
      minFirst  = tEnd - (int64_t)(M - 1) * perPulse;
      hourFirst = minFirst - (int64_t)H * perPulse;
      release   = hourFirst - settle - lag;
      p.endUs   = tEnd + (int64_t)cfg.pulseOnMs * 1000;
    } else {
      release   = tEnd - lag;
      hourFirst = tEnd + settle;
      minFirst  = 0;
      p.endUs   = hourFirst + (int64_t)H * perPulse;
    }
    const int64_t assertAt = release - hold;
    if (assertAt < now + 300000LL) continue;

    p.pulseH      = H;
    p.pulseM      = M;
    p.assertUs    = assertAt;
    p.releaseUs   = release;
    p.hourFirstUs = hourFirst;
    p.minFirstUs  = minFirst;
    p.refEpoch    = (time_t)(tEnd / 1000000);
    p.valid       = true;
    return p;
  }
  p.err = "could not schedule a minute boundary";
  return p;
}

/*
 * DST. Hour presses touch neither minutes nor seconds, so this is simply
 * N presses fired whenever — no boundary, no minute correction, no reset.
 * Spring forward resolves to 1 press, fall back to 23.
 */
static Plan planHourAdjust(int clockAheadHours) {
  Plan p;
  p.doReset     = false;
  p.pulseH      = ((-clockAheadHours) % 24 + 24) % 24;
  p.pulseM      = 0;
  p.hourFirstUs = nowUs() + 500000LL;
  p.endUs       = p.hourFirstUs +
                  (int64_t)p.pulseH * (cfg.pulseOnMs + cfg.pulseOffMs) * 1000;
  p.refEpoch    = time(nullptr);
  p.valid       = true;
  return p;
}

// The clock pulls HOUR, MIN and RESET up through 1 kohm; Q1..Q3 pull them
// down. Q1 and Q2 must never be on together: that would tie the two lines to
// the same node and advance both counters. Every press goes through here,
// one at a time.
static void pulseTrain(int pin, int count, int64_t firstEdgeUs) {
  const int64_t perPulse = (int64_t)(cfg.pulseOnMs + cfg.pulseOffMs) * 1000;
  for (int i = 0; i < count; i++) {
    const int64_t edge = firstEdgeUs + (int64_t)i * perPulse;
    sleepUntil(edge);
    if (!cfg.dryRun) digitalWrite(pin, HIGH);
    sleepUntil(edge + (int64_t)cfg.pulseOnMs * 1000);
    digitalWrite(pin, LOW);
  }
}

static bool runPlan(const Plan &p) {
  if (!p.valid) {
    lastResult = "plan failed: " + p.err;
    Serial.printf("[sync] %s\n", lastResult.c_str());
    return false;
  }
  if (!p.doReset && p.pulseH == 0) {
    lastResult = "no adjustment needed";
    return true;
  }

  syncing = true;
  Serial.printf("[sync] %s target %s  H+%d M+%d%s\n",
                p.doReset ? "RESET+SET" : "HOUR-ADJUST",
                fmtEpoch(p.refEpoch).c_str(), p.pulseH, p.pulseM,
                cfg.dryRun ? "  (DRY RUN)" : "");

  if (p.doReset) {
    sleepUntil(p.assertUs);
    const int64_t at = nowUs();
    if (!cfg.dryRun) digitalWrite(PIN_DRV_RST, HIGH);
    sleepUntil(p.releaseUs);
    digitalWrite(PIN_DRV_RST, LOW);   // unconditional: never leave it asserted
    Serial.printf("[sync] reset asserted %lld ms\n",
                  (long long)((nowUs() - at) / 1000));
  }

  pulseTrain(PIN_DRV_HOUR, p.pulseH, p.hourFirstUs);
  if (p.pulseM > 0) pulseTrain(PIN_DRV_MIN, p.pulseM, p.minFirstUs);

  lastSyncAt = time(nullptr);
  struct tm tmv;
  localtime_r(&lastSyncAt, &tmv);
  lastSyncDay = tmv.tm_yday;

  const long err = (long)((nowUs() - p.endUs) / 1000);
  lastResult = String(p.doReset ? "reset+set to " : "hour-adjusted at ") +
               fmtEpoch(p.refEpoch) + " (H+" + p.pulseH + " M+" + p.pulseM +
               ", sched err " + String(err) + " ms)" +
               (cfg.dryRun ? " [DRY RUN]" : "");
  Serial.printf("[sync] %s\n", lastResult.c_str());
  syncing = false;
  return true;
}

// ------------------------------------------------------------- HTML helper --
static String esc(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else o += c;
  }
  return o;
}

static String numField(const char *label, const char *name, long val,
                       const char *hint = "") {
  String s = "<label>" + String(label) + "<input type='number' name='" + name +
             "' value='" + String(val) + "'>";
  if (hint[0]) s += "<small>" + String(hint) + "</small>";
  return s + "</label>";
}

static String sel2(const char *label, const char *name, int cur,
                   const char *v0, const char *v1, const char *hint = "") {
  String s = "<label>" + String(label) + "<select name='" + name + "'>";
  s += "<option value='0'"; if (!cur) s += " selected"; s += ">" + String(v0) + "</option>";
  s += "<option value='1'"; if (cur)  s += " selected"; s += ">" + String(v1) + "</option>";
  s += "</select>";
  if (hint[0]) s += "<small>" + String(hint) + "</small>";
  return s + "</label>";
}

static const char *PAGE_CSS =
  "body{background:#111;color:#ddd;font:16px/1.5 system-ui,sans-serif;margin:0;"
  "padding:16px;max-width:520px;margin:auto}"
  "h1{font-size:20px;color:#ff8c42}h2{font-size:15px;color:#ff8c42;margin:22px 0 6px;"
  "border-bottom:1px solid #333;padding-bottom:4px}label{display:block;margin:10px 0}"
  "input,select{width:100%;box-sizing:border-box;padding:9px;margin-top:3px;"
  "background:#1c1c1c;color:#eee;border:1px solid #444;border-radius:5px;font-size:16px}"
  "small{color:#888;font-size:12px;display:block;margin-top:2px}"
  "button{width:100%;padding:13px;margin-top:18px;background:#ff8c42;color:#111;"
  "border:0;border-radius:5px;font-size:17px;font-weight:600}"
  ".s{background:#1a1a1a;border:1px solid #333;border-radius:6px;padding:10px;"
  "font-size:14px;margin-bottom:12px}.s b{color:#ff8c42}a{color:#ff8c42}";

static String ssidSelect() {
  int n = WiFi.scanNetworks(false, false);
  String s = "<label>WiFi network<select name='ssid'>";
  if (n <= 0) s += "<option value=''>-- none found --</option>";
  for (int i = 0; i < n; i++) {
    String id = WiFi.SSID(i);
    if (!id.length()) continue;
    s += "<option value='" + esc(id) + "'";
    if (id == String(cfg.ssid)) s += " selected";
    s += ">" + esc(id) + "  (" + String(WiFi.RSSI(i)) + " dBm)</option>";
  }
  s += "</select><small><a href='/'>rescan</a></small></label>";
  s += "<label>...or type it (hidden SSID, overrides the list)"
       "<input name='ssid_manual' placeholder='SSID'></label>";
  WiFi.scanDelete();
  return s;
}

static void handleRoot() {
  lastClientMs = millis();
  String h = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<title>NixieSync</title><style>";
  h += PAGE_CSS;
  h += "</style></head><body><h1>NixieSync</h1>";

  if (!portalMode) {
    h += "<div class='s'>IP <b>" + WiFi.localIP().toString() + "</b> on <b>" +
         esc(WiFi.SSID()) + "</b><br>Now <b>" +
         (timeValid() ? fmtEpoch(time(nullptr)) : String("no NTP")) +
         "</b>  (UTC offset " + String(timeValid() ? utcOffset() / 3600 : 0) +
         " h)<br>Last set <b>" + fmtEpoch(lastSyncAt) + "</b><br>Result <b>" +
         esc(lastResult) + "</b><br>Clock RESET line <b>" +
         (clockInReset() ? "asserted (in reset)" : "released (running)") +
         "</b>  (D1 pin reads " +
         (digitalRead(PIN_RST_SENSE) ? "HIGH" : "LOW") +
         ", foreign resets since boot " + String(foreignResets) +
         ")<br>Security <b>" +
         (cfg.authMode == AUTH_PEAP ? "PEAP/MSCHAPv2" : "WPA2-PSK") + "</b>" +
         (cfg.authMode == AUTH_PEAP
              ? String(", CA ") + (caCert.length() ? "stored" : "<b>not set</b>")
              : String("")) +
         "</div>";
    h += "<a href='/sync'>&#9654; full reset + set now</a> &nbsp; "
         "<a href='/test?r=h'>test hour pulse</a> &nbsp; "
         "<a href='/test?r=m'>test minute pulse</a> &nbsp; "
         "<a href='/test?r=r'>test reset pulse</a>";
  }

  h += "<form method='POST' action='/save'><h2>Network</h2>";
  h += ssidSelect();
  const String keep = String(portalMode ? "" : "blank = keep current");
  h += sel2("Security", "auth", cfg.authMode,
            "WPA2-PSK (passphrase)", "WPA2-Enterprise (PEAP/MSCHAPv2)");
  h += "<label>PSK passphrase<input type='password' name='pass' placeholder='" +
       keep + "'><small>WPA2-PSK only</small></label>";
  h += "<h2>Enterprise (PEAP / MSCHAPv2)</h2>";
  h += "<label>Anonymous identity<input name='eapid' value='" +
       esc(cfg.eapIdentity) + "'><small>outer identity; blank = same as "
       "username</small></label>";
  h += "<label>Username<input name='eapuser' value='" + esc(cfg.eapUser) +
       "'></label>";
  h += "<label>Password<input type='password' name='eappass' placeholder='" +
       keep + "'></label>";
  h += "<label>RADIUS server CA certificate (PEM)<textarea name='ca' rows='4' "
       "style='width:100%;box-sizing:border-box;padding:9px;margin-top:3px;"
       "background:#1c1c1c;color:#eee;border:1px solid #444;border-radius:5px;"
       "font-size:13px' placeholder='" +
       String(caCert.length() ? "a certificate is stored - paste to replace"
                              : "-----BEGIN CERTIFICATE-----") +
       "'></textarea><small>Strongly recommended. Without it the server is not "
       "validated, and a rogue AP can capture the MSCHAPv2 handshake.</small>"
       "</label>";
  h += "<label><input type='checkbox' name='cadel' value='1' "
       "style='width:auto'> erase the stored certificate</label>";

  h += "<h2>Time source</h2>";
  h += "<label>NTP server<input name='ntp1' value='" + esc(cfg.ntp1) + "'></label>";
  h += "<label>Fallback NTP<input name='ntp2' value='" + esc(cfg.ntp2) + "'></label>";
  h += "<label>Timezone (POSIX TZ)<input name='tz' value='" + esc(cfg.tz) +
       "'><small>Poland: CET-1CEST,M3.5.0,M10.5.0/3</small></label>";

  h += "<h2>Timing</h2>";
  h += numField("Reset assert (ms)", "hold", cfg.resetHoldMs,
                "50 ms is ample: the ST6 adds its own delay to the pin");
  h += numField("Startup lag comp (ms)", "lag", cfg.powerOnLagMs,
                "reset releases this early; raise if the clock ends up late");
  h += numField("Pulse closed (ms)", "on", cfg.pulseOnMs);
  h += numField("Pulse open (ms)", "off", cfg.pulseOffMs);
  h += numField("Settle after reset (ms)", "settle", cfg.postResetMs);

  h += "<h2>Triggers</h2>";
  h += sel2("Watch the clock's RESET line", "se", cfg.senseEnable,
            "disabled", "enabled",
            "catches LVD/watchdog resets the module slept through");
  h += sel2("Sense polarity", "si", cfg.senseInverted,
            "bare divider (pin LOW = reset)", "Q4 buffer (pin HIGH = reset)",
            "leave on Q4 unless you fitted a plain divider");
  h += sel2("DST change", "dstr", cfg.dstUseReset,
            "hour presses only (no blink)", "full reset");
  h += sel2("Daily re-sync", "de", cfg.dailyEnable, "disabled", "enabled");
  h += numField("Daily hour", "dh", cfg.dailyHour, "avoid 02:xx (DST gap)");
  h += numField("Daily minute", "dm", cfg.dailyMin);
  h += sel2("Dry run", "dry", cfg.dryRun, "off - drive the outputs",
            "on - log the plan only");

  h += "<button type='submit'>Save &amp; reboot</button></form></body></html>";
  server.send(200, "text/html", h);
}

static uint16_t argU(const char *n, uint16_t d) {
  return server.hasArg(n) ? (uint16_t)server.arg(n).toInt() : d;
}

static void handleSave() {
  lastClientMs = millis();
  String ssid = server.arg("ssid_manual");
  ssid.trim();
  if (!ssid.length()) ssid = server.arg("ssid");
  if (ssid.length()) strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (server.arg("pass").length())
    strlcpy(cfg.pass, server.arg("pass").c_str(), sizeof(cfg.pass));

  cfg.authMode = argU("auth", AUTH_PSK) ? AUTH_PEAP : AUTH_PSK;
  strlcpy(cfg.eapIdentity, server.arg("eapid").c_str(), sizeof(cfg.eapIdentity));
  if (server.arg("eapuser").length())
    strlcpy(cfg.eapUser, server.arg("eapuser").c_str(), sizeof(cfg.eapUser));
  if (server.arg("eappass").length())
    strlcpy(cfg.eapPass, server.arg("eappass").c_str(), sizeof(cfg.eapPass));

  String ca = server.arg("ca");
  ca.trim();
  if (server.arg("cadel") == "1")   caCert = "";
  else if (ca.length())             caCert = ca;
  if (server.arg("ntp1").length())
    strlcpy(cfg.ntp1, server.arg("ntp1").c_str(), sizeof(cfg.ntp1));
  strlcpy(cfg.ntp2, server.arg("ntp2").c_str(), sizeof(cfg.ntp2));
  if (server.arg("tz").length())
    strlcpy(cfg.tz, server.arg("tz").c_str(), sizeof(cfg.tz));

  cfg.resetHoldMs  = constrain((int)argU("hold", 50), 10, 30000);
  cfg.powerOnLagMs = (int16_t)constrain(server.arg("lag").toInt(), -2000L, 2000L);
  cfg.pulseOnMs    = constrain((int)argU("on", 120), 10, 2000);
  cfg.pulseOffMs   = constrain((int)argU("off", 180), 10, 2000);
  cfg.postResetMs  = constrain((int)argU("settle", 300), 0, 20000);
  cfg.senseEnable   = argU("se", 1) ? 1 : 0;
  cfg.senseInverted = argU("si", 1) ? 1 : 0;
  cfg.dstUseReset  = argU("dstr", 0) ? 1 : 0;
  cfg.dailyEnable  = argU("de", 0) ? 1 : 0;
  cfg.dailyHour    = argU("dh", 3) % 24;
  cfg.dailyMin     = argU("dm", 30) % 60;
  cfg.dryRun       = argU("dry", 0) ? 1 : 0;

  saveConfig();
  prefs.begin("nixiesync", false);
  prefs.putString("ca", caCert);
  prefs.end();
  server.send(200, "text/html",
              "<meta charset='utf-8'><body style='background:#111;color:#ddd;"
              "font:16px system-ui;padding:20px'>Saved. Rebooting...</body>");
  delay(400);
  ESP.restart();
}

static void handleSyncNow() {
  pendFull = true;
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static void handleTest() {
  const String w = server.arg("r");
  // "r" fires the reset driver for resetHoldMs. Deliberately not masked by the
  // syncing flag, so the sense path sees it: the clock should blink to
  // 00:00:00 and the log should report a foreign reset and a full set. That is
  // an end-to-end test of Q3, Q4 and the sense wiring in one click.
  const int pin = (w == "r") ? PIN_DRV_RST
                             : ((w == "m") ? PIN_DRV_MIN : PIN_DRV_HOUR);
  const uint16_t ms = (w == "r") ? cfg.resetHoldMs : cfg.pulseOnMs;
  if (!cfg.dryRun) {
    digitalWrite(pin, HIGH);
    delay(ms);
    digitalWrite(pin, LOW);
  }
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

static void handleNotFound() {
  if (portalMode) {
    server.sendHeader("Location", "http://192.168.4.1/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "not found");
}

static void startWebServer() {
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/sync", handleSyncNow);
  server.on("/test", handleTest);
  const char *probes[] = {"/generate_204", "/gen_204", "/hotspot-detect.html",
                          "/library/test/success.html", "/connecttest.txt",
                          "/ncsi.txt", "/redirect", "/canonical.html",
                          "/success.txt", "/fwlink"};
  for (auto q : probes) server.on(q, handleNotFound);
  server.onNotFound(handleNotFound);
  server.begin();
}

// --------------------------------------------------------------- WiFi/NTP --
static void ensureWeb() {
  if (!webUp) { startWebServer(); webUp = true; }
}

static void startPortal() {
  portalMode = true;

  // Read the MAC from eFuse, not from the driver. On a first-ever boot there
  // are no credentials, so connectSTA() returns without ever starting WiFi;
  // WiFi.macAddress() then has no station interface to query, fails, and
  // leaves the buffer at zeros - which is where "NixieSync-0000" came from.
  // esp_read_mac() works whether or not the driver has been initialised.
  uint8_t mac[6] = {0};
  if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK)
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

  WiFi.mode(WIFI_AP);
  char ap[32];
  snprintf(ap, sizeof(ap), "NixieSync-%02X%02X", mac[4], mac[5]);
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1),
                    IPAddress(255, 255, 255, 0));
  WiFi.softAP(ap, AP_PASSWORD);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53, "*", IPAddress(192, 168, 4, 1));
  ensureWeb();
  portalStartedAt = millis();
  lastClientMs = millis();
  Serial.printf("[portal] AP \"%s\" -> http://192.168.4.1/\n", ap);
}

// One place that knows how to start an association, so the boot attempt and the
// background retry cannot drift apart.
static void beginWifi() {
  if (cfg.authMode == AUTH_PEAP) {
    // No battery-backed RTC: after a power cut we boot at 1970 and cannot learn
    // the real time until we are on the network we are trying to join. Without
    // this, certificate expiry checking would refuse the association forever -
    // exactly the outage case this whole project exists to handle.
    EAP_NO_TIME_CHECK(true);

    if (caCert.length()) EAP_CA(caCert.c_str(), caCert.length() + 1);
    else                 EAP_CA_CLEAR();

    const char *ident = strlen(cfg.eapIdentity) ? cfg.eapIdentity : cfg.eapUser;
    EAP_IDENTITY(ident, strlen(ident));
    EAP_USERNAME(cfg.eapUser, strlen(cfg.eapUser));
    EAP_PASSWORD(cfg.eapPass, strlen(cfg.eapPass));
    EAP_ENABLE();
    WiFi.begin(cfg.ssid);
  } else {
    EAP_DISABLE();          // settings persist in NVS; clear them going back to PSK
    WiFi.begin(cfg.ssid, cfg.pass);
  }
}

static bool connectSTA() {
  if (!credentialsPresent()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  const uint32_t start = millis();
  while (millis() - start < STA_CONNECT_WINDOW_MS) {
    Serial.printf("[wifi] connecting to \"%s\" (%s)...\n", cfg.ssid,
                  cfg.authMode == AUTH_PEAP ? "PEAP/MSCHAPv2" : "WPA2-PSK");
    beginWifi();
    const uint32_t t = millis();
    while (millis() - t < STA_ATTEMPT_MS) {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[wifi] up, %s\n", WiFi.localIP().toString().c_str());
        return true;
      }
      digitalWrite(PIN_LED, (millis() / 250) & 1);
      delay(50);
    }
    // The router is almost certainly still booting after the same outage that
    // brought us up, so back off and retry rather than giving up.
    WiFi.disconnect(true);
    delay(3000);
  }
  return false;
}

static void configureNtp() {
  configTzTime(cfg.tz, cfg.ntp1, strlen(cfg.ntp2) ? cfg.ntp2 : nullptr);
  // Smooth mode uses adjtime for small offsets so SNTP cannot step the system
  // clock out from under an in-flight pulse train. The first sync (RTC at 1970)
  // still steps, which is what we want.
  esp_sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
  esp_sntp_set_sync_interval(3600 * 1000);
  esp_sntp_restart();
}

// Association is retried indefinitely in the background. Falling back to an
// open AP would be the wrong answer to a router that is merely slow.
static void serviceSta() {
  if (portalMode || staUp || !credentialsPresent()) return;
  if (WiFi.status() == WL_CONNECTED) {
    staUp = true;
    Serial.printf("[wifi] up, %s\n", WiFi.localIP().toString().c_str());
    ensureWeb();
    if (MDNS.begin("nixie")) MDNS.addService("http", "tcp", 80);
    configureNtp();
    return;
  }
  if ((int32_t)(millis() - staNextTry) < 0) return;
  staNextTry = millis() + STA_RETRY_MS;
  Serial.println("[wifi] retrying association");
  WiFi.disconnect(true);
  beginWifi();
}

// ------------------------------------------------------------ housekeeping --
// Three actions on one button, chosen by how long it is held. The LED shows
// which one is armed while you hold it, so you can let go before committing:
//   < 1.5 s    re-sync the clock now
//   1.5 - 8 s  open the configuration portal (slow blink)
//   > 8 s      erase stored credentials and reboot (fast blink)
//
// Both edges are debounced. The earlier version committed on a single HIGH
// sample, so any contact chatter part-way through a hold read as a release and
// fired whichever action happened to be armed at that instant - a hold meant
// for the wipe would open the portal, a hold meant for the portal would fire a
// re-sync. That was the unreliability.
static void serviceButton() {
  static bool     released = true;      // debounced level
  static bool     lastRaw = true;
  static uint32_t lastEdge = 0;
  static uint32_t downAt = 0;

  const bool raw = (digitalRead(PIN_BUTTON) != LOW);
  if (raw != lastRaw) {                 // bouncing: restart the timer
    lastRaw = raw;
    lastEdge = millis();
    return;
  }
  if (millis() - lastEdge < BTN_DEBOUNCE_MS) return;

  if (raw == released) {                // no committed change
    if (!released) {
      const uint32_t held = millis() - downAt;
      btnPattern = (held > BTN_WIPE_MS) ? 2 : (held > BTN_PORTAL_MS ? 1 : 0);
    }
    return;
  }

  released = raw;
  if (!released) {                      // press committed
    downAt = millis();
    btnPattern = 0;
    return;
  }

  const uint32_t held = millis() - downAt;   // release committed
  btnPattern = 0;
  if (held > BTN_WIPE_MS) {
    Serial.println("[btn] erasing configuration");
    wipeConfig();
    delay(200);
    ESP.restart();
  } else if (held > BTN_PORTAL_MS) {
    if (!portalMode) {
      Serial.println("[btn] opening the configuration portal");
      startPortal();
    }
  } else {
    Serial.println("[btn] re-sync requested");
    pendFull = true;
  }
}

static void serviceDst() {
  if (!timeValid()) return;
  static uint32_t last = 0;
  if (millis() - last < 10000) return;
  last = millis();

  const long off = utcOffset();
  if (lastOffset == LONG_MIN) { lastOffset = off; return; }
  if (off == lastOffset) return;

  const long d = off - lastOffset;
  lastOffset = off;
  Serial.printf("[dst] UTC offset changed by %ld s\n", d);

  if (d % 3600 || cfg.dstUseReset) {
    pendFull = true;                  // odd offset, or configured to reset
  } else {
    pendAdjAhead = (int)(-d / 3600);  // clock now reads this many hours ahead
    pendAdj = true;
  }
}

// The ST62 drives RESET low itself on LVD, watchdog or power-on reset. Watching
// that line closes the hole where a dip resets the clock but not the module,
// which would otherwise leave the display stuck at 00:00 until the next reboot.
static void serviceResetSense() {
  if (!cfg.senseEnable || syncing) return;
  static uint32_t last = 0;
  static int lowRun = 0;
  static bool armed = false;
  if (millis() - last < 5) return;
  last = millis();

  if (clockInReset()) {
    if (lowRun < 4 && ++lowRun == 4) {
      armed = true;
      Serial.println("[sense] RESET asserted");
    }
  } else {
    lowRun = 0;
    if (armed) {
      armed = false;
      foreignResets++;
      Serial.printf("[sense] RESET released - foreign reset #%u, "
                    "scheduling a full set\n", (unsigned)foreignResets);
      pendFull = true;
    }
  }
}

// Single owner for the indicator. Scattered digitalWrite() calls left it
// wherever the last blink happened to land, so it could sit dark after a
// button press and look like nothing had happened.
static void updateLed() {
  if (btnPattern == 2)      digitalWrite(PIN_LED, (millis() / 80) & 1);
  else if (btnPattern == 1) digitalWrite(PIN_LED, (millis() / 400) & 1);
  else if (syncing)         digitalWrite(PIN_LED, (millis() / 120) & 1);
  else if (portalMode)      digitalWrite(PIN_LED, (millis() / 150) & 1);
  else if (!staUp)          digitalWrite(PIN_LED, (millis() / 700) & 1);
  else                      digitalWrite(PIN_LED, HIGH);
}

// -------------------------------------------------------------------- main --
void setup() {
  // Outputs low before anything else. R4-R6 (100k gate pull-downs) cover the
  // window before this line executes; without them the clock would see a
  // spurious press on every module reset.
  pinMode(PIN_DRV_HOUR, OUTPUT);
  pinMode(PIN_DRV_MIN, OUTPUT);
  pinMode(PIN_DRV_RST, OUTPUT);
  digitalWrite(PIN_DRV_HOUR, LOW);
  digitalWrite(PIN_DRV_MIN, LOW);
  digitalWrite(PIN_DRV_RST, LOW);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, LOW);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_RST_SENSE, INPUT);   // divider sets the level; no internal pull

  Serial.begin(115200);
  delay(200);
  Serial.println("\n[boot] NixieSync r3");

  loadConfig();

  if (connectSTA()) {
    staUp = true;
    portalMode = false;
    ensureWeb();
    if (MDNS.begin("nixie")) MDNS.addService("http", "tcp", 80);
    configureNtp();
  } else if (credentialsPresent()) {
    // Credentials exist, so the network is simply not back yet. Keep retrying
    // in the background; do NOT raise an AP. Reset sensing carries on either
    // way, so a clock reset during the outage is not missed.
    Serial.println("[wifi] no association yet - retrying in the background");
  } else {
    startPortal();          // nothing stored: genuine first-time setup
  }
  digitalWrite(PIN_LED, HIGH);
}

void loop() {
  if (portalMode) dns.processNextRequest();
  server.handleClient();
  serviceButton();

  // SNTP keeps retrying in the background for as long as it takes, so pick up
  // the moment time becomes valid and set the clock then.
  if (!portalMode && bootSyncPending && timeValid()) {
    bootSyncPending = false;
    Serial.println("[boot] NTP arrived late, setting the clock now");
    pendFull = true;
  }

  // The on-demand portal closes itself so the AP is not left up indefinitely.
  if (portalMode && credentialsPresent() &&
      millis() - portalStartedAt > PORTAL_LIFE_MS &&
      millis() - lastClientMs > PORTAL_IDLE_MS) {
    Serial.println("[portal] idle, closing and rejoining the network");
    delay(100);
    ESP.restart();
  }
  serviceSta();
  serviceDst();
  serviceResetSense();

  if (!portalMode && cfg.dailyEnable && timeValid()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    if (tmv.tm_yday != lastSyncDay && tmv.tm_hour == cfg.dailyHour &&
        tmv.tm_min == cfg.dailyMin)
      pendFull = true;
  }

  if (!portalMode && timeValid() && (pendFull || pendAdj)) {
    const bool full = pendFull;      // a full set supersedes a pending adjust
    const int ahead = pendAdjAhead;
    pendFull = pendAdj = false;
    runPlan(full ? planFullSet() : planHourAdjust(ahead));
  }

  updateLed();
  delay(5);
}
