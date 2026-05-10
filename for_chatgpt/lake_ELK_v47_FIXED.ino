// *** CHANGE SKETCH NAME HERE ONLY — propagates everywhere automatically ***
#define SKETCH_NAME  "ELKv47"
#define PAIRED_WITH  "HVACv62"
// **************************************************************************
//
// ELKv47 -- Lake-only portrait per-CT display.
// Fix: commit received Frame B (pendingExt -> receivedExt) and erase stale waiting text.
// Restores the lake_elk_emporia_spec_v1 layout (column header + mains + 3
// priority rows + 6 device rows + MISC + footer) in PORTRAIT orientation
// (480 wide x 800 tall), with size 2 headers and size 1 data rows for ~80
// chars per line. Read distance ~3-4 ft.
//
// Carries forward from v44:
//   - WiFi STA join + ArduinoOTA listener at lake-elk.local:3232
//   - Channel auto-detect from WiFi.channel() so ESP-NOW aligns with AP
//   - RX-timeout watchdog reboot (5 min)
//   - Frame A struct extended to 320 bytes (lake* Today/Yest fields)
// Adds Frame B receive + per-CT rendering from ELKv36.
//
// PONDO ELK is on its own sketch (ELKv53) — this file is Lake-only.
// =================================================================

// v41: WiFi creds (both sites in source; runtime MAC picks one)
#define PONDO_SSID  "William_Lynn-2.4"
#define PONDO_PASS  "77330314"
#define LAKE_SSID   "marknet"
#define LAKE_PASS   "73duster"
const char* OTA_PASSWORD = "elk-pondo-2026";
bool wifiConnected = false;
unsigned long lastRxMs = 0;          // v41: RX-timeout watchdog

#include <Wire.h>
#include <esp_now.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_wifi.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include "LovyanGFX_Driver.h"

static LGFX lcd;
bool isLakeUnit = false;  // v45: set in setup() from MAC; controls rotation + draw path

// ================================================================
// FRAME A -- 320 bytes (must match HVACv62 exactly)
// ================================================================
struct hvac_data {
  float         outdoorTemp;
  int           furnaceWatts;
  float         furnaceKWh;
  int           heatingState;
  float         hddToday;
  float         cddToday;
  char          status[16];
  unsigned long packetNum;
  float         currentAmps;
  char          timeStr[20];
  float         runtimeHours;
  float         runtimePercent;
  bool          systemError;
  char          errorMsg[30];
  int           elkSuccess;
  char          lastRestartTime[20];
  int           wifiSuccessCount;
  int           wifiFailCount;
  int           weatherSuccessCount;
  int           weatherFailCount;
  int           ntpSuccessCount;
  int           ntpFailCount;
  int           wdtResetCount;
  float         acAmps;
  int           acWatts;
  float         acKWh;
  bool          acOn;
  float         acRuntimeHours;
  float         acRuntimePercent;
  uint32_t      freeHeap;
  float         wtrAmps;
  int           wtrWatts;
  float         wtrKWh;
  float         wtrRuntimeHours;
  float         wtrRuntimePercent;
  bool          isLakeHouse;
  int           espnowFailTotal;
  int           gthCycleCount;
  int           pumpCycleCount;
  int           wtrCycleCount;
  float         kwhRate;
  float         lakeGeoKwhToday;
  float         lakeGeoKwhYest;
  int           lakeGeoMinutesToday;
  int           lakeGeoMinutesYest;
  int           lakeGeoCyclesToday;
  int           lakeGeoCyclesYest;
  float         lakePumpKwhToday;
  float         lakePumpKwhYest;
  int           lakePumpMinutesToday;
  int           lakePumpMinutesYest;
  int           lakePumpCyclesToday;
  int           lakePumpCyclesYest;
  float         lakeWaterKwhToday;
  float         lakeWaterKwhYest;
  int           lakeWaterMinutesToday;
  int           lakeWaterMinutesYest;
  int           lakeWaterCyclesToday;
  int           lakeWaterCyclesYest;
  float         lakeMainsKwhToday;
  float         lakeMainsKwhYest;
  float         lakeMainsDollarsToday;
  float         lakeMainsDollarsYest;
} receivedData;
static hvac_data pendingData;

// ================================================================
// FRAME B -- hvac_lake_extension from HVACv62 sendFrameB()
// ================================================================
struct ext_device {
  uint16_t today_cents;
  uint16_t yesterday_cents;
  uint16_t runtime_min;
  uint8_t  cycles;
  uint8_t  flag;            // 0=normal 1=high 2=low 3=no_baseline
};

struct hvac_lake_extension {
  uint16_t magic;            // 0xE36B = ELKv36 Frame B sentinel
  uint8_t  version;
  uint8_t  reserved;
  uint16_t geo_today_cents,        geo_yesterday_cents;
  uint16_t pump_geo_today_cents,   pump_geo_yesterday_cents;
  uint16_t pump_wtr_today_cents,   pump_wtr_yesterday_cents;
  uint16_t geo_runtime_min;
  uint8_t  geo_cycles;
  uint8_t  reserved_a;
  uint16_t pump_runtime_min;
  uint8_t  pump_cycles;
  uint8_t  reserved_b;
  ext_device devices[6];   // water_heater, range, dryer, kitchen, freezer, sewage_pump
  uint16_t mains_today_cents;
  uint16_t mains_yesterday_cents;
  float    mains_kwh_today;
  uint16_t misc_today_cents;
  uint16_t misc_yesterday_cents;
  float    source_water_f;
  float    cop_today;
  char     first_error[24];
} receivedExt;
static hvac_lake_extension pendingExt;

// extDataReady means receivedExt contains at least one committed Frame B.
// extPendingReady means the ESP-NOW callback has placed a new Frame B in pendingExt
// and loop() still needs to copy it into receivedExt before drawing.
bool extDataReady = false;
volatile bool extPendingReady = false;
unsigned long lastExtRxMs = 0;

static const char* DEVICE_LABELS[6] = {
  "WATER HEATER", "RANGE", "DRYER", "KITCHEN", "FREEZER", "SEWAGE PUMP"
};

// ================================================================
// STATE
// ================================================================
volatile bool dataReady = false;
unsigned long packetsReceived       = 0;
unsigned long lastPacketTime        = 0;
bool          dataReceived          = false;
bool          firstDataReceived     = false;
int           rssi                  = 0;
unsigned long lastReceivedPacketNum = 0;
unsigned long packetsLost           = 0;
unsigned long successCount          = 0;
unsigned long totalCount            = 0;
float         linkQuality           = 0.0;
bool packetSizeMismatch      = false;
bool packetSizeMismatchShown = false;
int  lastRxSize              = 0;

// ================================================================
// LAKE PORTRAIT LAYOUT (480 wide x 800 tall, rotation 1)
// ================================================================
#define COL_BG       TFT_BLACK
#define COL_FG       TFT_WHITE
#define COL_GREEN    TFT_GREEN
#define COL_RED      TFT_RED
#define COL_YELLOW   TFT_YELLOW
#define COL_DIM      0x7BEF
#define COL_HDR      TFT_CYAN

// Y positions (size 2 = 16 px tall, size 1 = 8 px tall, +8 px gap typical)
static const int YL_HEADER     =   4;   // size 3 ~24 tall, ends ~28
static const int YL_STATUS     =  36;   // size 2 ~16 tall
static const int YL_DERIVED    =  60;   // size 2
static const int YL_OTA        =  84;   // small mDNS / IP info, size 1
static const int YL_COL_HDR    = 102;   // size 2, column labels
static const int YL_ROW_START  = 124;   // size 1 data rows begin
static const int ROW_PRI_H     =  16;   // priority rows tighter, size 2
static const int ROW_DEV_H     =  12;   // device rows at size 1
static const int YL_FOOTER     = 760;   // size 2 footer at bottom
static const int YL_BOTTOM     = 800;

// X positions (480 wide, size 1 = 6 px per char)
static const int XL_LABEL    =   4;
static const int XL_TIME_ON  = 130;
static const int XL_CYCLES   = 200;
static const int XL_PCT_ON   = 260;
static const int XL_TODAY    = 330;
static const int XL_YESTDAY  = 410;

// ================================================================
// PONDO LANDSCAPE LAYOUT (800 wide x 480 tall, rotation 0)  -- carried from v44
// ================================================================
#define Y_HEADER      10
#define Y_VERSION     50
#define Y_PACKETS     85
#define Y_STATUS_ROW 120
#define Y_COUNTERS   155
#define Y_COLHDR     190
#define Y_GTH        225
#define Y_WTR        260
#define Y_PUMP       295
#define Y_TOTAL      330
#define Y_SUMMARY    365
#define Y_STATUSBAR  400
#define Y_NOSIGNAL   400

// ── COP from Pi5 (Lake only)
float pi5COP = 0.0f;
bool  pi5COPvalid = false;
unsigned long lastMidnightPkt = 0;

void soundAlarm(const char *errorType) {
  Serial.print("[ALARM] "); Serial.println(errorType);
}

bool i2cScanForAddress(uint8_t address) {
  Wire.beginTransmission(address);
  return (Wire.endTransmission() == 0);
}

void sendI2CCommand(uint8_t command) {
  Wire.beginTransmission(0x30); Wire.write(command); Wire.endTransmission();
}

String formatRestartAMPM(const char* t) {
  int mo=0, dy=0, hr=0, mn=0;
  if (sscanf(t, "%d/%d %d:%d", &mo, &dy, &hr, &mn) == 4) {
    const char* ap = (hr >= 12) ? "pm" : "am";
    if (hr > 12) hr -= 12;
    if (hr == 0) hr = 12;
    char buf[18];
    snprintf(buf, sizeof(buf), "%02d/%02d %d:%02d%s", mo, dy, hr, mn, ap);
    return String(buf);
  }
  return String(t);
}

static inline float effectiveKwhRate() {
  float r = receivedData.kwhRate;
  if (r <= 0.0f || r > 1.0f) return 0.20f;
  return r;
}

// ================================================================
// FRAME B HELPERS
// ================================================================
static void fmtDollarsCents(uint16_t cents, char* out, size_t outsz) {
  if (cents == 0xFFFF) { snprintf(out, outsz, "--"); return; }
  snprintf(out, outsz, "$%d.%02d", cents / 100, cents % 100);
}

static void fmtMinToHHMM(uint16_t mins, char* out, size_t outsz) {
  if (mins == 0xFFFF) { snprintf(out, outsz, "--"); return; }
  snprintf(out, outsz, "%d:%02d", mins / 60, mins % 60);
}

static int minutes_since_midnight() {
  return (millis() / 60000) % 1440;
}

static float pct_on(uint16_t runtime_min) {
  int elapsed = minutes_since_midnight();
  if (elapsed <= 0 || runtime_min == 0xFFFF) return -1;
  float p = ((float)runtime_min / (float)elapsed) * 100.0f;
  if (p > 100.0f) p = 100.0f;
  return p;
}

// ================================================================
// ESP-NOW RX -- dispatches on length (Frame A vs Frame B)
// ================================================================
void onDataReceive(const esp_now_recv_info *recv_info, const uint8_t *data, int data_len) {
  rssi = recv_info->rx_ctrl->rssi;
  if (data_len == (int)sizeof(hvac_data)) {
    memcpy(&pendingData, data, sizeof(pendingData));
    dataReady = true;
    packetsReceived++; successCount++; lastPacketTime = millis(); lastRxMs = millis();
    if (pendingData.systemError) {
      soundAlarm("HVAC_ERROR");
    }
  } else if (data_len == (int)sizeof(hvac_lake_extension)) {
    hvac_lake_extension tmp;
    memcpy(&tmp, data, sizeof(tmp));
    if (tmp.magic == 0xE36B) {
      memcpy(&pendingExt, &tmp, sizeof(pendingExt));
      extPendingReady = true;
      lastExtRxMs = millis();
      lastRxMs = millis();
    } else {
      Serial.printf("[RX B] bad magic 0x%04X\n", tmp.magic);
    }
  } else {
    Serial.printf("[RX] unknown length %d\n", data_len);
    packetSizeMismatch = true; lastRxSize = data_len;
  }
  totalCount++;
  if (totalCount > 0) linkQuality = (float)successCount / (float)totalCount * 100.0f;
}

// ================================================================
// LAKE PORTRAIT DRAW ROUTINES
// ================================================================
static void drawLakeHeader() {
  lcd.fillRect(0, YL_HEADER, 480, 30, COL_BG);
  lcd.setTextColor(COL_HDR, COL_BG);
  lcd.setTextSize(3);
  lcd.setCursor(8, YL_HEADER);
  lcd.print("LAKE HOUSE");

  lcd.setTextSize(1);
  lcd.setTextColor(COL_FG, COL_BG);
  char vers[24];
  snprintf(vers, sizeof(vers), "%s/%s", PAIRED_WITH, SKETCH_NAME);
  lcd.setCursor(280, YL_HEADER + 4);
  lcd.print(vers);
  lcd.setCursor(280, YL_HEADER + 14);
  lcd.printf("%-19s", receivedData.timeStr);
}

static void drawLakeStatusLine() {
  bool err = receivedData.systemError ||
             (receivedExt.first_error[0] != '\0' && extDataReady);
  uint16_t bg = err ? COL_RED : COL_GREEN;
  lcd.fillRect(0, YL_STATUS, 480, 22, bg);
  lcd.setTextColor(COL_BG, bg);
  lcd.setTextSize(2);
  lcd.setCursor(4, YL_STATUS + 3);
  if (err) {
    const char* msg = receivedData.systemError ? receivedData.errorMsg : receivedExt.first_error;
    lcd.printf("STATUS: ERR %.30s", msg);
  } else {
    lcd.print("STATUS: GOOD");
  }
}

static void drawLakeDerivedLine() {
  lcd.fillRect(0, YL_DERIVED, 480, 22, COL_BG);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.setTextSize(1);
  lcd.setCursor(4, YL_DERIVED + 6);
  float src = extDataReady ? receivedExt.source_water_f : 0.0f;
  float cop = extDataReady ? receivedExt.cop_today : 0.0f;
  lcd.printf("HDD %.1f  CDD %.1f  COP %.1f  Out %.0fF  Src %.1fF",
             receivedData.hddToday, receivedData.cddToday, cop,
             receivedData.outdoorTemp, src);
}

static void drawLakeOtaLine() {
  lcd.fillRect(0, YL_OTA, 480, 16, COL_BG);
  lcd.setTextColor(COL_DIM, COL_BG);
  lcd.setTextSize(1);
  lcd.setCursor(4, YL_OTA + 4);
  if (wifiConnected) {
    lcd.printf("lake-elk.local  RSSI:%ddBm  RX:%lu  Lost:%lu",
               rssi, packetsReceived, packetsLost);
  } else {
    lcd.print("[OTA] WiFi disconnected");
  }
}

static void drawLakeColumnHeader() {
  lcd.fillRect(0, YL_COL_HDR, 480, 18, COL_BG);
  lcd.setTextColor(COL_DIM, COL_BG);
  lcd.setTextSize(1);
  lcd.setCursor(XL_LABEL,    YL_COL_HDR + 4); lcd.print("Device");
  lcd.setCursor(XL_TIME_ON,  YL_COL_HDR + 4); lcd.print("Time On");
  lcd.setCursor(XL_CYCLES,   YL_COL_HDR + 4); lcd.print("Cycles");
  lcd.setCursor(XL_PCT_ON,   YL_COL_HDR + 4); lcd.print("% On");
  lcd.setCursor(XL_TODAY,    YL_COL_HDR + 4); lcd.print("Today $");
  lcd.setCursor(XL_YESTDAY,  YL_COL_HDR + 4); lcd.print("Yest $");
}

// Draw one device/priority row at given y, height h.
static void drawLakeRow(int y, int h, int textsize, const char* label,
                        const char* time_on, int cycles, float pct,
                        uint16_t today_cents, uint16_t yest_cents, uint8_t flag) {
  lcd.fillRect(0, y, 480, h, COL_BG);
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.setTextSize(textsize);
  int yo = (textsize == 1) ? 2 : 1;
  lcd.setCursor(XL_LABEL, y + yo); lcd.print(label);
  if (time_on)        { lcd.setCursor(XL_TIME_ON, y + yo); lcd.print(time_on); }
  else                { lcd.setCursor(XL_TIME_ON, y + yo); lcd.print("--"); }
  if (cycles >= 0)    { lcd.setCursor(XL_CYCLES,  y + yo); lcd.printf("%d", cycles); }
  else                { lcd.setCursor(XL_CYCLES,  y + yo); lcd.print("--"); }
  if (pct >= 0)       { lcd.setCursor(XL_PCT_ON,  y + yo); lcd.printf("%.1f%%", pct); }
  else                { lcd.setCursor(XL_PCT_ON,  y + yo); lcd.print("--"); }
  uint16_t tcol = (flag == 1 || flag == 2) ? COL_YELLOW : COL_FG;
  char dbuf[12];
  fmtDollarsCents(today_cents, dbuf, sizeof(dbuf));
  lcd.setTextColor(tcol, COL_BG);
  lcd.setCursor(XL_TODAY, y + yo); lcd.print(dbuf);
  fmtDollarsCents(yest_cents, dbuf, sizeof(dbuf));
  lcd.setTextColor(COL_FG, COL_BG);
  lcd.setCursor(XL_YESTDAY, y + yo); lcd.print(dbuf);
}

static void drawLakeAllRows() {
  int y = YL_ROW_START;

  // Mains row at size 2 (priority)
  uint16_t mains_today = extDataReady ? receivedExt.mains_today_cents     : 0xFFFF;
  uint16_t mains_yest  = extDataReady ? receivedExt.mains_yesterday_cents : 0xFFFF;
  drawLakeRow(y, ROW_PRI_H + 4, 2, "MAINS", "--", -1, 100.0f, mains_today, mains_yest, 0);
  y += ROW_PRI_H + 6;

  // GTH (geo compressor)
  {
    char ton[12];
    fmtMinToHHMM(extDataReady ? receivedExt.geo_runtime_min : 0xFFFF, ton, sizeof(ton));
    int cyc = extDataReady ? (int)receivedExt.geo_cycles : -1;
    drawLakeRow(y, ROW_PRI_H, 2, "GTH", ton, cyc,
                pct_on(extDataReady ? receivedExt.geo_runtime_min : 0xFFFF),
                extDataReady ? receivedExt.geo_today_cents     : 0xFFFF,
                extDataReady ? receivedExt.geo_yesterday_cents : 0xFFFF, 0);
    y += ROW_PRI_H + 4;
  }

  // PUMP/GTH
  drawLakeRow(y, ROW_PRI_H, 2, "PUMP/GTH", "--", -1, -1,
              extDataReady ? receivedExt.pump_geo_today_cents     : 0xFFFF,
              extDataReady ? receivedExt.pump_geo_yesterday_cents : 0xFFFF, 0);
  y += ROW_PRI_H + 4;

  // PUMP/WTR
  {
    char ton[12];
    fmtMinToHHMM(extDataReady ? receivedExt.pump_runtime_min : 0xFFFF, ton, sizeof(ton));
    int cyc = extDataReady ? (int)receivedExt.pump_cycles : -1;
    drawLakeRow(y, ROW_PRI_H, 2, "PUMP/WTR", ton, cyc,
                pct_on(extDataReady ? receivedExt.pump_runtime_min : 0xFFFF),
                extDataReady ? receivedExt.pump_wtr_today_cents     : 0xFFFF,
                extDataReady ? receivedExt.pump_wtr_yesterday_cents : 0xFFFF, 0);
    y += ROW_PRI_H + 6;
  }

  // 6 device rows at size 1
  for (int i = 0; i < 6; i++) {
    if (extDataReady) {
      char ton[12];
      fmtMinToHHMM(receivedExt.devices[i].runtime_min, ton, sizeof(ton));
      float p = pct_on(receivedExt.devices[i].runtime_min);
      drawLakeRow(y, ROW_DEV_H, 1, DEVICE_LABELS[i], ton,
                  receivedExt.devices[i].cycles, p,
                  receivedExt.devices[i].today_cents,
                  receivedExt.devices[i].yesterday_cents,
                  receivedExt.devices[i].flag);
    } else {
      drawLakeRow(y, ROW_DEV_H, 1, DEVICE_LABELS[i], "--", -1, -1, 0xFFFF, 0xFFFF, 0);
    }
    y += ROW_DEV_H;
  }

  // MISC residual row
  drawLakeRow(y, ROW_DEV_H, 1, "MISC", "--", -1, -1,
              extDataReady ? receivedExt.misc_today_cents     : 0xFFFF,
              extDataReady ? receivedExt.misc_yesterday_cents : 0xFFFF, 0);
}

static void drawLakeFooter() {
  lcd.fillRect(0, YL_FOOTER, 480, YL_BOTTOM - YL_FOOTER, COL_DIM);
  lcd.setTextColor(COL_BG, COL_DIM);
  lcd.setTextSize(2);
  if (extDataReady) {
    int today_total = (int)receivedExt.mains_today_cents;
    int yest_total  = (int)receivedExt.mains_yesterday_cents;
    int delta = today_total - yest_total;
    char buf[60];
    snprintf(buf, sizeof(buf), "T $%d.%02d  Y $%d.%02d  %s$%d.%02d",
             today_total/100, today_total%100,
             yest_total/100,  yest_total%100,
             delta < 0 ? "-" : "+",
             abs(delta)/100, abs(delta)%100);
    lcd.setCursor(4, YL_FOOTER + 8);
    lcd.print(buf);
  } else {
    lcd.setCursor(4, YL_FOOTER + 8);
    lcd.print("(waiting for Frame B)");
  }
}

void drawStaticLayoutLake() {
  lcd.fillScreen(COL_BG);
  drawLakeHeader();
  lcd.setTextColor(COL_YELLOW, COL_BG);
  lcd.setTextSize(2);
  lcd.setCursor(80, 380);
  lcd.print("Waiting for HVACv62...");
}

void drawStaticLayoutPondo() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_CYAN, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(220, Y_HEADER); lcd.print("HVAC Monitor");
  lcd.setTextColor(TFT_WHITE, TFT_BLACK); lcd.setTextSize(2);
  lcd.setCursor(50, Y_VERSION); lcd.print(SKETCH_NAME);
}

// ================================================================
// LAKE updateDisplay -- portrait layout
// ================================================================
void updateDisplayLake() {
  // Erase the one-time setup message ("Waiting for HVACv62...") and any stale text
  // in the unused middle/lower area before redrawing the live lake table.
  lcd.fillRect(0, 340, 480, 180, COL_BG);
  drawLakeHeader();
  drawLakeStatusLine();
  drawLakeDerivedLine();
  drawLakeOtaLine();
  drawLakeColumnHeader();
  drawLakeAllRows();
  drawLakeFooter();
}

// ================================================================
// PONDO updateDisplay -- carry-over from v44 (landscape Today/Yest)
// (kept identical for safety; never used at Lake)
// ================================================================
void updateDisplayPondo() {
  // Pondo runs its own ELKv53 firmware. This function is a stub here.
  // If this v45 sketch is ever flashed to Pondo by mistake, show a warning.
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_RED, TFT_BLACK); lcd.setTextSize(3);
  lcd.setCursor(50, 200);
  lcd.print("ELKv47 IS LAKE-ONLY");
  lcd.setCursor(50, 250);
  lcd.print("Reflash with ELKv53");
}

// ================================================================
// WiFi + OTA (carried from v44 unchanged)
// ================================================================
void connectWiFiAndOta() {
  uint8_t mac[6]; WiFi.macAddress(mac);
  const char* ssid = isLakeUnit ? LAKE_SSID  : PONDO_SSID;
  const char* pass = isLakeUnit ? LAKE_PASS  : PONDO_PASS;
  const char* host = isLakeUnit ? "lake-elk" : "pondo-elk";
  Serial.printf("[OTA] WiFi.begin(%s) as %s.local...\n", ssid, host);
  WiFi.setHostname(host);
  WiFi.begin(ssid, pass);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 12000) delay(250);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[OTA] WiFi timeout -- ESP-NOW only mode this boot");
    wifiConnected = false; return;
  }
  wifiConnected = true;
  uint8_t apCh = WiFi.channel();
  Serial.printf("[OTA] WiFi OK, IP=%s Ch=%u\n", WiFi.localIP().toString().c_str(), apCh);
  esp_wifi_set_channel(apCh, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[ESP-NOW] channel set to %u (matches AP)\n", apCh);
  if (MDNS.begin(host)) {
    MDNS.addService("arduino", "tcp", 3232);
    Serial.printf("[OTA] mDNS: %s.local\n", host);
  }
  ArduinoOTA.setHostname(host);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([](){ Serial.println("[OTA] flash started"); });
  ArduinoOTA.onEnd([](){ Serial.println("[OTA] flash complete -- rebooting"); });
  ArduinoOTA.onError([](ota_error_t e){ Serial.printf("[OTA] error %u\n", e); });
  ArduinoOTA.begin();
  Serial.println("[OTA] listening on TCP/3232");
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200); delay(2000);
  Serial.println(SKETCH_NAME " - ALIVE");
  Serial.println("\n===============================================");
  Serial.println("   " SKETCH_NAME " - Lake-only portrait per-CT layout");
  Serial.println("   Paired with " PAIRED_WITH);
  Serial.println("   Frame A: 320 bytes  Frame B: hvac_lake_extension");
  Serial.println("===============================================\n");

  // Detect site BEFORE display init so we can pick rotation correctly
  WiFi.mode(WIFI_STA); delay(100);
  uint8_t mac[6]; WiFi.macAddress(mac);
  isLakeUnit = (mac[5] == 0xA0);
  Serial.printf("[WiFi] ELK MAC: %02X:%02X:%02X:%02X:%02X:%02X  -> %s\n",
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
    isLakeUnit ? "LAKE (portrait)" : "PONDO (landscape)");

  Serial.println("[DISPLAY] Init LovyanGFX...");
  lcd.init();
  lcd.setRotation(isLakeUnit ? 3 : 0);   // v46: Lake portrait flipped 180 (USB at top), Pondo landscape
  lcd.setBrightness(255);
  lcd.setFont(&fonts::Font0);
  Serial.println("[DISPLAY] Ready");

  Serial.println("[INIT] Scanning I2C...");
  const int MAX_I2C_ATTEMPTS = 10;
  int i2cAttempts = 0;
  bool i2cReady = false;
  while (i2cAttempts < MAX_I2C_ATTEMPTS) {
    if (i2cScanForAddress(0x30) && i2cScanForAddress(0x5D)) {
      i2cReady = true;
      Serial.println("[I2C] Ready"); break;
    }
    i2cAttempts++;
    Serial.printf("[I2C] Attempt %d/%d\n", i2cAttempts, MAX_I2C_ATTEMPTS);
    sendI2CCommand(250);
    pinMode(1, OUTPUT); digitalWrite(1, LOW); delay(120);
    pinMode(1, INPUT);  delay(100);
  }
  if (!i2cReady) Serial.println("[I2C] FAILED — check display power and wiring");
  sendI2CCommand(0);

  if (isLakeUnit) drawStaticLayoutLake();
  else            drawStaticLayoutPondo();

  // v41: bring up WiFi STA FIRST so we know AP channel before ESP-NOW init
  connectWiFiAndOta();

  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.printf("[ESP-NOW] FAILED: %d\n", (int)result);
    while (1) delay(1000);
  }
  delay(100);
  esp_now_register_recv_cb(onDataReceive);
  Serial.println("[ESP-NOW] Ready -- listening for " PAIRED_WITH " Frame A + Frame B");
  receivedExt.first_error[0] = '\0';
  Serial.println("[OK] Setup done!");
  Serial.println("===============================================");
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  if (wifiConnected) ArduinoOTA.handle();

  // RX-timeout watchdog: 5 min after last packet -> restart
  static unsigned long bootMs = millis();
  if (lastRxMs > 0 && (millis() - lastRxMs) > 300000UL) {
    Serial.println("[v45] no ESP-NOW packet for 5 min -- restarting");
    delay(500); ESP.restart();
  }
  if (lastRxMs == 0 && (millis() - bootMs) > 360000UL) {
    Serial.println("[v45] no ESP-NOW packet since boot for 6 min -- restarting");
    delay(500); ESP.restart();
  }

  // Read COP from Pi5 via serial (Lake only)
  static char serialBuf[32];
  static int  serialPos = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      serialBuf[serialPos] = '\0';
      if (serialPos > 4 && strncmp(serialBuf, "COP:", 4) == 0) {
        float v = atof(serialBuf + 4);
        if (v >= 0.0f && v < 20.0f) {
          pi5COP = v;
          pi5COPvalid = (v > 0.01f);
        }
      }
      serialPos = 0;
    } else if (serialPos < (int)sizeof(serialBuf) - 1) {
      serialBuf[serialPos++] = c;
    }
  }

  if (dataReady) {
    dataReady = false;
    memcpy(&receivedData, &pendingData, sizeof(receivedData));
    dataReceived = true; firstDataReceived = true;
    if (lastReceivedPacketNum > 0) {
      if (receivedData.packetNum > lastReceivedPacketNum)
        packetsLost = receivedData.packetNum - lastReceivedPacketNum - 1;
      else if (receivedData.packetNum < lastReceivedPacketNum) {
        packetsLost = 0; lastReceivedPacketNum = 0;
      } else packetsLost = 0;
    } else packetsLost = 0;
    lastReceivedPacketNum = receivedData.packetNum;
  }

  // Commit Frame B data received by the ESP-NOW callback.
  // ELKv46 set extDataReady in the callback but never copied pendingExt into
  // receivedExt, while the display reads receivedExt. That made the Frame B
  // table stay zero/stale even though RX and RSSI looked healthy.
  if (extPendingReady) {
    extPendingReady = false;
    memcpy(&receivedExt, &pendingExt, sizeof(receivedExt));
    extDataReady = true;
    dataReceived = true;
  }

  if (dataReceived) {
    dataReceived = false;
    if (isLakeUnit) updateDisplayLake();
    else            updateDisplayPondo();
  }

  // Periodic diagnostic (every 60s)
  static unsigned long lastDiagTime = 0;
  if (millis() - lastDiagTime >= 60000) {
    Serial.printf("[DIAG] %s paired:%s RX:%lu Lost:%lu Link:%.0f%% RSSI:%d Heap:%lu ext:%s\n",
      SKETCH_NAME, PAIRED_WITH, packetsReceived, packetsLost, linkQuality, rssi,
      (unsigned long)ESP.getFreeHeap(), extDataReady ? "yes" : "no");
    lastDiagTime = millis();
  }

  // Packet-size mismatch warning
  if (packetSizeMismatch && !packetSizeMismatchShown) {
    Serial.printf("[ERROR] Packet size mismatch: got %d\n", lastRxSize);
    packetSizeMismatchShown = true;
  }
  if (!packetSizeMismatch && packetSizeMismatchShown) packetSizeMismatchShown = false;

  yield();
}
