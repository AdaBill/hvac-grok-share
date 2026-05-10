# SPEC: Add CRC32 integrity check to lake HVAC + lake ELK firmware

## Context for ChatGPT

You already analyzed the lake ELK silent-data bug and produced ELKv47_FIXED. Excellent work. The fix landed and is running on the lake ELK now.

This is a follow-on hardening task. The owner wants **silent corruption detection** so future packet-level data issues don't sit undetected. He has explicitly chosen NOT to add automated recovery — only detection + notification. A human will respond to the alert.

## Problem statement

Lake firmware (HVAC v62 + ELK v47) has **no data integrity check** on ESP-NOW packets. If a packet arrives at ELK with corrupted bytes, ELK will memcpy the garbage into its display structs and render bad values silently. We need detection.

## Constraints (HARD)

1. **No struct size change.** Adding a CRC field to the struct would change `sizeof(hvac_data)` and `sizeof(hvac_lake_extension)`, which would require lockstep reflash of HVAC and ELK or the lake goes dark. The owner explicitly wants the two firmwares to be flashable INDEPENDENTLY.
2. **No automated recovery action.** Just detect and report. Recovery is a human decision.
3. CRC must cover BOTH Frame A (`hvac_data`) and Frame B (`hvac_lake_extension`).
4. Use CRC32 (false-negative rate ~1 in 4 billion, more than enough; library available on Arduino-ESP32).
5. If only one firmware gets the change initially, the other side just won't have CRC data yet — nothing should break. Backwards-compatible.

## Design

Both sides compute CRC32 over the same byte range (the full struct, since there's no CRC field embedded). Both sides print their computed CRC to serial. A Pi 5 cron script (separately written by Claude — not your responsibility) tails both serial logs, matches by packet number, and ntfys the owner on mismatch.

### Required output format on serial

**HVAC side** (just before each ESP-NOW send):
```
[CRC] sent pktA=842 crc=0x12345678 size=228
[CRC] sent pktB=842 crc=0x9ABCDEF0 size=NNN
```

**ELK side** (just after each `memcpy` in `onDataReceive`, BEFORE the early-return on size mismatch):
```
[CRC] rcvd pktA=842 crc=0x12345678 size=228
[CRC] rcvd pktB=842 crc=0x9ABCDEF0 size=NNN
```

Notes:
- `pktA` / `pktB` share the same packet number (HVAC sends Frame A and Frame B as a paired set).
- The packet number is `packetNum` field in the struct (already exists in HVACv62).
- Print AFTER memcpy on receive, so we capture the CRC of bytes as actually received (corruption-detecting).
- Print on size mismatch too (will already be flagged separately by existing `[ERROR] Mismatch!` line, but include CRC for forensics).

## Implementation guidance

### CRC32 algorithm

Use the standard zlib-compatible CRC32 (polynomial 0xEDB88320, initial value 0xFFFFFFFF, final XOR 0xFFFFFFFF). Tiny table-free implementation (~30 lines) is fine. Or use the ESP-IDF built-in `esp_crc32_le()` if it's exposed in Arduino-ESP32. Either is acceptable — pick whichever is cleaner.

### Where to add it on the HVAC side (lake_HVAC_v62_EXACT.ino, becomes v63)

- Bump `#define SKETCH_NAME "HVACv62"` to `"HVACv63"`
- Just before each `esp_now_send()` call, compute CRC32 over the bytes about to be sent and `Serial.printf("[CRC] sent pkt%c=%lu crc=0x%08lX size=%d\n", frameLetter, packetNum, crc, sizeof(struct))`
- Two send sites (Frame A and Frame B) — both need the CRC line

### Where to add it on the ELK side (lake_ELK_v47_FIXED.ino, becomes v48)

- Bump `#define SKETCH_NAME "ELKv47"` to `"ELKv48"`
- Inside `onDataReceive()`, AFTER the memcpy into `pendingData` or `pendingExt`, compute CRC32 over those bytes and `Serial.printf("[CRC] rcvd pkt%c=%lu crc=0x%08lX size=%d\n", ...)`
- Frame A goes into `pendingData`; Frame B goes into `pendingExt`. Two paths, both need the CRC line.
- For Frame B (`hvac_lake_extension`), the packet number field is wherever it is in that struct — please find it and use it. If there is no per-frame packet number, use the latest known `pendingData.packetNum` (the paired Frame A's number).

## Files in this folder

- `SPEC_for_chatgpt.md` — this file
- `lake_HVAC_v62_EXACT.ino` — current lake HVAC source. Modify into v63.
- `lake_ELK_v47_FIXED.ino` — your previously-fixed ELK source. Modify into v48.

## Output you should produce

Two new `.ino` files:
1. `lake_HVAC_v63.ino` — v62 + CRC compute & print on each send
2. `lake_ELK_v48.ino` — v47_FIXED + CRC compute & print on each receive

Plus a brief change log at the top of each file describing what was added.

## Out of scope (DO NOT do these)

- Don't change struct definitions
- Don't add CRC FIELD to the struct
- Don't modify any ESP-NOW retry/recovery logic
- Don't modify the existing v47 Frame B commit fix (your existing work)
- Don't add automated recovery actions on CRC failure (detection only)

## Why this design

Code-only changes on both sides. No struct change → no lockstep reflash → either firmware can be updated independently and the other will just continue with no CRC data until upgraded. Lowest blast radius, highest detectability.
