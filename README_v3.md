# PROKOPOV alarm — firmware v3 (main board Rev A)

Branch `v3-board`. The HTTP contract with `alarm-server` (`/status`, `/command`, `/config`, `/cards`, `/events`,
`/zones/reconcile`, `/card-scan/*`, Shelly webhooks on 8081) is unchanged, so the server and the web UI keep working.

## What is new

| Area | v1/v2 (Olimex + SD) | v3 (Rev A board) |
|---|---|---|
| Readers | one Wiegand reader | two readers (outside J4, inside J5), separate counters and lock-outs |
| Storage | SD card (cards, config, state, log) | cards + config in flash FAT (`storage` partition, written only on sync); state and audit journal in an optional FRAM module (U12). Without FRAM: state in flash (only on real changes), journal in RAM + alarm-server |
| Journal | append to SD | with FRAM: 250-event ring that survives power loss; without: RAM ring, events also kept on alarm-server |
| Lock | one relay, follows the alarm | `lock_type: maglock / strike`; modes `auto`, `always_locked`, `always_unlocked` (commands `lock_mode_auto`, `lock_mode_locked`, `lock_mode_unlocked`), mode kept across reboots |
| Door while armed | `unlock` allowed | `unlock` refused while armed / in alarm |
| Brute force | none | 5 rejected cards or PINs within 5 min, then that input is blocked for 5 min (`LOCKOUT_300S` event, `clear_lockout` command) |
| Keypad | none | 2.8" panel on RS-485, PIN + action; frames HMAC-SHA256 signed, session + nonce + sequence (no replay) |
| PIN storage | — | only SHA-256 of the PIN (`pin_hash` in the card record); `/cards` reports `has_pin`, never the hash |
| Shelly Uni | Wi-Fi webhooks | also wired: OUT1 = arm, OUT2 = disarm (opto inputs), IN1 = armed, IN2 = alarm (opto outputs) |
| Supervision | — | mains (opto), battery V / A (ADS1115), daily battery test (LM2576 switched off for 60 s via GPIO45), enclosure tamper (GPIO46), siren line voltage on ADS1115 A1: short / OK / open (needs a **2.2 kΩ EOL resistor at the siren**) |
| Watchdog | — | NE555 (~7 s, first period ~11 s), AC-coupled kick; a timer kicks it during boot, then only `loop()` (every 200 ms, after 15 s of normal running); JP4 removed = watchdog disabled (first flashing / debugging) |
| Keys | `==` compare | constant-time compare for API and webhook keys; API key at least 16 characters |

## Parts (all available in Bulgaria)

RTC: DS3231 mini module (ESPHome `ds1307` platform, same registers). RS-485: SN75176 at 5 V. I/O expander: none
(tamper on GPIO46, battery-test switch on GPIO45, siren line on the ADS1115). See the board BOM for the full list.

## Files

* `esphome/components/prokopov_alarm/` — alarm core (v3)
* `esphome/components/prokopov_panel_link/` — RS-485 protocol, controller and panel role in one component
* `examples/prokopov-alarm-v3.yaml` — the main board
* `examples/prokopov-panel-cyd.yaml` — ESP32-2432S028 touch panel (LVGL keypad)
* `examples/partitions_v3_16mb.csv` — partition table with the 2 MB `storage` FAT partition

## Secrets (`secrets.yaml`)

```
alarm_control_key: "<at least 16 random characters>"
alarm_shelly_key:  "<at least 12 random characters>"
panel_link_key:    "<64 hex characters>"     # python3 -c "import secrets;print(secrets.token_hex(32))"
```

## PIN codes

The server sends a PIN as its SHA-256 in the card record:

```json
{"uid": "1234567", "user_name": "Ivaylo", "can_arm": true, "can_disarm": true,
 "pin_hash": "03ac674216f3e15c761ee1a5e255f067953623c8b388b4459e13f978d7c846f4"}
```

(`python3 -c "import hashlib;print(hashlib.sha256(b'1234').hexdigest())"`). Minimum PIN length is 4 digits.

## First start of a new board

1. Remove JP4 (watchdog), flash over USB, check the log: `Flash FAT mounted` (and `FRAM journal` if U12 is fitted).
2. Fit JP4. From now on a stuck main loop resets the board within about 7 s.
3. Sync config and cards from alarm-server: until `config_revision` > 0 the board refuses to arm (as before).
