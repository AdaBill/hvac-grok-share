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
