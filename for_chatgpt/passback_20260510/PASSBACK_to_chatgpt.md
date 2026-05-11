# Passback to ChatGPT: HVAC v66 + ELK v51 flashed, alarm-firing problem

## Context

Both files (`lake_HVAC_v66_FRAMEB_ONLY_CRC_REVIEWED.ino` and `lake_ELK_v51_FRAMEB_ONLY_CRC_REVIEWED.ino`) were compiled clean (static_asserts passed) and flashed successfully today around 20:00 EDT. Owner is at the lake house. Claude Code was asked to flash and verify.

## What works (verified)

- **HVAC v66 USB-flashed via lake-pi5** with `UploadMode=cdc`. Hash verified, hard reset clean.
- **ELK v51 OTA-flashed** to lake-elk.local (192.168.4.58). 100% Done.
- ELK boots cleanly, banner says `ELKv51 - ALIVE`, `Paired with HVACv66`, `Frame A: DEPRECATED/IGNORED  Frame B: CRC32 verified`.
- ELK joins WiFi marknet, IP=192.168.4.58, channel locked to 11, mDNS up.
- ESP-NOW pairing healthy.
- **Frame B receipt is working**: ELK [DIAG] line shows `RX:121 Lost:0 Link:100% RSSI:-52 Heap:251888 ext:yes` — the `ext:yes` means `extDataReady` flipped true (your v47 commit-from-pendingExt fix is exercising correctly).
- **Link quality jumped from 50% (pre-flash) to 100% (post-flash)** — a real win, removing the deprecated/oversized Frame A from the air freed up airtime.

## The problem: ELK [ALARM] HVAC_ERROR fires every ~10 seconds

ELK serial log (excerpt, runtime_evidence.txt has more):
```
2026-05-10T20:16:23-04:00 [ALARM] HVAC_ERROR
2026-05-10T20:16:31-04:00 [DIAG] ELKv51 paired:HVACv66 RX:102 Lost:0 Link:100% RSSI:-52 Heap:251888 ext:yes
2026-05-10T20:16:32-04:00 [ALARM] HVAC_ERROR
2026-05-10T20:16:42-04:00 [ALARM] HVAC_ERROR
... (continues ~every 9-10 sec)
```

ELK v51 alarm gate (your code, line 378):
```c
extAlarmPending = (tmp.systemError != 0) || (tmp.first_error[0] != '\0');
```

So one or both of these is true on every Frame B:
- `receivedExt.systemError != 0`
- `receivedExt.first_error[0] != '\0'`

## What we cannot directly observe (and why)

**Lake-pi5's `hvac-logger.service` (which captures `/dev/ttyACM0` to `/home/lake/hvac_log/hvac_YYYYMMDD.log`) stopped capturing HVAC output at 19:58:29 EDT** — the moment the HVAC USB flash began. The logger uses `cat /dev/ttyACM0 | while read ...` and its file handle went stale during the ESP32 USB re-enumeration. It hasn't been restarted. So we have ZERO direct visibility into what HVAC v66 is actually printing on serial — no `[HVACv66] Pkt:N` lines, no `[EXT] applied Frame B from serial` lines, no `[CRC] sent pktB=...` lines, nothing.

`lsof /dev/ttyACM0` confirms: no process is currently reading that port. (Only `cat /dev/ttyUSB0` is held by elk-logger.)

This is fixable trivially (`sudo systemctl restart hvac-logger.service`) but Claude was told to STOP and write this passback rather than keep poking. **We need this restarted to see HVAC's side of the story** — but the owner has not green-lit that action yet, so leaving it to whoever (you or Claude) acts next.

## What we DO observe on lake-pi5 / ext_publisher side

`ext_publisher.py` (lake-monitor's serial→HVAC EXT line publisher) was killed and restarted to refresh its serial handle after the flash. It's now successfully writing EXT lines to `/dev/ttyACM0` every 60 sec:

```
[20:16:59] EXT line sent (130 bytes)
[20:18:00] EXT line sent (131 bytes)
[20:19:00] EXT line sent (131 bytes)
```

So EXT data is flowing INTO HVAC's serial. Whether HVAC v66's `applyExtLine()` is actually parsing them and setting `myExtFilled = true` — we can't see (no HVAC log capture).

## Two hypotheses for the alarm

We can't distinguish without HVAC's serial output, but logically one of these must be true:

### Hypothesis A: `myExt.first_error` is still set to "EXT missing"

In `sendFrameB()`:
```c
if (!myExtFilled || myExt.magic != 0xE36B) {
    memset(&myExt, 0, sizeof(myExt));
    myExt.magic = 0xE36B;
    myExt.version = 2;
    strncpy(myExt.first_error, "EXT missing", sizeof(myExt.first_error) - 1);
}
```

If `myExtFilled` is still false (because `applyExtLine` isn't parsing the inbound EXT lines), this path runs every send and `first_error[0] = 'E'`. ELK alarm fires.

Possible causes:
- The EXT line format from `ext_publisher.py` doesn't match what `applyExtLine` expects in v66 (we DID look — it expects "EXT 1 ..." with positional fields; ext_publisher writes that format. But you'd know better whether v66's parser is strict about field counts/format edge cases.)
- `readSerialEXT()` isn't being called frequently enough to consume incoming bytes.
- The serial line buffer is overflowing (`extLineBuf` size limit).
- Some other parse failure.

### Hypothesis B: `myExt.systemError = 1` because `myData.systemError` is true

In `sendFrameB()`:
```c
myExt.systemError = myData.systemError ? 1 : 0;
strncpy(myExt.errorMsg, myData.errorMsg, sizeof(myExt.errorMsg) - 1);
```

`myData.systemError` is set by HVAC's own startup-state checks:
- `!espnowOK` → "ERR:ESPNOW"
- `!ads1115OK` → "ERR:CT"
- `!timeValid` → "ERR:NTP"
- `wifiFailCount >= 3` → "ERR:WIFI"
- `weatherFailCount >= 3` → "ERR:WX"

After a fresh flash, NTP and WX haven't necessarily succeeded yet. HVAC might be flagging itself as in-error during startup, which then propagates into Frame B's systemError, which then triggers the ELK alarm.

Without HVAC's serial output, we can't see which of these (if any) is firing.

## Files in this folder

- `PASSBACK_to_chatgpt.md` — this writeup
- `runtime_evidence.txt` — actual log excerpts and process state captured 20:19 EDT
- `lake_HVAC_v66_FRAMEB_ONLY_CRC_REVIEWED.ino` — your file, exactly as flashed
- `lake_ELK_v51_FRAMEB_ONLY_CRC_REVIEWED.ino` — your file, exactly as flashed

(The `.ino` files are the same ones you produced. Included so you have everything in one place without needing to look elsewhere.)

## Specific questions for you

1. **Should ELK v51's alarm logic differentiate between "EXT missing" (transient, expected at startup) and "real HVAC error" (persistent)?** Currently they trigger the same alarm path. Maybe `systemError` should fire alarm but `first_error == "EXT missing"` should NOT — instead show as a separate "data feed missing" indicator. That'd reduce noise during expected startup transients.

2. **Should HVAC v66 wait for its startup conditions (NTP, WX, ESPNOW pairing) to settle before reporting systemError = true?** Right now any boot-state flag like "WX hasn't succeeded yet, count = 0" might leave systemError true at the first send. A small "still warming up" grace period would reduce false alarms.

3. **Is there any path in v66 where `applyExtLine()` could silently reject a valid-looking EXT line?** Worth a sanity check on the parser given that we've confirmed ext_publisher IS sending well-formed EXT 1 lines but we can't confirm HVAC accepts them.

4. **Should v66 print explicit serial diagnostics for state transitions?** E.g.:
   - `[EXT] applied: pktB=N first_error='X' systemError=Y`
   - `[EXT] REJECTED: reason=...`
   - `[STATE] systemError=1 because: timeValid=false weatherFailCount=2 ...`
   That would immediately answer "which hypothesis is true" without log archaeology.

## What Claude could do next on Bill's say-so

- Restart `hvac-logger.service` to re-establish capture of `/dev/ttyACM0`. Once captured, the next minute of HVAC output will tell us which hypothesis (A or B) is correct. This is the SINGLE highest-value next step.
- Roll back to v62/v47 if the alarm is unacceptable (lockstep flash in reverse).
- Adjust ELK alarm gating (per your guidance) to silence the noise while keeping detection.

## Net assessment

The architecture change is working as designed: Frame B with embedded CRC32 is being received and verified successfully (RX climbing, no Lost packets, Link 100%, ext:yes). The lake operational table SHOULD now show real numbers (owner can confirm visually since he's at the lake).

The alarm is the only outstanding issue and it's almost certainly one of the two hypotheses above. With the hvac-logger restart we'd know within 60 seconds which one.
