# Lake ELK ESP-NOW issue — combined package for Grok

This single file contains everything: the problem writeup AND both relevant source files inline. Just share this file with Grok.

================================================================================
PART 1 — PROBLEM WRITEUP (from README)
================================================================================

# Lake ELK ESP-NOW silent receive + missing self-restart — for Grok analysis

## Files in this folder

| File | What it is |
|---|---|
| `lake_ELK_v46.ino` | The exact firmware source running on the lake ELK display board RIGHT NOW. ELKv46 — only difference from prior version is `setRotation(1) → setRotation(3)` for portrait flip. |
| `pondo_HVAC_v64_closest_to_v62.ino` | The closest available source to what's flashed on lake HVAC. Lake HVAC is running HVACv62. The exact v62 source wasn't preserved as a backup file; this v64 backup is the closest predecessor still on disk. The differences from v62 are minor (no struct changes between v62 and v64). Useful for understanding the SEND side of ESP-NOW (what HVAC transmits, struct shape, etc.). |

## Hardware
- **Lake HVAC**: ESP32 (native USB-CDC, `CDCOnBoot=cdc`), runs HVACv62. CT clamps measure geo compressor, well pump, water-pump current. Sends a 228-byte struct via ESP-NOW every ~10 sec to the ELK.
- **Lake ELK**: ESP32-S3 (native USB-CDC), runs ELKv46. Receives ESP-NOW packets, renders to a LovyanGFX touchscreen. Also joins WiFi STA (`marknet`) for ArduinoOTA listening.
- Both boards on the same WiFi (single fixed channel). ESP-NOW is locked to that channel via `connectWiFiAndOta()` at boot.
- Sender MAC = HVAC. Receiver MAC = ELK = `80:B5:4E:F6:D8:A0`.

## Observed problem
Lake ELK display showed two contradictory things at once:
- `Waiting for HVACv62...` — text printed once during `setup()` (line 521 of ELK.ino), only erased when first valid packet draws over it.
- Diagnostic strip showing `RX:7557 Lost:0 RSSI:-45dBm` — the RX counter (`packetsReceived` in code) only increments inside the receive callback AFTER the size check passes. So the ELK had received 7557 size-matched packets at some point in this boot session.

Both can't be true unless either:
- the screen-draw path froze while the receive callback kept working, leaving stale text on screen, OR
- the watchdog WAS firing periodically and `packetsReceived` survives across an `ESP.restart()` somehow (it shouldn't — it's a regular int), OR
- something else I'm missing

The lake HVAC firmware is alive and continuously sending packets the entire time — verified by the HVAC's own serial log showing `Pkt:N` lines incrementing throughout the period in question.

## Existing self-restart code (ELKv46, lines 552-560)
```c
void loop() {
  if (wifiConnected) ArduinoOTA.handle();
  static unsigned long bootMs = millis();
  if (lastRxMs > 0 && (millis() - lastRxMs) > 300000UL) {
    Serial.println("[v41] no ESP-NOW packet for 5 min -- restarting");
    delay(500); ESP.restart();
  }
  if (lastRxMs == 0 && (millis() - bootMs) > 360000UL) {
    Serial.println("[v41] no ESP-NOW packet since boot for 6 min -- restarting");
    delay(500); ESP.restart();
  }
  // ... rest of loop, including LovyanGFX screen draw
}
```

This SHOULD have rebooted ELK after 5 min stuck. It didn't.

## Receive callback (ELK.ino lines 195-213)
```c
void onDataReceive(const esp_now_recv_info *recv_info, const uint8_t *data, int data_len) {
  rssi = recv_info->rx_ctrl->rssi;
  Serial.print("[RX] Size: "); Serial.println(data_len);
  if (data_len != (int)sizeof(pendingData)) {
    Serial.printf("[ERROR] Mismatch! Got:%d Exp:%d -- reflash both!\n", data_len, (int)sizeof(pendingData));
    packetSizeMismatch = true; lastRxSize = data_len; totalCount++; return;  // early return — lastRxMs NOT updated
  }
  packetSizeMismatch = false;
  memcpy(&pendingData, data, sizeof(pendingData));
  dataReady = true; packetsReceived++; successCount++;
  lastPacketTime = millis(); lastRxMs = millis();   // only updates on size match
  ...
}
```

`lastRxMs` only updates on size-matched packets. Mismatched packets bump `totalCount` but not `packetsReceived` and not `lastRxMs`.

## Workaround applied
OTA-flashed the same ELKv46 firmware → ELK rebooted → ESP-NOW pairing re-established → packets flowing again. **This is symptomatic, not root cause.**

After OTA reboot, ELK reports:
```
[DIAG] ELKv46 paired:HVACv62 RX:6 Lost:0 Link:50% RSSI:-50 Heap:251400
```
- `RX:6` and climbing → struct sizes DO match (228 bytes both sides). Not a struct issue.
- `Link:50%` → only HALF of ESP-NOW transmissions are succeeding despite strong RSSI (-50 dBm).

## Observability gap
Until very recently, no logger was running on the ELK side (`/dev/ttyUSB0` on lake-pi5). All ELK serial output went into the void — no historical record exists from when the issue was occurring. We can only analyze the code, not the runtime evidence.

## Open questions
1. **Why didn't the 5-min `lastRxMs` watchdog fire** when ELK was in the stuck state? Either the receive callback was being called (updating `lastRxMs`) or it wasn't (so `lastRxMs` stayed stale and watchdog should have fired). What third option is there?
2. **What explains `Link:50%`** with strong RSSI on a single fixed-channel ESP-NOW link? Both boards run WiFi STA (for OTA) AND ESP-NOW concurrently — could be airtime contention. Or could be receiver-side buffer/queue saturation. What's the most likely cause and how to diagnose?
3. **What's a defensible fix that doesn't require physical power-cycle of the ELK?** Owner explicitly does NOT want host-side close+reopen of `/dev/ttyUSB0` as recovery — that has its own bad side effects on this hardware (sends control signals that reset the chip).
4. **Is there a possible firmware-side issue** where `loop()` blocks somewhere (e.g., inside `ArduinoOTA.handle()`, the LovyanGFX draw path, `esp_now_register_recv_cb` callback context, or audio alarm code) that prevents the watchdog check from being reached?

## Pasting this prompt to Grok
The two .ino files are the actual source. Drop them in along with this README and ask Grok to analyze.

================================================================================
PART 2 — LAKE ELK FIRMWARE SOURCE (lake_ELK_v46.ino)
Currently flashed on the lake ELK board. ELKv46.
================================================================================

```cpp
// *** CHANGE SKETCH NAME HERE ONLY — propagates everywhere automatically ***
#define SKETCH_NAME  "ELKv46"
#define PAIRED_WITH  "HVACv62"
// **************************************************************************
//
// ELKv46 -- Lake-only portrait per-CT display.
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
bool extDataReady = false;
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
      extDataReady = true;
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
  lcd.print("ELKv46 IS LAKE-ONLY");
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
```

================================================================================
PART 3 — HVAC FIRMWARE SOURCE (closest to lake's v62)
Lake HVAC runs HVACv62. Exact v62 source not preserved as backup.
This is HVACv64 — the closest predecessor on disk, no struct changes between v62-v64.
================================================================================

```cpp
// *** CHANGE SKETCH NAME HERE ONLY — propagates everywhere automatically ***
#define SKETCH_NAME "HVACv64"
#define PAIRED_WITH "ELKv35"

// **************************************************************************

// ── FIELD-TUNABLE SETTINGS ──
#define THRESH_IDLE_W       50
#define HEAP_WARN_THRESHOLD 50000
#define LOCATION_PIN         4    // floating=PONDO  jumper to GND=Lake House

// ── ELK MAC ADDRESSES — GPIO4 selects at boot ────────────────────────────
uint8_t pondoMAC[] = { 0x80, 0xB5, 0x4E, 0xF6, 0xD8, 0xDC };  // <-- CHANGE HERE: PONDO ELK
uint8_t lakeMAC[]  = { 0x80, 0xB5, 0x4E, 0xF6, 0xD8, 0xA0 };  // <-- CHANGE HERE: Lake House ELK
uint8_t elkMAC[6];  // set at boot from LOCATION_PIN

// ── CT SETTINGS — set at boot from LOCATION_PIN ──────────────────────────
float CT1_RATIO; float CT1_CAL_FACTOR; float CT1_VOLTAGE; float CT1_PF;
float CT2_RATIO; float CT2_CAL_FACTOR; float CT2_VOLTAGE; float CT2_PF;

// ── PONDO CT VALUES ──────────────────────────────────────────────────────
#define PONDO_CT1_RATIO       15.0   // <-- CHANGE HERE
#define PONDO_CT1_CAL_FACTOR   2.06  // <-- CHANGE HERE
#define PONDO_CT1_VOLTAGE    120.0   // <-- CHANGE HERE
#define PONDO_CT1_PF           0.90  // <-- CHANGE HERE
#define PONDO_CT2_RATIO       20.0   // <-- CHANGE HERE
#define PONDO_CT2_CAL_FACTOR   1.18  // <-- CHANGE HERE
#define PONDO_CT2_VOLTAGE    240.0   // <-- CHANGE HERE
#define PONDO_CT2_PF           0.85  // <-- CHANGE HERE

// ── LAKE HOUSE CT VALUES ─────────────────────────────────────────────────
#define LAKE_CT1_RATIO        30.0   // <-- CHANGE HERE
#define LAKE_CT1_CAL_FACTOR    1.39  // <-- CHANGE HERE
#define LAKE_CT1_VOLTAGE     240.0   // <-- CHANGE HERE
#define LAKE_CT1_PF            0.90  // <-- CHANGE HERE
#define LAKE_CT2_RATIO        20.0   // <-- CHANGE HERE
#define LAKE_CT2_CAL_FACTOR    1.42  // <-- CHANGE HERE
#define LAKE_CT2_VOLTAGE     240.0   // <-- CHANGE HERE
#define LAKE_CT2_PF            0.85  // <-- CHANGE HERE

// ── PUMP HYSTERESIS THRESHOLDS ───────────────────────────────────────────
#define AC_ON_THRESHOLD_ON   0.55f   // <-- CHANGE HERE
#define AC_ON_THRESHOLD_OFF  0.45f   // <-- CHANGE HERE

// ── COP CONSTANTS — Lake House only ─────────────────────────────────────
#define GEO_FLOW_GPM   10.84f  // <-- CHANGE HERE if flow rate changes
#define GEO_DELTA_T_W   7.0f   // <-- CHANGE HERE if water deltaT changes

// ── WIFI & GPS — set at boot from LOCATION_PIN ───────────────────────────
#define PONDO_SSID      "William_Lynn-2.4"  // <-- CHANGE HERE
#define PONDO_PASS      "77330314"           // <-- CHANGE HERE
#define PONDO_LAT       43.1014f             // <-- CHANGE HERE
#define PONDO_LON      -85.5742f             // <-- CHANGE HERE

#define LAKE_SSID       "marknet"            // <-- CHANGE HERE
#define LAKE_PASS       "73duster"           // <-- CHANGE HERE
#define LAKE_LAT        43.1625f             // <-- CHANGE HERE
#define LAKE_LON       -85.5742f             // <-- CHANGE HERE

const char* WIFI_SSID     = PONDO_SSID;
const char* WIFI_PASSWORD = PONDO_PASS;
float LATITUDE            = PONDO_LAT;
float LONGITUDE           = PONDO_LON;

// ── HOME ASSISTANT ────────────────────────────────────────────────────────
#define HA_HOST              "pondovpn.duckdns.org"                        // <-- CHANGE HERE
#define HA_PORT              443                                            // <-- CHANGE HERE
#define HA_TOKEN             "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiI3OGVkNjZhZjgxZDQ0OWYwYjQzZWNiNWMzZWFhZjZlYyIsImlhdCI6MTc3NDkyNzYwOSwiZXhwIjoyMDkwMjg3NjA5fQ.Mwe5RnYMsy7U6lTCaa81sEG8tVj8-zRNNyHNN6n81FY"  // <-- CHANGE HERE
#define HA_POST_INTERVAL_SEC 1800                                          // <-- CHANGE HERE: seconds between HA posts (1800 = 30 min)

/*
===============================================================
  *** VERSION: HVACv38 ***

  HVACv38 - March 2026
  Freenove ESP32-S3 FNK0099
  Paired with ELKv28

  CHANGES FROM HVACv37:
  - Remove WiFi.config() DNS override in connectWiFi()
    WiFi.config() forced DNS to 8.8.8.8 which may be blocked by router/ISP
    Let DHCP assign the router's own DNS — fixes weather HTTP:-1 failure
  - Add [DNS] diagnostic print in fetchWeatherWork() to confirm resolution
  - No struct change — ELK reflash NOT required

  OLED (9 active lines — unchanged from v36):
      Line 1: Out 32F  08:15  v38
      Line 2: GTH:6700 5.9h 43% C:3
      Line 3: PMP: 967 6.4h 47% C:0
      Line 4: WTR:   0 0.5h  3% C:1  (Lake only, blank at Pondo)
      Line 5: G:$3.70 P:$1.13 W:$0.06
      Line 6: WX+x-x NT+x-x
      Line 7: WF+x-x EN+x WD:x  (ELK%/heap appended only if problem)
      Line 8: Total: $x.xx
      Line 9: error msg OR last restart time if no error

  HARDWARE: ESP32-S3 Freenove FNK0099, ADS1115, SH1107 128x128 OLED, RGB LED
  CT ch 0/1: HVAC/GEO load — WaterFurnace WPV36, 30A/1V CT, 240VAC
  CT ch 2/3: AC/PUMP load  — standalone condenser, 20A/1V CT, 240VAC
  LOCATION_PIN GPIO4: floating=PONDO, GND=Lake House
  Struct: 228 bytes — must match ELKv28 exactly
===============================================================
*/

#include <Wire.h>
#include <Adafruit_ADS1X15.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_sntp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <U8g2lib.h>
#include <FastLED.h>
#include <WiFiClientSecure.h>

#define RGB_LED_PIN 48
#define NUM_LEDS 1
CRGB leds[NUM_LEDS];

const char* NTP_SERVERS[] = {
  "216.239.35.0", "216.239.35.4", "129.6.15.28",
  "us.pool.ntp.org", "time.google.com"
};
const int NTP_SERVER_COUNT = 5;
const long GMT_OFFSET_SEC = -5 * 3600;
const int DAYLIGHT_OFFSET_SEC = 3600;
struct tm timeinfo;
bool timeValid = false;
unsigned long timeStartMillis = 0;
time_t timeStartEpoch = 0;

volatile bool lastSendOK = false;

#define I2C_SDA 8
#define I2C_SCL 9
U8G2_SH1107_SEEED_128X128_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

Adafruit_ADS1115 ads;

#define NUM_SAMPLES     40
#define CT_AVG_SAMPLES  10
#define THRESH_FAN_HEAT 140
#define THRESH_HEAT_FAN 100
#define THRESH_HI_HEAT  9999
#define STATE_IDLE    0
#define STATE_FAN     1
#define STATE_HEATING 2
#define STATE_HI_HEAT 3

// ================================================================
// STRUCT - 232 bytes - must match ELKv32 exactly
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
  int           gthCycleCount;   // NEW v36
  int           pumpCycleCount;  // NEW v36
  int           wtrCycleCount;   // NEW v36
  float         kwhRate;         // NEW v51 - pushed to ELK so ELK can compute $
} myData;
// struct size: 232 bytes - must match ELKv32 exactly

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int wdtResetCount = 0;
RTC_DATA_ATTR char lastRestartTime[20] = "No restart yet";
RTC_DATA_ATTR float rtcTotalKWh = 0.0;
RTC_DATA_ATTR unsigned long rtcRuntimeMS = 0;
RTC_DATA_ATTR float rtcAcKWh = 0.0;
RTC_DATA_ATTR unsigned long rtcAcRuntimeMS = 0;
RTC_DATA_ATTR float rtcWtrKWh = 0.0;
RTC_DATA_ATTR unsigned long rtcWtrRuntimeMS = 0;
RTC_DATA_ATTR time_t rtcLastDay = 0;
RTC_DATA_ATTR int  rtcGthCycles      = 0;   // NEW v36
RTC_DATA_ATTR int  rtcPumpCycles     = 0;   // NEW v36
RTC_DATA_ATTR int  rtcWtrCycles      = 0;   // NEW v36
RTC_DATA_ATTR bool rtcLastGeoRunning = false;  // persist across reboots for cycle detection
RTC_DATA_ATTR bool rtcLastAcOn       = false;
RTC_DATA_ATTR bool rtcLastWtrOn      = false;

unsigned long packetCount = 0;
int elkPacketsSent = 0;
int elkPacketsSuccess = 0;
unsigned long failCount = 0;
unsigned long successCount = 0;
float totalKWh = 0.0;
float acKWh = 0.0;
float wtrKWh = 0.0;
float acAmps = 0.0;
int acWatts = 0;
int wtrWatts = 0;
int pumpWatts = 0;
bool geoRunning = false;
bool acOn = false;
int furnaceState = STATE_IDLE;
int prevFurnaceState = STATE_IDLE;
int heatCycleCount = 0;
int acCycleCount = 0;
unsigned long lastReadingTime = 0;
unsigned long lastStateTime = 0;
unsigned long runtimeMStoday = 0;
unsigned long acRuntimeMStoday = 0;
unsigned long wtrRuntimeMStoday = 0;
float runtimeHoursToday = 0.0;
float acRuntimeHoursToday = 0.0;
float wtrRuntimeHoursToday = 0.0;
float runtimePercent = 0.0;
float acRuntimePercent = 0.0;
float wtrRuntimePercent = 0.0;
float outdoorTemp = 25.0;
time_t lastWeatherFetch = 0;
time_t nextWeatherCheck = 0;
time_t nextNTPCheck = 0;
int lastDDDay = -1;
int oledUpdateCounter = 0;
const int OLED_UPDATE_INTERVAL = 10;
bool sdCardOK = false;
bool ads1115OK = true;
bool espnowOK = true;
bool isLakeHouse = false;
int espnowFailCount = 0;
int espnowFailTotal = 0;
int ctBadReadCount = 0;
String currentLogFile = "";
unsigned long lastLogTime = 0;
const unsigned long LOG_INTERVAL = 60000;
int lastHourlyRecap = -1;
int rebootsSinceMidnight = 0;
bool midnightResetDone = false;
int startupWifiFailCount = 0;
bool systemReady = false;
int haPostCount  = 0;
int haFailCount  = 0;
time_t nextHAPost = 0;   // rate-limits HA posts independently of weather retries
int weatherFetchCount = 0;
int weatherFailCount = 0;
int ntpSyncCount = 0;
int ntpFailCount = 0;
int wifiFailCount = 0;
int wifiSuccessCount = 0;
char startupErrorMsg[40] = "None";
time_t lastNTPFailTime = 0;
time_t lastWiFiFailTime = 0;
time_t lastWXFailTime = 0;
float hddAccumulated = 0.0;
float cddAccumulated = 0.0;
float hoursOfData = 0.0;
float hddToday = 0.0;
float cddToday = 0.0;
const float LSB = 0.0000625;

// v51: kWh rate fetched from HA sensor.kwh_rate_now (Pi5 rate_helper is single source of truth)
float fetchedKwhRate = 0.20f;   // cached, default to winter flat
unsigned long lastRateFetchMs = 0;
bool rateFetchValid = false;

// ── Forward declarations ─────────────────────────────────────────────────
void updateOLEDStatus(const char* l1, const char* l2, const char* l3, const char* l4);
bool connectWiFi();
void disconnectWiFi();
bool syncNTPTime();
bool doWiFiSession(bool needNTP, bool needWeather);
bool initESPNow();
void updateDisplay(float current, int power, float outdoorTemp, float kWh, int state, bool wifiActive);
bool initSDCard();
String getLogFileName();
bool logToSD(float current, int power, float outdoorTemp, int state);
void appendSeasonSummary();
bool createLogFile();
void calculateNextWeatherCheck();
void calculateNextNTPCheck();
bool fetchWeatherWork();
float readCTCurrent();
float readACCurrent();
void restartAfterDelay(const char* errorMsg, int delayMinutes);
void haltWithError(const char* errorMsg);
void checkSystemErrors();
void yieldDelay(int totalMs);
void printHourlyRecap();
String formatFailTime(time_t t);
int wattsToState(int watts);
const char* stateLabel(int state);
void showWifiFailOLED();
void postToHA();
bool fetchCurrentRate();

void onDataSent(const wifi_tx_info_t* tx_info, esp_now_send_status_t status) {
  lastSendOK = (status == ESP_NOW_SEND_SUCCESS);
}

int wattsToState(int watts) {
  if (watts < THRESH_IDLE_W)   return STATE_IDLE;
  if (watts >= THRESH_HI_HEAT) return STATE_HI_HEAT;
  if (furnaceState == STATE_HEATING || furnaceState == STATE_HI_HEAT)
    return (watts < THRESH_HEAT_FAN) ? STATE_FAN : STATE_HEATING;
  else
    return (watts >= THRESH_FAN_HEAT) ? STATE_HEATING : STATE_FAN;
}

const char* stateLabel(int state) {
  switch (state) {
    case STATE_IDLE:    return "IDLE";
    case STATE_FAN:     return "FAN";
    case STATE_HEATING: return "HEATING";
    case STATE_HI_HEAT: return "HI";
    default:            return "UNKNOWN";
  }
}

void yieldDelay(int totalMs) {
  int remaining = totalMs;
  while (remaining > 0) {
    int chunk = (remaining > 100) ? 100 : remaining;
    yield(); delay(chunk); yield();
    remaining -= chunk;
  }
}

// v51: getKwhRate() deleted. Rate is now fetched from Pi5 via HA sensor.kwh_rate_now.
// Pi5 rate_helper.py + rate_config.json is single source of truth for both sites.
// See fetchCurrentRate() below.

// Fetch the current kWh rate from Home Assistant sensor.kwh_rate_now.
// Updates fetchedKwhRate on success. Leaves cached value on failure.
bool fetchCurrentRate() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  char url[110];
  snprintf(url, sizeof(url), "https://" HA_HOST "/api/states/sensor.kwh_rate_now");
  http.begin(client, url);
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  http.addHeader("Authorization", "Bearer " HA_TOKEN);
  yield();
  int code = http.GET();
  String body;
  if (code == 200) body = http.getString();
  http.end();
  yield();
  if (code != 200) {
    Serial.printf("[RATE] fetch failed HTTP %d, using cached %.2f\n", code, fetchedKwhRate);
    return false;
  }
  // Parse "state":"X.XX" out of JSON body
  int sIdx = body.indexOf("\"state\":\"");
  if (sIdx < 0) {
    Serial.printf("[RATE] parse failed (no state field), using cached %.2f\n", fetchedKwhRate);
    return false;
  }
  sIdx += 9;  // past the "\"state\":\"" prefix
  int eIdx = body.indexOf("\"", sIdx);
  if (eIdx < 0) {
    Serial.printf("[RATE] parse failed (no closing quote), using cached %.2f\n", fetchedKwhRate);
    return false;
  }
  String stateStr = body.substring(sIdx, eIdx);
  float parsed = stateStr.toFloat();
  if (parsed <= 0.0f || parsed > 1.0f) {
    Serial.printf("[RATE] parsed %.3f out of sane range, using cached %.2f\n", parsed, fetchedKwhRate);
    return false;
  }
  fetchedKwhRate = parsed;
  rateFetchValid = true;
  lastRateFetchMs = millis();
  Serial.printf("[RATE] fetched %.3f $/kWh from HA\n", fetchedKwhRate);
  return true;
}

float calcCOP(int watts) {
  if (watts < 100) return 0.0f;
  float btu_in    = watts * 3.412f;
  float btu_water = GEO_FLOW_GPM * 500.0f * GEO_DELTA_T_W;
  return (btu_in + btu_water) / btu_in;
}

void showWifiFailOLED() {
  char line1[22]; char line2[22]; char line3[22];
  snprintf(line1, sizeof(line1), "WiFi FAILED x%d", startupWifiFailCount);
  snprintf(line2, sizeof(line2), "Mode:%-14s", isLakeHouse ? "LAKE HOUSE" : "PONDO");
  snprintf(line3, sizeof(line3), "%.18s", WIFI_SSID);
  updateOLEDStatus(line1, line2, line3, "Check GPIO4 pin");
  leds[0] = CRGB::Orange; FastLED.show();
}

bool connectWiFi() {
  Serial.println("[WiFi] Connecting...");
  WiFi.persistent(false); WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    yield(); delay(500); yield(); Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] FAILED"); wifiFailCount++; return false;
  }
  delay(200); wifiSuccessCount++;
  Serial.print("[WiFi] OK RSSI:"); Serial.print(WiFi.RSSI());
  Serial.print(" IP:"); Serial.println(WiFi.localIP());
  return true;
}

void disconnectWiFi() {
  WiFi.disconnect(false); yieldDelay(200);
  WiFi.mode(WIFI_STA); yield();
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.println("[WiFi] Ch1 restored, disconnected");
}

void updateOLEDStatus(const char* l1, const char* l2, const char* l3, const char* l4) {
  yield(); u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_mf); int y = 10;
  if (l1 && l1[0]) { u8g2.setCursor(2, y); u8g2.print(l1); y += 11; }
  if (l2 && l2[0]) { u8g2.setCursor(2, y); u8g2.print(l2); y += 11; }
  if (l3 && l3[0]) { u8g2.setCursor(2, y); u8g2.print(l3); y += 11; }
  if (l4 && l4[0]) { u8g2.setCursor(2, y); u8g2.print(l4); }
  yield(); u8g2.sendBuffer();
}

void restartAfterDelay(const char* errorMsg, int delayMinutes) {
  Serial.print("[RESTART] "); Serial.println(errorMsg);
  strncpy(startupErrorMsg, errorMsg, 39); startupErrorMsg[39] = '\0';
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_mf);
  u8g2.setCursor(2, 10); u8g2.print("ERROR:");
  u8g2.setCursor(2, 21); u8g2.print(errorMsg);
  u8g2.setCursor(2, 32); u8g2.print("Restart in:");
  u8g2.setCursor(2, 43); u8g2.print(delayMinutes); u8g2.print(" min");
  u8g2.sendBuffer(); leds[0] = CRGB::Orange; FastLED.show();
  yieldDelay(delayMinutes * 60000); ESP.restart();
}

void haltWithError(const char* errorMsg) {
  Serial.print("[HALT] "); Serial.println(errorMsg);
  strncpy(startupErrorMsg, errorMsg, 39); startupErrorMsg[39] = '\0';
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_mf);
  u8g2.setCursor(2, 10); u8g2.print("** HALTED **");
  u8g2.setCursor(2, 21); u8g2.print(errorMsg);
  u8g2.setCursor(2, 32); u8g2.print("Check hardware");
  u8g2.setCursor(2, 43); u8g2.print("& restart MCU");
  u8g2.sendBuffer(); leds[0] = CRGB::Red; FastLED.show();
  while (true) { yield(); delay(100); }
}

String formatFailTime(time_t t) {
  if (t == 0) return "none";
  struct tm* ti = localtime(&t); char buf[8];
  strftime(buf, sizeof(buf), "%H:%M", ti); return String(buf);
}

void printHourlyRecap() {
  Serial.println("\n**************************************************");
  Serial.print("* NTP fail:  "); Serial.print(ntpFailCount);
  Serial.print("  WiFi fail: "); Serial.print(wifiFailCount);
  Serial.print("  WX fail:   "); Serial.println(weatherFailCount);
  Serial.print("* Reboots:   "); Serial.print(bootCount);
  Serial.print("  WDT:       "); Serial.println(wdtResetCount);
  Serial.printf("* GTH Cy:%d  PUMP Cy:%d  WTR Cy:%d\n", rtcGthCycles, rtcPumpCycles, rtcWtrCycles);
  Serial.printf("* HA Posts:%d  HA Fails:%d\n", haPostCount, haFailCount);
  Serial.printf("* Heap: %lu bytes\n", ESP.getFreeHeap());
  Serial.println("**************************************************\n");
}

bool syncNTPTime() {
  Serial.println("[NTP] Syncing...");
  char rssiStr[22]; sprintf(rssiStr, "WiFi: %d dBm", WiFi.RSSI());
  updateOLEDStatus(SKETCH_NAME, rssiStr, "NTP: Syncing...", "");
  for (int s = 0; s < NTP_SERVER_COUNT; s++) {
    Serial.print("[NTP] "); Serial.print(NTP_SERVERS[s]); Serial.print("... ");
    configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVERS[s], "", "");
    delay(2000); bool gotTime = false;
    for (int i = 0; i < 5; i++) {
      if (getLocalTime(&timeinfo, 4000)) { gotTime = true; break; }
      Serial.print("."); yield(); delay(1000);
    }
    if (gotTime) {
      char ts[25]; strftime(ts, sizeof(ts), "%m/%d/%Y %H:%M:%S", &timeinfo);
      Serial.print(" OK: "); Serial.println(ts);
      timeValid = true; ntpSyncCount++;
      char td[22]; strftime(td, sizeof(td), "NTP: %H:%M:%S", &timeinfo);
      updateOLEDStatus(SKETCH_NAME, rssiStr, td, "Synced!");
      timeStartMillis = millis(); timeStartEpoch = mktime(&timeinfo);
      esp_sntp_stop();  // stop background SNTP daemon — prevents interference with HTTP calls
      Serial.println("[NTP] SNTP daemon stopped");
      delay(1000); return true;
    }
    Serial.println(" timeout");
    if (s < NTP_SERVER_COUNT - 1) {
      disconnectWiFi();
      if (!connectWiFi()) { ntpFailCount++; return false; }
      sprintf(rssiStr, "WiFi: %d dBm", WiFi.RSSI());
    }
  }
  Serial.println("[NTP] FAILED"); ntpFailCount++; return false;
}

void calculateNextNTPCheck() {
  time_t now = time(NULL); struct tm ti; localtime_r(&now, &ti);
  ti.tm_hour = 3; ti.tm_min = 0; ti.tm_sec = 0;
  time_t next = mktime(&ti); if (next <= now) next += 86400;
  nextNTPCheck = next;
  char buf[20]; struct tm disp; localtime_r(&next, &disp);
  strftime(buf, sizeof(buf), "%m/%d %H:%M", &disp);
  Serial.print("[NTP] Next 3AM: "); Serial.println(buf);
}

void calculateNextWeatherCheck() {
  time_t now = time(NULL); time_t next = now + 60;
  struct tm ti; localtime_r(&next, &ti);
  int minsOver = ti.tm_min % 30; int secsOver = minsOver * 60 + ti.tm_sec;
  next = next - secsOver + 1800; if (next < now + 1800) next = now + 1800;
  nextWeatherCheck = next;
  char buf[20]; struct tm disp; localtime_r(&next, &disp);
  strftime(buf, sizeof(buf), "%H:%M", &disp);
  Serial.print("[WX] Next: "); Serial.println(buf);
}

bool fetchWeatherWork() {
  yieldDelay(100);
  char url[100];
  // wttr.in: plain HTTP, no API key, no TLS, reliable on ESP32
  // %%t in format string produces %t in URL = current temperature
  // &u = imperial (Fahrenheit)
  snprintf(url, sizeof(url),
    "http://wttr.in/%.4f,%.4f?format=%%t&u", LATITUDE, LONGITUDE);
  Serial.print("[WX] GET wttr.in ... ");
  WiFiClient wxClient; HTTPClient http;
  http.begin(wxClient, url);
  http.setConnectTimeout(8000); http.setTimeout(8000);
  http.addHeader("User-Agent", "ESP32-HVAC/1.0");
  IPAddress resolved; WiFi.hostByName("wttr.in", resolved);
  Serial.print("[DNS] wttr.in="); Serial.println(resolved.toString());
  yield();
  int httpCode = http.GET();
  Serial.println(httpCode);
  if (httpCode != 200) { http.end(); weatherFailCount++; return false; }
  yield();
  String payload = http.getString();
  yield(); http.end();
  payload.trim();
  // Expect "+32°F" or "-5°F" — toFloat() reads sign + digits, stops at °
  if (payload.length() < 2 ||
      !(isDigit(payload[0]) || payload[0] == '+' || payload[0] == '-')) {
    Serial.printf("[WX] Bad response: %s\n", payload.c_str());
    weatherFailCount++; return false;
  }
  float newTemp = payload.toFloat();
  Serial.printf("[WX] %.1fF (%s)\n", newTemp, payload.c_str());
  weatherFetchCount++;
  float hours, avgTemp;
  if (lastWeatherFetch > 0) {
    hours = (time(NULL) - lastWeatherFetch) / 3600.0;
    avgTemp = (outdoorTemp + newTemp) / 2.0;
  } else { hours = 0.5; avgTemp = newTemp; }
  if (avgTemp < 64.0) hddAccumulated += (65.0 - avgTemp) * hours;
  else if (avgTemp > 66.0) cddAccumulated += (avgTemp - 65.0) * hours;
  hoursOfData += hours;
  if (hoursOfData > 0) {
    hddToday = hddAccumulated / hoursOfData;
    cddToday = cddAccumulated / hoursOfData;
  }
  outdoorTemp = newTemp; lastWeatherFetch = time(NULL); yield(); return true;
}

// ── HOME ASSISTANT PUSH ───────────────────────────────────────────────────
static bool postOne(WiFiClientSecure& client, const char* entityId, const char* body) {
  HTTPClient http;
  char url[100];
  snprintf(url, sizeof(url), "https://" HA_HOST "/api/states/%s", entityId);
  http.begin(client, url);
  http.setConnectTimeout(6000);
  http.setTimeout(6000);
  http.addHeader("Authorization", "Bearer " HA_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.setReuse(true);
  yield();
  int code = http.POST((uint8_t*)body, strlen(body));
  http.end();
  yield();
  bool ok = (code == 200 || code == 201);
  Serial.printf("[HA] %-35s -> %d\n", entityId, code);
  return ok;
}

void postToHA() {
  if (!systemReady) { Serial.println("[HA] Skipped — startup not complete"); return; }
  Serial.println("[HA] Posting to Home Assistant...");
  WiFiClientSecure client;
  client.setInsecure();

  const char* pfx       = isLakeHouse ? "lake"  : "pondo";
  const char* locLabel  = isLakeHouse ? "Lake"  : "Pondo";
  float rate      = fetchedKwhRate;  // v51: from Pi5 via HA sensor.kwh_rate_now
  float totalCost = (totalKWh + acKWh + wtrKWh) * rate;
  char eid[48];
  char body[220];
  int ok = 0;

  // GTH kWh
  snprintf(eid,  sizeof(eid),  "sensor.%s_gth_kwh", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.3f\",\"attributes\":{\"unit_of_measurement\":\"kWh\","
    "\"device_class\":\"energy\",\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s\"}}",
    totalKWh, isLakeHouse ? "Lake GTH Energy" : "Pondo Furnace Blower Energy");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH power
  snprintf(eid,  sizeof(eid),  "sensor.%s_gth_power", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"W\","
    "\"device_class\":\"power\",\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s\"}}",
    myData.furnaceWatts, isLakeHouse ? "Lake GTH Power" : "Pondo Furnace Blower Power");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // PUMP kWh
  snprintf(eid,  sizeof(eid),  "sensor.%s_pump_kwh", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.3f\",\"attributes\":{\"unit_of_measurement\":\"kWh\","
    "\"device_class\":\"energy\",\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s\"}}",
    acKWh, isLakeHouse ? "Lake Pump Energy" : "Pondo AC Condenser Energy");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // PUMP power
  snprintf(eid,  sizeof(eid),  "sensor.%s_pump_power", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"W\","
    "\"device_class\":\"power\",\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s\"}}",
    acWatts, isLakeHouse ? "Lake Pump Power" : "Pondo AC Condenser Power");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // WTR kWh — Lake only
  if (isLakeHouse) {
    snprintf(body, sizeof(body),
      "{\"state\":\"%.3f\",\"attributes\":{\"unit_of_measurement\":\"kWh\","
      "\"device_class\":\"energy\",\"state_class\":\"total_increasing\","
      "\"friendly_name\":\"Lake WTR Energy\"}}",
      wtrKWh);
    postOne(client, "sensor.lake_wtr_kwh", body) ? ok++ : haFailCount++;
  }

  // Outdoor temp  (plain ASCII F - HA renders as "F", no UTF-8 degree symbol)
  snprintf(eid,  sizeof(eid),  "sensor.%s_outdoor_temp", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.1f\",\"attributes\":{\"unit_of_measurement\":\"F\","
    "\"device_class\":\"temperature\",\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s Outdoor Temp\"}}",
    outdoorTemp, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // Total cost
  snprintf(eid,  sizeof(eid),  "sensor.%s_total_cost", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.2f\",\"attributes\":{\"unit_of_measurement\":\"USD\","
    "\"device_class\":\"monetary\",\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s Total Cost Today\"}}",
    totalCost, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // Status string
  snprintf(eid,  sizeof(eid),  "sensor.%s_status", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"%s\"}}",
    myData.status, isLakeHouse ? "Lake HVAC Status" : "Pondo HVAC Status");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH cycle count
  snprintf(eid,  sizeof(eid),  "sensor.%s_gth_cycles", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"cycles\","
    "\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s\"}}",
    rtcGthCycles, isLakeHouse ? "Lake GTH Cycles Today" : "Pondo Furnace Blower Cycles Today");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH runtime %  (%% in format string = literal % in output)
  snprintf(eid,  sizeof(eid),  "sensor.%s_runtime_pct", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.1f\",\"attributes\":{\"unit_of_measurement\":\"%%\","
    "\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s\"}}",
    runtimePercent, isLakeHouse ? "Lake GTH Runtime %" : "Pondo Furnace Blower Runtime %");
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // COP — Lake only
  if (isLakeHouse) {
    float cop = calcCOP(myData.furnaceWatts);
    snprintf(body, sizeof(body),
      "{\"state\":\"%.2f\",\"attributes\":{\"unit_of_measurement\":\"COP\","
      "\"state_class\":\"measurement\","
      "\"friendly_name\":\"Lake Geothermal COP\"}}",
      cop);
    postOne(client, "sensor.lake_cop", body) ? ok++ : haFailCount++;
  }

  haPostCount++;
  int total = isLakeHouse ? 11 : 9;
  Serial.printf("[HA] Session #%d: %d/%d OK  total fails: %d\n",
    haPostCount, ok, total, haFailCount);
}

bool doWiFiSession(bool needNTP, bool needWeather) {
  if (!needNTP && !needWeather) return true;
  Serial.println("\n[WiFi] Session...");
  if (!connectWiFi()) {
    startupWifiFailCount++;
    if (startupWifiFailCount >= 2) showWifiFailOLED();
    if (needNTP)     { nextNTPCheck     = time(NULL) + 300; ntpFailCount++; }
    if (needWeather) { nextWeatherCheck = time(NULL) + 300; weatherFailCount++; }
    return false;
  }
  startupWifiFailCount = 0;
  // v51: fetch current kWh rate from Pi5 (via HA sensor.kwh_rate_now) once per WiFi session
  fetchCurrentRate();
  bool ntpOK = true;
  if (needNTP) {
    ntpOK = syncNTPTime();
    if (ntpOK) calculateNextNTPCheck(); else nextNTPCheck = time(NULL) + 300;
  }
  if (needWeather && timeValid) {
    if (needNTP) yieldDelay(500);
    bool wxOK = fetchWeatherWork();
    if (wxOK) calculateNextWeatherCheck(); else nextWeatherCheck = time(NULL) + 300;
  }
  time_t now_t = time(NULL);
  if (timeValid && now_t >= nextHAPost) {
    postToHA();
    // v63 fix: only schedule next POST if this one actually ran (systemReady).
    // Otherwise a Skipped post would lock out the next 30 min of HA updates after boot.
    if (systemReady) nextHAPost = now_t + HA_POST_INTERVAL_SEC;
  }
  disconnectWiFi(); Serial.println("[WiFi] Session done"); return ntpOK;
}

bool initESPNow() {
  WiFi.mode(WIFI_STA); yieldDelay(100);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  Serial.println("[ESP-NOW] Ch1"); yieldDelay(50);
  if (esp_now_init() != ESP_OK) { Serial.println("[ESP-NOW] Init fail"); return false; }
  yieldDelay(100); esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  peer.channel = 1; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
  memcpy(peer.peer_addr, elkMAC, 6);
  if (esp_now_add_peer(&peer) != ESP_OK) { Serial.println("[ESP-NOW] Peer fail"); return false; }
  Serial.printf("[ESP-NOW] ELK: %02X:%02X:%02X:%02X:%02X:%02X\n",
    elkMAC[0], elkMAC[1], elkMAC[2], elkMAC[3], elkMAC[4], elkMAC[5]);
  return true;
}

float readCTCurrent() {
  float sum = 0.0; int valid = 0;
  for (int j = 0; j < CT_AVG_SAMPLES; j++) {
    long sumSq = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
      if (i % 30 == 0) yield();
      int16_t s = ads.readADC_Differential_0_1(); sumSq += (long)s * s;
    }
    float rms = sqrt((float)sumSq / NUM_SAMPLES);
    if (!isnan(rms) && rms >= 0) {
      float amps = rms * LSB * CT1_RATIO * CT1_CAL_FACTOR;
      sum += (!isnan(amps) && amps >= 0.05) ? amps : 0.0; valid++;
    }
    delay(10);
  }
  return (valid > 0) ? (sum / valid) : 0.0;
}

float readACCurrent() {
  float sum = 0.0; int valid = 0;
  for (int j = 0; j < CT_AVG_SAMPLES; j++) {
    long sumSq = 0;
    for (int i = 0; i < NUM_SAMPLES; i++) {
      if (i % 30 == 0) yield();
      int16_t s = ads.readADC_Differential_2_3(); sumSq += (long)s * s;
    }
    float rms = sqrt((float)sumSq / NUM_SAMPLES);
    if (!isnan(rms) && rms >= 0) {
      float amps = rms * LSB * CT2_RATIO * CT2_CAL_FACTOR;
      sum += (!isnan(amps) && amps >= 0.10) ? amps : 0.0; valid++;
    }
    delay(10);
  }
  return (valid > 0) ? (sum / valid) : 0.0;
}

void checkSystemErrors() {
  myData.systemError = false;
  strncpy(myData.errorMsg, "OK", sizeof(myData.errorMsg));
  myData.elkSuccess = elkPacketsSent > 0 ? (elkPacketsSuccess * 100) / elkPacketsSent : 0;
  if (!espnowOK)            { myData.systemError = true; strncpy(myData.errorMsg, "ERR:ESPNOW", sizeof(myData.errorMsg)); return; }
  if (!ads1115OK)           { myData.systemError = true; strncpy(myData.errorMsg, "ERR:CT",     sizeof(myData.errorMsg)); return; }
  if (!timeValid)           { myData.systemError = true; strncpy(myData.errorMsg, "ERR:NTP",    sizeof(myData.errorMsg)); return; }
  if (wifiFailCount >= 3)   { myData.systemError = true; strncpy(myData.errorMsg, "ERR:WIFI",   sizeof(myData.errorMsg)); return; }
  if (weatherFailCount >= 3){ myData.systemError = true; strncpy(myData.errorMsg, "ERR:WX",     sizeof(myData.errorMsg)); return; }
  uint32_t heap = ESP.getFreeHeap();
  if (heap < HEAP_WARN_THRESHOLD) {
    myData.systemError = true;
    char hm[30]; snprintf(hm, sizeof(hm), "LOW HEAP:%luK", heap / 1000);
    strncpy(myData.errorMsg, hm, sizeof(myData.errorMsg) - 1);
    myData.errorMsg[sizeof(myData.errorMsg) - 1] = '\0';
  }
}

// ── OLED display update ───────────────────────────────────────────────────
void updateDisplay(float current, int power, float outdoorTemp,
                   float kWh, int state, bool wifiActive) {
  yield(); u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_mf);
  char buf[22];
  float rate = fetchedKwhRate;  // v51: from Pi5 via HA sensor.kwh_rate_now

  // Line 1 (y=11): Out 32F  08:15  vXX (from SKETCH_NAME)
  u8g2.setCursor(2, 11);
  u8g2.print("Out "); u8g2.print((int)outdoorTemp); u8g2.print("F");
  u8g2.setCursor(46, 11);
  if (timeValid && getLocalTime(&timeinfo, 100)) {
    char ts[6]; strftime(ts, sizeof(ts), "%H:%M", &timeinfo); u8g2.print(ts);
  } else u8g2.print("--:--");
  u8g2.setCursor(75, 11); u8g2.print(" "); u8g2.print(SKETCH_NAME + 4);

  // Line 2 (y=22): static circuit identity label, tight format to fit cycle digits.
  //   Pondo CT0/1 = furnace blower+gas -> "FAN"
  //   Lake  CT0/1 = WPV36 geo compressor -> "GTH"
  //   State info lives in myData.status / HA sensor.
  u8g2.setCursor(2, 22);
  const char* line2_label = isLakeHouse ? "GTH" : "FURN";
  snprintf(buf, sizeof(buf), "%s:%4d %2dh %2d%%C:%d",
    line2_label, power, (int)runtimeHoursToday, (int)runtimePercent, rtcGthCycles);
  buf[21] = 0; u8g2.print(buf);

  // Line 3 (y=33): static circuit identity label.
  //   Pondo CT2/3 = AC condenser -> "AC "
  //   Lake  CT2/3 = Grundfos well pump -> "PMP"
  u8g2.setCursor(2, 33);
  const char* line3_label = isLakeHouse ? "PMP" : "AC ";
  snprintf(buf, sizeof(buf), "%s:%4d %2dh %2d%%C:%d",
    line3_label, acWatts, (int)acRuntimeHoursToday, (int)acRuntimePercent, rtcPumpCycles);
  buf[21] = 0; u8g2.print(buf);

  // Line 4 (y=44): WTR at Lake (water heater est), blank at Pondo
  u8g2.setCursor(2, 44);
  if (isLakeHouse) {
    snprintf(buf, sizeof(buf), "WTR:%4d %2dh %2d%%C:%d",
      wtrWatts, (int)wtrRuntimeHoursToday, (int)wtrRuntimePercent, rtcWtrCycles);
    buf[21] = 0; u8g2.print(buf);
  } else {
    u8g2.print("                     ");
  }

  // Line 5 (y=55): G:$3.70 P:$1.13 W:$0.06  (Lake) or FAN:$x.xx  AC:$x.xx  (Pondo)
  u8g2.setCursor(2, 55);
  if (isLakeHouse) {
    snprintf(buf, sizeof(buf), "G:$%.2f P:$%.2f W:$%.2f",
      kWh * rate, acKWh * rate, wtrKWh * rate);
  } else {
    snprintf(buf, sizeof(buf), "FAN:$%.2f  AC:$%.2f",
      kWh * rate, acKWh * rate);
  }
  buf[21] = 0; u8g2.print(buf);

  // Line 6 (y=66): WX+27-0 NT+1-0
  u8g2.setCursor(2, 66);
  snprintf(buf, sizeof(buf), "WX+%d-%d NT+%d-%d",
    weatherFetchCount, weatherFailCount, ntpSyncCount, ntpFailCount);
  buf[21] = 0; u8g2.print(buf);

  // Line 7 (y=77): WF+28-0 EN-57 WD:0  (ELK%/heap appended only if problem)
  u8g2.setCursor(2, 77);
  bool heapBad = (ESP.getFreeHeap() < HEAP_WARN_THRESHOLD);
  bool elkBad  = (myData.elkSuccess < 80 && elkPacketsSent > 10);
  if (heapBad || elkBad) {
    snprintf(buf, sizeof(buf), "WF+%d-%d EN-%d ELK:%d%%",
      wifiSuccessCount, wifiFailCount, espnowFailTotal, myData.elkSuccess);
  } else {
    snprintf(buf, sizeof(buf), "WF+%d-%d EN-%d WD:%d",
      wifiSuccessCount, wifiFailCount, espnowFailTotal, wdtResetCount);
  }
  buf[21] = 0; u8g2.print(buf);

  // Line 8 (y=88): Total: $4.89
  u8g2.setCursor(2, 88);
  float totalCost = (kWh + acKWh + wtrKWh) * rate;
  snprintf(buf, sizeof(buf), "Total: $%.2f", totalCost);
  buf[21] = 0; u8g2.print(buf);

  // Line 9 (y=99): error msg OR last restart time if no error
  u8g2.setCursor(2, 99);
  if (myData.systemError) {
    snprintf(buf, sizeof(buf), "%-21s", myData.errorMsg);
  } else {
    snprintf(buf, sizeof(buf), "%-21s", lastRestartTime);
  }
  buf[21] = 0; u8g2.print(buf);

  // Lines 10-11: blank
  u8g2.setCursor(2, 110); u8g2.print("                     ");
  u8g2.setCursor(2, 121); u8g2.print("                     ");

  yield(); u8g2.sendBuffer();
}

// ================================================================
// SETUP
// ================================================================
void setup() {
  Serial.begin(115200); yield(); yieldDelay(2000);
  bootCount++;
  esp_reset_reason_t reason = esp_reset_reason();
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) {
    wdtResetCount++;
    Serial.printf("[BOOT] WDT reset #%d\n", wdtResetCount);
    strncpy(startupErrorMsg, "WDT Reset at boot", 39);
  }
  Serial.print("[BOOT] Boot #"); Serial.println(bootCount);

  FastLED.addLeds<WS2812, RGB_LED_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(5); leds[0] = CRGB::Black; FastLED.show();

  pinMode(LOCATION_PIN, INPUT_PULLUP); delay(10);
  isLakeHouse = (digitalRead(LOCATION_PIN) == LOW);

  if (isLakeHouse) {
    memcpy(elkMAC, lakeMAC, 6);
    CT1_RATIO = LAKE_CT1_RATIO; CT1_CAL_FACTOR = LAKE_CT1_CAL_FACTOR;
    CT1_VOLTAGE = LAKE_CT1_VOLTAGE; CT1_PF = LAKE_CT1_PF;
    CT2_RATIO = LAKE_CT2_RATIO; CT2_CAL_FACTOR = LAKE_CT2_CAL_FACTOR;
    CT2_VOLTAGE = LAKE_CT2_VOLTAGE; CT2_PF = LAKE_CT2_PF;
    WIFI_SSID = LAKE_SSID; WIFI_PASSWORD = LAKE_PASS;
    LATITUDE = LAKE_LAT; LONGITUDE = LAKE_LON;
  } else {
    memcpy(elkMAC, pondoMAC, 6);
    CT1_RATIO = PONDO_CT1_RATIO; CT1_CAL_FACTOR = PONDO_CT1_CAL_FACTOR;
    CT1_VOLTAGE = PONDO_CT1_VOLTAGE; CT1_PF = PONDO_CT1_PF;
    CT2_RATIO = PONDO_CT2_RATIO; CT2_CAL_FACTOR = PONDO_CT2_CAL_FACTOR;
    CT2_VOLTAGE = PONDO_CT2_VOLTAGE; CT2_PF = PONDO_CT2_PF;
    WIFI_SSID = PONDO_SSID; WIFI_PASSWORD = PONDO_PASS;
    LATITUDE = PONDO_LAT; LONGITUDE = PONDO_LON;
  }

  Serial.print("[LOC] "); Serial.println(isLakeHouse ? "LAKE HOUSE" : "PONDO");
  Serial.printf("[MAC] ELK: %02X:%02X:%02X:%02X:%02X:%02X\n",
    elkMAC[0], elkMAC[1], elkMAC[2], elkMAC[3], elkMAC[4], elkMAC[5]);
  Serial.printf("[NET] SSID:%s  Lat:%.4f Lon:%.4f\n", WIFI_SSID, LATITUDE, LONGITUDE);

  Serial.println("\n===============================================");
  Serial.print("   "); Serial.print(SKETCH_NAME);
  Serial.println(isLakeHouse ? " - LAKE HOUSE" : " - PONDO");
  Serial.println("   Paired with " PAIRED_WITH);
  Serial.println("   HVAC/GEO ch0/1  AC/PUMP ch2/3");
  Serial.println("   All settings auto-selected from LOCATION_PIN GPIO4");
  Serial.println("   Struct: 228 bytes — must match ELKv28 exactly");
  if (isLakeHouse)
    Serial.printf("   COP: GPM=%.2f deltaT=%.1fF\n", GEO_FLOW_GPM, GEO_DELTA_T_W);
  Serial.println("===============================================\n");

  Wire.begin(I2C_SDA, I2C_SCL); yieldDelay(50);
  u8g2.begin();
  u8g2.clearBuffer(); u8g2.setFont(u8g2_font_6x12_mf);
  u8g2.setCursor(0, 10); u8g2.print(SKETCH_NAME);
  u8g2.setCursor(0, 21); u8g2.print(isLakeHouse ? "Lake House" : "PONDO");
  u8g2.sendBuffer(); yieldDelay(1000);

  // ── Phase 1: NTP (required) ───────────────────────────────────────────
  int startupAttempts = 0; const int MAX_STARTUP_ATTEMPTS = 5;
  while (true) {
    bool ntpOK = doWiFiSession(true, false);  // NTP only — SNTP stopped inside on success
    if (ntpOK && timeValid) {
      startupWifiFailCount = 0;
      if (getLocalTime(&timeinfo)) {
        lastDDDay = timeinfo.tm_mday;
        strftime(lastRestartTime, sizeof(lastRestartTime), "%m/%d %H:%M:%S", &timeinfo);
        Serial.print("[BOOT] "); Serial.println(lastRestartTime);
      }
      break;
    }
    startupAttempts++;
    Serial.printf("[STARTUP] NTP attempt %d/%d failed\n", startupAttempts, MAX_STARTUP_ATTEMPTS);
    if (startupAttempts >= MAX_STARTUP_ATTEMPTS) {
      strncpy(startupErrorMsg, "No NTP at boot", 39);
      Serial.println("[STARTUP] Degraded mode — no NTP"); break;
    }
    yieldDelay(30000);
  }

  // ── Phase 2: Weather (non-blocking — loop retries if this fails) ──────
  if (timeValid) {
    systemReady = true;  // v63 fix: enable HA POST in this WiFi session so first POST after every boot fires
    Serial.println("[STARTUP] NTP done — fetching weather on fresh connection...");
    yieldDelay(500);
    doWiFiSession(false, true);
  }
  yieldDelay(500);

  if (!ads.begin(0x48)) haltWithError("ADS1115 Not Found");
  ads.setGain(GAIN_TWO);
  Serial.println("[ADS] GAIN_TWO");
  Serial.printf("[CAL] CT1 ratio=%.1f cal=%.3f V=%.0f PF=%.2f\n", CT1_RATIO, CT1_CAL_FACTOR, CT1_VOLTAGE, CT1_PF);
  Serial.printf("[CAL] CT2 ratio=%.1f cal=%.3f V=%.0f PF=%.2f\n", CT2_RATIO, CT2_CAL_FACTOR, CT2_VOLTAGE, CT2_PF);
  Serial.printf("[GEO] geoRunning threshold: power > %dW\n", THRESH_IDLE_W);
  Serial.printf("[PMP] acOn ON=%.2fA OFF=%.2fA (hysteresis)\n", AC_ON_THRESHOLD_ON, AC_ON_THRESHOLD_OFF);
  if (isLakeHouse)
    Serial.printf("[COP] GPM=%.2f deltaT=%.1fF\n", GEO_FLOW_GPM, GEO_DELTA_T_W);

  sdCardOK = initSDCard();
  if (!initESPNow()) haltWithError("ESP-NOW Failed");

  leds[0] = CRGB::Green; FastLED.show();
  Serial.println("[OK] Setup complete");
  Serial.println("===============================================\n");

  updateDisplay(0.0, 0, outdoorTemp, 0.0, STATE_IDLE, false);
  lastReadingTime = millis();

  if (timeValid && getLocalTime(&timeinfo)) {
    struct tm midnight = timeinfo;
    midnight.tm_hour = 0; midnight.tm_min = 0; midnight.tm_sec = 0;
    time_t todayMidnight = mktime(&midnight);
    if (rtcLastDay == todayMidnight) {
      totalKWh = rtcTotalKWh; runtimeMStoday = rtcRuntimeMS;
      acKWh = rtcAcKWh; acRuntimeMStoday = rtcAcRuntimeMS;
      wtrKWh = rtcWtrKWh; wtrRuntimeMStoday = rtcWtrRuntimeMS;
      rebootsSinceMidnight++;
      Serial.printf("[BOOT] Restored GEO:%.4f PUMP:%.4f WATR:%.4f GTHcy:%d PMPcy:%d WTRcy:%d\n",
        totalKWh, acKWh, wtrKWh, rtcGthCycles, rtcPumpCycles, rtcWtrCycles);
    } else {
      rtcTotalKWh = 0; rtcRuntimeMS = 0; rtcAcKWh = 0; rtcAcRuntimeMS = 0;
      rtcWtrKWh = 0; rtcWtrRuntimeMS = 0;
      rtcGthCycles = 0; rtcPumpCycles = 0; rtcWtrCycles = 0;
      rtcLastDay = todayMidnight; rebootsSinceMidnight = 0;
      Serial.println("[BOOT] New day - RTC reset");
    }
  }
  systemReady = true;
}

// ================================================================
// LOOP
// ================================================================
void loop() {
  unsigned long now = millis();

  if (lastStateTime > 0) {
    if (geoRunning)          runtimeMStoday    += (now - lastStateTime);
    if (acOn)                acRuntimeMStoday  += (now - lastStateTime);
    if (acOn && !geoRunning) wtrRuntimeMStoday += (now - lastStateTime);
  }
  lastStateTime = now;
  runtimeHoursToday    = runtimeMStoday    / 3600000.0;
  acRuntimeHoursToday  = acRuntimeMStoday  / 3600000.0;
  wtrRuntimeHoursToday = wtrRuntimeMStoday / 3600000.0;

  yield();

  if (timeValid && getLocalTime(&timeinfo, 100)) {
    int min = (timeinfo.tm_hour * 60) + timeinfo.tm_min;
    if (min > 0) {
      runtimePercent    = (runtimeHoursToday    / (min / 60.0)) * 100.0;
      acRuntimePercent  = (acRuntimeHoursToday  / (min / 60.0)) * 100.0;
      wtrRuntimePercent = (wtrRuntimeHoursToday / (min / 60.0)) * 100.0;
    }
  }

  yield();

  if (timeValid && getLocalTime(&timeinfo, 100)) {
    int currentDay = timeinfo.tm_mday;
    if (currentDay != lastDDDay) {
      Serial.println("[MIDNIGHT] Daily reset starting...");
      if (lastDDDay != -1) appendSeasonSummary();
      yield();
      hddAccumulated = 0; cddAccumulated = 0; hddToday = 0; cddToday = 0; hoursOfData = 0;
      yield();
      totalKWh = 0; acKWh = 0; wtrKWh = 0;
      yield();
      runtimeMStoday = 0; acRuntimeMStoday = 0; wtrRuntimeMStoday = 0;
      runtimeHoursToday = 0; runtimePercent = 0;
      acRuntimeHoursToday = 0; acRuntimePercent = 0;
      wtrRuntimeHoursToday = 0; wtrRuntimePercent = 0;
      yield();
      struct tm nm = timeinfo; nm.tm_hour = 0; nm.tm_min = 0; nm.tm_sec = 0;
      rtcTotalKWh = 0; rtcRuntimeMS = 0; rtcAcKWh = 0; rtcAcRuntimeMS = 0;
      rtcWtrKWh = 0; rtcWtrRuntimeMS = 0;
      rtcGthCycles = 0; rtcPumpCycles = 0; rtcWtrCycles = 0;  // reset cycles at midnight
      rtcLastGeoRunning = false; rtcLastAcOn = false; rtcLastWtrOn = false;
      rtcLastDay = mktime(&nm); rebootsSinceMidnight = 0;
      yield();
      packetCount = 0; elkPacketsSent = 0; elkPacketsSuccess = 0;
      yield();
      ntpFailCount = 0; weatherFailCount = 0; wifiFailCount = 0; wifiSuccessCount = 0;
      ntpSyncCount = 0; weatherFetchCount = 0; haPostCount = 0; haFailCount = 0;
      nextHAPost = 0;
      yield();
      heatCycleCount = 0; acCycleCount = 0;
      lastNTPFailTime = 0; lastWiFiFailTime = 0; lastWXFailTime = 0;
      lastHourlyRecap = -1; lastDDDay = currentDay;
      yield();
      midnightResetDone = true;
      Serial.println("[MIDNIGHT] Daily reset complete — WiFi deferred one loop");
    }
    int recapHour = timeinfo.tm_hour;
    if (timeinfo.tm_min == 0 && timeinfo.tm_sec < 3 && recapHour != lastHourlyRecap) {
      printHourlyRecap(); lastHourlyRecap = recapHour;
    }
  }

  time_t ct = time(NULL);
  bool needNTP     = !timeValid || (timeValid && ct >= nextNTPCheck);
  bool needWeather = timeValid && (ct >= nextWeatherCheck);
  bool midnightBlock = timeValid && getLocalTime(&timeinfo, 100) &&
                       (timeinfo.tm_hour == 23 && timeinfo.tm_min >= 30);
  if (midnightBlock) {
    needWeather = false;
    if (needNTP) { nextNTPCheck = time(NULL) + 300; needNTP = false; }
  }
  if (midnightResetDone) {
    midnightResetDone = false; needNTP = false; needWeather = false;
    Serial.println("[MIDNIGHT] WiFi deferred — will run next loop");
  }
  if (needNTP || needWeather) doWiFiSession(needNTP, needWeather);

  float current = readCTCurrent();
  acAmps = readACCurrent(); if (isnan(acAmps)) acAmps = 0.0;

  pumpWatts = (int)(acAmps * CT2_VOLTAGE * CT2_PF);
  acWatts   = pumpWatts;

  if (!acOn && acAmps >= AC_ON_THRESHOLD_ON)  acOn = true;
  if ( acOn && acAmps <  AC_ON_THRESHOLD_OFF) acOn = false;

  static int consecutiveBadReads = 0;
  if (isnan(current)) {
    consecutiveBadReads++; ctBadReadCount++;
    if (consecutiveBadReads >= 3) { ads1115OK = false; Serial.println("[ERROR] ADS1115!"); }
  } else { consecutiveBadReads = 0; ads1115OK = true; }

  int power = 0;
  if (!isnan(current)) {
    float pf = current * CT1_VOLTAGE * CT1_PF;
    if (!isnan(pf) && pf >= 0 && pf < 100000) power = (int)pf;
  }

  prevFurnaceState = furnaceState;
  furnaceState     = wattsToState(power);
  geoRunning       = (power > THRESH_IDLE_W);
  wtrWatts         = (!geoRunning && acOn) ? pumpWatts : 0;  // WTR only when pump is standalone

  // ── Cycle count logic — track OFF->ON transitions ────────────────────
  bool wtrOn = (acOn && !geoRunning);

  if (geoRunning && !rtcLastGeoRunning) { rtcGthCycles++;  Serial.printf("[CYCLE] GTH  #%d\n", rtcGthCycles); }
  if (acOn       && !rtcLastAcOn)       { rtcPumpCycles++; Serial.printf("[CYCLE] PUMP #%d\n", rtcPumpCycles); }
  if (wtrOn      && !rtcLastWtrOn)      { rtcWtrCycles++;  Serial.printf("[CYCLE] WTR  #%d\n", rtcWtrCycles); }
  rtcLastGeoRunning = geoRunning;
  rtcLastAcOn       = acOn;
  rtcLastWtrOn      = wtrOn;
  // ─────────────────────────────────────────────────────────────────────

  if (furnaceState != prevFurnaceState) {
    Serial.print("[STATE] "); Serial.print(stateLabel(prevFurnaceState));
    Serial.print(" -> ");     Serial.print(stateLabel(furnaceState));
    Serial.print(" power=");  Serial.print(power);
    Serial.print("W geoRunning="); Serial.println(geoRunning ? "YES" : "NO");
    updateDisplay(current, power, outdoorTemp, totalKWh, furnaceState, false);
    oledUpdateCounter = 0;
  }

  float elapsed_h = (now - lastReadingTime) / 3600000.0;
  totalKWh += (power / 1000.0) * elapsed_h;
  if (geoRunning)
    acKWh  += (pumpWatts / 1000.0) * elapsed_h;
  else if (acOn && isLakeHouse)              // Lake: standalone pump = water heater
    wtrKWh += (pumpWatts / 1000.0) * elapsed_h;
  else if (acOn)                             // Pondo: standalone CT2 = AC condenser
    acKWh  += (pumpWatts / 1000.0) * elapsed_h;
  lastReadingTime = now;

  rtcTotalKWh = totalKWh; rtcRuntimeMS    = runtimeMStoday;
  rtcAcKWh    = acKWh;    rtcAcRuntimeMS  = acRuntimeMStoday;
  rtcWtrKWh   = wtrKWh;   rtcWtrRuntimeMS = wtrRuntimeMStoday;

  myData.currentAmps         = isnan(current) ? 0.0 : current;
  myData.furnaceWatts        = power;
  myData.furnaceKWh          = isnan(totalKWh) ? 0.0 : totalKWh;
  myData.heatingState        = furnaceState;
  myData.outdoorTemp         = outdoorTemp;
  myData.hddToday            = hddToday;
  myData.cddToday            = cddToday;
  myData.runtimeHours        = runtimeHoursToday;
  myData.runtimePercent      = runtimePercent;
  strncpy(myData.lastRestartTime, lastRestartTime, sizeof(myData.lastRestartTime) - 1);
  myData.lastRestartTime[sizeof(myData.lastRestartTime) - 1] = '\0';
  myData.wifiSuccessCount    = wifiSuccessCount;
  myData.wifiFailCount       = wifiFailCount;
  myData.weatherSuccessCount = weatherFetchCount;
  myData.weatherFailCount    = weatherFailCount;
  myData.ntpSuccessCount     = ntpSyncCount;
  myData.ntpFailCount        = ntpFailCount;
  myData.wdtResetCount       = wdtResetCount;
  myData.acAmps              = acAmps;
  myData.acWatts             = acWatts;
  myData.acKWh               = isnan(acKWh)  ? 0.0 : acKWh;
  myData.acOn                = acOn;
  myData.acRuntimeHours      = acRuntimeHoursToday;
  myData.acRuntimePercent    = acRuntimePercent;
  myData.freeHeap            = ESP.getFreeHeap();
  myData.wtrAmps             = (wtrWatts > 0) ? acAmps : 0.0f;
  myData.wtrWatts            = wtrWatts;
  myData.wtrKWh              = isnan(wtrKWh) ? 0.0 : wtrKWh;
  myData.wtrRuntimeHours     = wtrRuntimeHoursToday;
  myData.wtrRuntimePercent   = wtrRuntimePercent;
  myData.isLakeHouse         = isLakeHouse;
  myData.espnowFailTotal     = espnowFailTotal;
  myData.gthCycleCount       = rtcGthCycles;   // NEW v36
  myData.pumpCycleCount      = rtcPumpCycles;  // NEW v36
  myData.wtrCycleCount       = rtcWtrCycles;   // NEW v36
  myData.kwhRate             = fetchedKwhRate; // NEW v51 - push rate to ELK

  const char* geoLabel;
  if (isLakeHouse) {
    if      (geoRunning) geoLabel = "PUMP";
    else if (acOn)       geoLabel = "WTR";
    else                 geoLabel = stateLabel(furnaceState);
  } else {
    if      (geoRunning) geoLabel = stateLabel(furnaceState);
    else if (acOn)       geoLabel = "AC";
    else                 geoLabel = stateLabel(furnaceState);
  }
  strncpy(myData.status, geoLabel, sizeof(myData.status) - 1);
  myData.status[sizeof(myData.status) - 1] = '\0';

  if (timeValid) {
    unsigned long elapsedSec = (millis() - timeStartMillis) / 1000;
    time_t epoch = timeStartEpoch + elapsedSec; struct tm* t = localtime(&epoch);
    int h = t->tm_hour; const char* ap = (h >= 12) ? "PM" : "AM";
    if (h > 12) h -= 12; if (h == 0) h = 12;
    snprintf(myData.timeStr, sizeof(myData.timeStr), "%02d/%02d %2d:%02d%s",
      t->tm_mon + 1, t->tm_mday, h, t->tm_min, ap);
  } else strcpy(myData.timeStr, "--:--");

  checkSystemErrors();

  myData.packetNum = elkPacketsSent;
  elkPacketsSent++; lastSendOK = false;
  esp_err_t r = esp_now_send(elkMAC, (uint8_t*)&myData, sizeof(myData));
  yield(); yieldDelay(50);
  if (r == ESP_OK && lastSendOK) {
    elkPacketsSuccess++; espnowFailCount = 0; espnowOK = true;
  } else {
    failCount++; espnowFailCount++; espnowFailTotal++;
    if (espnowFailCount >= 5) { espnowOK = false; Serial.println("[ERROR] ESP-NOW!"); }
  }
  packetCount++;

  Serial.print("["); Serial.print(SKETCH_NAME); Serial.print("] Pkt:"); Serial.print(packetCount);
  Serial.print(isLakeHouse ? " GEO:" : " HVAC:"); Serial.print(current, 2); Serial.print("A ");
  Serial.print(power); Serial.print("W "); Serial.print(stateLabel(furnaceState));
  Serial.print(geoRunning ? " GEO=ON" : " GEO=off");
  Serial.print(" Stat:"); Serial.print(myData.status);
  Serial.print(isLakeHouse ? " | PUMP:" : " | AC:"); Serial.print(acAmps, 2); Serial.print("A ");
  Serial.print(pumpWatts); Serial.print("W");
  Serial.print(acOn ? " acON" : " acOFF");
  Serial.printf(" GTHcy:%d PMPcy:%d WTRcy:%d", rtcGthCycles, rtcPumpCycles, rtcWtrCycles);
  Serial.printf(" Heap:%lu", ESP.getFreeHeap()); Serial.println();

  oledUpdateCounter++;
  if (oledUpdateCounter >= OLED_UPDATE_INTERVAL) {
    updateDisplay(current, power, outdoorTemp, totalKWh, furnaceState, false);
    oledUpdateCounter = 0;
  }

  yield(); yieldDelay(2000);
}

// ── SD stubs (not used) ───────────────────────────────────────────────────
bool initSDCard()   { return false; }
String getLogFileName() { return ""; }
bool logToSD(float, int, float, int) { return false; }
void appendSeasonSummary() {}
bool createLogFile() { return false; }

// ================================================================
// End of sketch
// ================================================================
```
