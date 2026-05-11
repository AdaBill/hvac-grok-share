// *** CHANGE SKETCH NAME HERE ONLY — propagates everywhere automatically ***
#define SKETCH_NAME "HVACv66"
#define PAIRED_WITH "ELKv51"

// **************************************************************************
// HVACv66 -- FRAME B ONLY + embedded CRC32.
// Reviewed hardening: static size guard, deterministic string clearing before CRC.
// IMPORTANT: Frame A is deprecated as a live ESP-NOW transport packet because
// it is too large/unreliable for this system. This sketch keeps the old
// hvac_data struct for local calculations/OLED only, but does NOT send it.
// Lake ELK receives only smaller Frame B packets. Frame B now carries the
// packet number, essential former-Frame-A display fields, and an embedded
// CRC32. Detection only: no automatic recovery action is added.


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
#include <stddef.h>


static uint32_t crc32_zlib(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  for (size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for (int j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & (0UL - (crc & 1UL)));
    }
  }
  return crc ^ 0xFFFFFFFFUL;
}

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
// 320 bytes - must match ELKv43 exactly
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
  // v62 (Lake): Today/Yest entities fetched from HA for ELK display
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
} myData;

// =================================================================
// FRAME B -- Lake-only extension (HVACv55 / ELKv36)
// Filled from Serial "EXT 1 ..." lines published by lake-monitor.service.
// Sent via ESP-NOW after myData when isLakeHouse is true.
// =================================================================
struct ext_device {
  uint16_t today_cents;
  uint16_t yesterday_cents;
  uint16_t runtime_min;
  uint8_t  cycles;
  uint8_t  flag;
};
struct hvac_lake_extension {
  uint16_t magic;
  uint8_t  version;
  uint8_t  reserved;
  uint16_t geo_today_cents,        geo_yesterday_cents;
  uint16_t pump_geo_today_cents,   pump_geo_yesterday_cents;
  uint16_t pump_wtr_today_cents,   pump_wtr_yesterday_cents;
  // v57: Emporia-derived runtime + cycles for geo and pump (priority rows on OLED+ELK)
  uint16_t geo_runtime_min;
  uint8_t  geo_cycles;
  uint8_t  reserved_a;
  uint16_t pump_runtime_min;
  uint8_t  pump_cycles;
  uint8_t  reserved_b;
  ext_device devices[6];
  uint16_t mains_today_cents;
  uint16_t mains_yesterday_cents;
  float    mains_kwh_today;
  uint16_t misc_today_cents;
  uint16_t misc_yesterday_cents;
  float    source_water_f;
  float    cop_today;
  char     first_error[24];

  // v66: Frame B is now the only live ELK transport packet. These fields
  // replace the small set of Frame A values the Lake ELK display still needs.
  char     timeStr[20];
  float    outdoorTemp;
  float    hddToday;
  float    cddToday;
  float    kwhRate;
  uint8_t  systemError;
  char     errorMsg[30];

  // v66: integrity fields. crc32 covers bytes from struct start through the
  // byte before crc32. packetNum lets ELK detect missed Frame B packets.
  unsigned long packetNum;
  uint32_t crc32;
} myExt;
static_assert(sizeof(hvac_lake_extension) <= 250, "Frame B must stay <= ESP-NOW safe payload size");
static_assert(offsetof(hvac_lake_extension, crc32) + sizeof(uint32_t) == sizeof(hvac_lake_extension), "crc32 must remain the final field");
static bool myExtFilled = false;
static unsigned long lastExtUpdateMs = 0;
static esp_err_t sendFrameB(unsigned long packetNum);
// 320 bytes - must match ELKv43 exactly

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
int activeChannel = 6;
// v62 Lake: fetched HA values
float fLkGeoKwhToday=0, fLkGeoKwhYest=0;
int   fLkGeoMinT=0, fLkGeoMinY=0, fLkGeoCycT=0, fLkGeoCycY=0;
float fLkPumpKwhToday=0, fLkPumpKwhYest=0;
int   fLkPumpMinT=0, fLkPumpMinY=0, fLkPumpCycT=0, fLkPumpCycY=0;
float fLkWaterKwhToday=0, fLkWaterKwhYest=0;
int   fLkWaterMinT=0, fLkWaterMinY=0, fLkWaterCycT=0, fLkWaterCycY=0;
float fLkMainsKwhToday=0, fLkMainsKwhYest=0;
float fLkMainsDolToday=0, fLkMainsDolYest=0;             // v61: actual AP channel (Lake marknet=Ch6); WiFi.channel() updates after connect
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
bool fetchLakeYesterdayMainsData();  // v62 Lake forward decl

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
  // v61: detect AP's actual channel and lock ESP-NOW to match
  activeChannel = WiFi.channel();
  esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
  Serial.print("[WiFi] OK RSSI:"); Serial.print(WiFi.RSSI());
  Serial.print(" IP:"); Serial.print(WiFi.localIP());
  Serial.printf(" Ch:%d\n", activeChannel);
  // v61: re-add ESP-NOW peer with detected channel
  esp_now_peer_info_t peer = {};
  peer.channel = activeChannel; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
  memcpy(peer.peer_addr, elkMAC, 6);
  esp_now_del_peer(elkMAC);
  esp_now_add_peer(&peer);
  return true;
}

void disconnectWiFi() {
  WiFi.disconnect(false); yieldDelay(200);
  WiFi.mode(WIFI_STA); yield();
  esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
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

// v62 Lake: Fetch yesterday + mains entities from HA. Lake only (Pondo skipped via isLakeHouse).
bool fetchLakeYesterdayMainsData() {
  if (!isLakeHouse) return true;
  WiFiClientSecure client; client.setInsecure();
  struct EF { const char* eid; void* dest; bool isInt; };
  EF ents[22] = {
    { "sensor.lake_emporia_geo_today_kwh",            &fLkGeoKwhToday,    false },
    { "sensor.lake_emporia_geo_yesterday_kwh",        &fLkGeoKwhYest,     false },
    { "sensor.lake_emporia_geo_today_runtime_min",    &fLkGeoMinT,        true  },
    { "sensor.lake_emporia_geo_yesterday_runtime_min",&fLkGeoMinY,        true  },
    { "sensor.lake_emporia_geo_today_cycles",         &fLkGeoCycT,        true  },
    { "sensor.lake_emporia_geo_yesterday_cycles",     &fLkGeoCycY,        true  },
    { "sensor.lake_emporia_well_pump_today_kwh",            &fLkPumpKwhToday, false },
    { "sensor.lake_emporia_well_pump_yesterday_kwh",        &fLkPumpKwhYest,  false },
    { "sensor.lake_emporia_well_pump_today_runtime_min",    &fLkPumpMinT,     true  },
    { "sensor.lake_emporia_well_pump_yesterday_runtime_min",&fLkPumpMinY,     true  },
    { "sensor.lake_emporia_well_pump_today_cycles",         &fLkPumpCycT,     true  },
    { "sensor.lake_emporia_well_pump_yesterday_cycles",     &fLkPumpCycY,     true  },
    { "sensor.lake_emporia_water_heater_today_kwh",            &fLkWaterKwhToday, false },
    { "sensor.lake_emporia_water_heater_yesterday_kwh",        &fLkWaterKwhYest,  false },
    { "sensor.lake_emporia_water_heater_today_runtime_min",    &fLkWaterMinT,     true  },
    { "sensor.lake_emporia_water_heater_yesterday_runtime_min",&fLkWaterMinY,     true  },
    { "sensor.lake_emporia_water_heater_today_cycles",         &fLkWaterCycT,     true  },
    { "sensor.lake_emporia_water_heater_yesterday_cycles",     &fLkWaterCycY,     true  },
    { "sensor.lake_emporia_mains_today_kwh",          &fLkMainsKwhToday, false },
    { "sensor.lake_emporia_mains_yesterday_kwh",      &fLkMainsKwhYest,  false },
    { "sensor.lake_emporia_mains_today_dollars",      &fLkMainsDolToday, false },
    { "sensor.lake_emporia_mains_yesterday_dollars",  &fLkMainsDolYest,  false },
  };
  int okCount = 0;
  for (int i = 0; i < 22; i++) {
    HTTPClient http;
    char url[160];
    snprintf(url, sizeof(url), "https://" HA_HOST "/api/states/%s", ents[i].eid);
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
    if (code != 200) continue;
    int sIdx = body.indexOf("\"state\":\"");
    if (sIdx < 0) continue;
    sIdx += 9;
    int eIdx = body.indexOf("\"", sIdx);
    if (eIdx < 0) continue;
    String stateStr = body.substring(sIdx, eIdx);
    if (ents[i].isInt) {
      *((int*)ents[i].dest) = stateStr.toInt();
    } else {
      *((float*)ents[i].dest) = stateStr.toFloat();
    }
    okCount++;
  }
  Serial.printf("[LkYest] fetched %d/22 entities\n", okCount);
  return okCount > 0;
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
    "\"friendly_name\":\"%s GTH Energy\"}}",
    totalKWh, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH power
  snprintf(eid,  sizeof(eid),  "sensor.%s_gth_power", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"W\","
    "\"device_class\":\"power\",\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s GTH Power\"}}",
    myData.furnaceWatts, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // PUMP kWh
  snprintf(eid,  sizeof(eid),  "sensor.%s_pump_kwh", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.3f\",\"attributes\":{\"unit_of_measurement\":\"kWh\","
    "\"device_class\":\"energy\",\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s Pump Energy\"}}",
    acKWh, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // PUMP power
  snprintf(eid,  sizeof(eid),  "sensor.%s_pump_power", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"W\","
    "\"device_class\":\"power\",\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s Pump Power\"}}",
    acWatts, locLabel);
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
    "{\"state\":\"%s\",\"attributes\":{\"friendly_name\":\"%s HVAC Status\"}}",
    myData.status, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH cycle count
  snprintf(eid,  sizeof(eid),  "sensor.%s_gth_cycles", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%d\",\"attributes\":{\"unit_of_measurement\":\"cycles\","
    "\"state_class\":\"total_increasing\","
    "\"friendly_name\":\"%s GTH Cycles Today\"}}",
    rtcGthCycles, locLabel);
  postOne(client, eid, body) ? ok++ : haFailCount++;

  // GTH runtime %  (%% in format string = literal % in output)
  snprintf(eid,  sizeof(eid),  "sensor.%s_runtime_pct", pfx);
  snprintf(body, sizeof(body),
    "{\"state\":\"%.1f\",\"attributes\":{\"unit_of_measurement\":\"%%\","
    "\"state_class\":\"measurement\","
    "\"friendly_name\":\"%s GTH Runtime %%\"}}",
    runtimePercent, locLabel);
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
  fetchLakeYesterdayMainsData();  // v62 Lake
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
    nextHAPost = now_t + HA_POST_INTERVAL_SEC;
  }
  disconnectWiFi(); Serial.println("[WiFi] Session done"); return ntpOK;
}

bool initESPNow() {
  WiFi.mode(WIFI_STA); yieldDelay(100);
  esp_wifi_set_channel(activeChannel, WIFI_SECOND_CHAN_NONE);
  Serial.println("[ESP-NOW] Ch1"); yieldDelay(50);
  if (esp_now_init() != ESP_OK) { Serial.println("[ESP-NOW] Init fail"); return false; }
  yieldDelay(100); esp_now_register_send_cb(onDataSent);
  esp_now_peer_info_t peer = {};
  peer.channel = activeChannel; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
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
  Serial.println("[OLED] -- frame --");
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
  {
    char l1[22];
    if (timeValid) {
      char ts2[6]; struct tm tt; if (getLocalTime(&tt, 50)) strftime(ts2, sizeof(ts2), "%H:%M", &tt); else strcpy(ts2, "--:--");
      snprintf(l1, sizeof(l1), "Out %dF %s %s", (int)outdoorTemp, ts2, SKETCH_NAME + 4);
    } else {
      snprintf(l1, sizeof(l1), "Out %dF --:-- %s", (int)outdoorTemp, SKETCH_NAME + 4);
    }
    Serial.print("[OLED L1] "); Serial.println(l1);
  }

  // Line 2 (y=22): static circuit identity label, tight format to fit cycle digits.
  //   Pondo CT0/1 = furnace blower+gas -> "FAN"
  //   Lake  CT0/1 = WPV36 geo compressor -> "GTH"
  //   State info lives in myData.status / HA sensor.
  u8g2.setCursor(2, 22);
  if (isLakeHouse && myExtFilled) {
    // v57: pull from Emporia (Frame B) since local CT is disconnected at Lake.
    float gdlr = myExt.geo_today_cents / 100.0f;
    snprintf(buf, sizeof(buf), "GTH: %3dm C:%d $%.2f",
      (int)myExt.geo_runtime_min, (int)myExt.geo_cycles, gdlr);
  } else {
    const char* line2_label = isLakeHouse ? "GTH" : "FAN";
    snprintf(buf, sizeof(buf), "%s:%4d %2dh %2d%%C:%d",
      line2_label, power, (int)runtimeHoursToday, (int)runtimePercent, rtcGthCycles);
  }
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

  // Line 3 (y=33): static circuit identity label.
  //   Pondo CT2/3 = AC condenser -> "AC "
  //   Lake  CT2/3 = Grundfos well pump -> "PMP"
  u8g2.setCursor(2, 33);
  if (isLakeHouse && myExtFilled) {
    // Combined pump $ = pump_geo + pump_wtr; runtime/cycles = full pump activity.
    float pdlr = (myExt.pump_geo_today_cents + myExt.pump_wtr_today_cents) / 100.0f;
    snprintf(buf, sizeof(buf), "PMP: %3dm C:%d $%.2f",
      (int)myExt.pump_runtime_min, (int)myExt.pump_cycles, pdlr);
  } else {
    const char* line3_label = isLakeHouse ? "PMP" : "AC ";
    snprintf(buf, sizeof(buf), "%s:%4d %2dh %2d%%C:%d",
      line3_label, acWatts, (int)acRuntimeHoursToday, (int)acRuntimePercent, rtcPumpCycles);
  }
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

  // Line 4 (y=44): WTR at Lake = water heater (Emporia devices[0]). Blank at Pondo.
  u8g2.setCursor(2, 44);
  if (isLakeHouse && myExtFilled) {
    float wdlr = myExt.devices[0].today_cents / 100.0f;
    snprintf(buf, sizeof(buf), "WTR: %3dm C:%d $%.2f",
      (int)myExt.devices[0].runtime_min, (int)myExt.devices[0].cycles, wdlr);
    buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);
  } else if (isLakeHouse) {
    snprintf(buf, sizeof(buf), "WTR:%4d %2dh %2d%%C:%d",
      wtrWatts, (int)wtrRuntimeHoursToday, (int)wtrRuntimePercent, rtcWtrCycles);
    buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);
  } else {
    u8g2.print("                     ");
  }

  // Line 5 (y=55): cost summary. Lake = Emporia G/P/W; Pondo = FAN/AC firmware-local.
  u8g2.setCursor(2, 55);
  if (isLakeHouse && myExtFilled) {
    float gd = myExt.geo_today_cents / 100.0f;
    float pd = (myExt.pump_geo_today_cents + myExt.pump_wtr_today_cents) / 100.0f;
    float wd = myExt.devices[0].today_cents / 100.0f;
    snprintf(buf, sizeof(buf), "G$%.2f P$%.2f W$%.2f", gd, pd, wd);
  } else if (isLakeHouse) {
    snprintf(buf, sizeof(buf), "G:$%.2f P:$%.2f W:$%.2f",
      kWh * rate, acKWh * rate, wtrKWh * rate);
  } else {
    snprintf(buf, sizeof(buf), "FAN:$%.2f  AC:$%.2f",
      kWh * rate, acKWh * rate);
  }
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

  // Line 6 (y=66): WX+27-0 NT+1-0
  u8g2.setCursor(2, 66);
  snprintf(buf, sizeof(buf), "WX+%d-%d NT+%d-%d",
    weatherFetchCount, weatherFailCount, ntpSyncCount, ntpFailCount);
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

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
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

  // Line 8 (y=88): Total today $. Lake = mains from Emporia, Pondo = sum of CTs.
  u8g2.setCursor(2, 88);
  float totalCost;
  if (isLakeHouse && myExtFilled) {
    totalCost = myExt.mains_today_cents / 100.0f;
  } else {
    totalCost = (kWh + acKWh + wtrKWh) * rate;
  }
  snprintf(buf, sizeof(buf), "Total: $%.2f", totalCost);
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

  // Line 9 (y=99): error msg OR last restart time if no error
  u8g2.setCursor(2, 99);
  if (myData.systemError) {
    snprintf(buf, sizeof(buf), "%-21s", myData.errorMsg);
  } else {
    snprintf(buf, sizeof(buf), "%-21s", lastRestartTime);
  }
  buf[21] = 0; u8g2.print(buf); Serial.print("[OLED] "); Serial.println(buf);

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
  readSerialEXT();
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
  myData.kwhRate             = fetchedKwhRate;
  // v62 Lake: populate Today/Yest fields for ELK display
  if (isLakeHouse) {
    myData.lakeGeoKwhToday = fLkGeoKwhToday;
    myData.lakeGeoKwhYest = fLkGeoKwhYest;
    myData.lakeGeoMinutesToday = fLkGeoMinT;
    myData.lakeGeoMinutesYest = fLkGeoMinY;
    myData.lakeGeoCyclesToday = fLkGeoCycT;
    myData.lakeGeoCyclesYest = fLkGeoCycY;
    myData.lakePumpKwhToday = fLkPumpKwhToday;
    myData.lakePumpKwhYest = fLkPumpKwhYest;
    myData.lakePumpMinutesToday = fLkPumpMinT;
    myData.lakePumpMinutesYest = fLkPumpMinY;
    myData.lakePumpCyclesToday = fLkPumpCycT;
    myData.lakePumpCyclesYest = fLkPumpCycY;
    myData.lakeWaterKwhToday = fLkWaterKwhToday;
    myData.lakeWaterKwhYest = fLkWaterKwhYest;
    myData.lakeWaterMinutesToday = fLkWaterMinT;
    myData.lakeWaterMinutesYest = fLkWaterMinY;
    myData.lakeWaterCyclesToday = fLkWaterCycT;
    myData.lakeWaterCyclesYest = fLkWaterCycY;
    myData.lakeMainsKwhToday = fLkMainsKwhToday;
    myData.lakeMainsKwhYest = fLkMainsKwhYest;
    myData.lakeMainsDollarsToday = fLkMainsDolToday;
    myData.lakeMainsDollarsYest = fLkMainsDolYest;
  } // NEW v51 - push rate to ELK

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

  // v66: Frame A is intentionally NOT sent. It is too large/unreliable as an
  // ESP-NOW transport unit in this system. Keep myData only for local OLED and
  // for copying a few display fields into smaller CRC-protected Frame B.
  myData.packetNum = elkPacketsSent;
  elkPacketsSent++;
  esp_err_t r = ESP_OK;
  lastSendOK = true;
  if (isLakeHouse) {
    lastSendOK = false;
    r = sendFrameB(myData.packetNum);
    yieldDelay(20);
  }
  yield(); yieldDelay(50);
  if (r == ESP_OK && lastSendOK) {
    elkPacketsSuccess++; espnowFailCount = 0; espnowOK = true;
  } else {
    failCount++; espnowFailCount++; espnowFailTotal++;
    if (espnowFailCount >= 5) { espnowOK = false; Serial.println("[ERROR] ESP-NOW!"); }
    if (espnowFailCount >= 30) {
      Serial.printf("[v61] %d consecutive ESP-NOW fails -- restarting to re-detect channel\n", espnowFailCount);
      delay(500); ESP.restart();
    }
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


// =================================================================
// FRAME B -- helpers (added v55)
// =================================================================
static char extLineBuf[512];
static size_t extLineLen = 0;

static uint16_t pop_u16(char** sp) {
  char* tok = strtok_r(NULL, " \t\r\n", sp);
  return tok ? (uint16_t) atoi(tok) : 0;
}
static uint8_t pop_u8(char** sp) {
  char* tok = strtok_r(NULL, " \t\r\n", sp);
  return tok ? (uint8_t) atoi(tok) : 0;
}
static float pop_f(char** sp) {
  char* tok = strtok_r(NULL, " \t\r\n", sp);
  return tok ? (float) atof(tok) : 0.0f;
}

static void applyExtLine(char* line) {
  // Format documented in HVACv55: positional, single line, ends at newline.
  // First token must be "EXT", second must be version "1".
  char* save = NULL;
  char* tok = strtok_r(line, " \t\r\n", &save);
  if (!tok || strcmp(tok, "EXT") != 0) return;
  tok = strtok_r(NULL, " \t\r\n", &save);
  if (!tok || atoi(tok) != 1) return;

  hvac_lake_extension n = {};
  n.magic   = 0xE36B;
  n.version = 2;  // v66 packet layout; EXT input line remains version 1
  n.geo_today_cents       = pop_u16(&save);
  n.geo_yesterday_cents   = pop_u16(&save);
  n.pump_geo_today_cents  = pop_u16(&save);
  n.pump_geo_yesterday_cents = pop_u16(&save);
  n.pump_wtr_today_cents  = pop_u16(&save);
  n.pump_wtr_yesterday_cents = pop_u16(&save);
  n.geo_runtime_min  = pop_u16(&save);
  n.geo_cycles       = pop_u8(&save);
  n.pump_runtime_min = pop_u16(&save);
  n.pump_cycles      = pop_u8(&save);
  for (int i = 0; i < 6; i++) {
    n.devices[i].today_cents     = pop_u16(&save);
    n.devices[i].yesterday_cents = pop_u16(&save);
    n.devices[i].runtime_min     = pop_u16(&save);
    n.devices[i].cycles          = pop_u8(&save);
    n.devices[i].flag            = pop_u8(&save);
  }
  n.mains_today_cents     = pop_u16(&save);
  n.mains_yesterday_cents = pop_u16(&save);
  n.mains_kwh_today       = pop_f(&save);
  n.misc_today_cents      = pop_u16(&save);
  n.misc_yesterday_cents  = pop_u16(&save);
  n.source_water_f        = pop_f(&save);
  n.cop_today             = pop_f(&save);
  // Remaining text (if any) is first_error string -- single token, no spaces.
  char* rest = strtok_r(NULL, "\r\n", &save);
  if (rest && strcmp(rest, "-") != 0) {
    strncpy(n.first_error, rest, sizeof(n.first_error) - 1);
    n.first_error[sizeof(n.first_error) - 1] = '\0';
  } else {
    n.first_error[0] = '\0';
  }
  memcpy(&myExt, &n, sizeof(myExt));
  myExtFilled = true;
  lastExtUpdateMs = millis();
  Serial.println("[EXT] applied Frame B from serial");
}

static void readSerialEXT() {
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n' || extLineLen >= sizeof(extLineBuf) - 1) {
      extLineBuf[extLineLen] = 0;
      if (extLineLen > 4) applyExtLine(extLineBuf);
      extLineLen = 0;
    } else if (c != '\r') {
      extLineBuf[extLineLen++] = (char) c;
    }
  }
}

static esp_err_t sendFrameB(unsigned long packetNum) {
  if (!isLakeHouse) return ESP_OK;

  // If lake-monitor has not supplied EXT data yet, still send a valid small
  // Frame B so ELK has a heartbeat and can show an explicit EXT-missing error
  // instead of depending on dead/oversized Frame A or rebooting for no RX.
  if (!myExtFilled || myExt.magic != 0xE36B) {
    memset(&myExt, 0, sizeof(myExt));
    myExt.magic = 0xE36B;
    myExt.version = 2;
    strncpy(myExt.first_error, "EXT missing", sizeof(myExt.first_error) - 1);
  }

  // Copy essential former-Frame-A display values into Frame B so ELK no longer
  // depends on the oversized/deprecated Frame A packet.
  memset(myExt.timeStr, 0, sizeof(myExt.timeStr));
  strncpy(myExt.timeStr, myData.timeStr, sizeof(myExt.timeStr) - 1);
  myExt.outdoorTemp = myData.outdoorTemp;
  myExt.hddToday = myData.hddToday;
  myExt.cddToday = myData.cddToday;
  myExt.kwhRate = myData.kwhRate;
  myExt.systemError = myData.systemError ? 1 : 0;
  memset(myExt.errorMsg, 0, sizeof(myExt.errorMsg));
  strncpy(myExt.errorMsg, myData.errorMsg, sizeof(myExt.errorMsg) - 1);

  myExt.packetNum = packetNum;
  myExt.crc32 = 0;
  const size_t crcLen = offsetof(hvac_lake_extension, crc32);
  myExt.crc32 = crc32_zlib((const uint8_t*)&myExt, crcLen);
  Serial.printf("[CRC] sent pktB=%lu crc=0x%08lX size=%d crcLen=%u\n",
                myExt.packetNum, (unsigned long)myExt.crc32,
                (int)sizeof(myExt), (unsigned)crcLen);

  esp_err_t r = esp_now_send(elkMAC, (uint8_t*)&myExt, sizeof(myExt));
  if (r != ESP_OK) Serial.printf("[FrameB] send fail %d\n", (int)r);
  return r;
}
