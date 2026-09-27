# Clean-room FCC unlocks

Every FCC unlock in `modules/` is clean-room: it contains no OEM code and loads
none of the bundled libraries at runtime. Each was derived by reading that OEM's
own code path for that specific modem and reproducing the messages it sends with
stock tooling. For most modules that path is in Lenovo's `lenovo-wwan-unlock`
package, `DPR_Fcc_unlock_service` and the worker library it `dlopen()`s. Where a
module is also sold by Dell or HP with a different sequence, that OEM's own
Windows tool was read too; per module traces, with addresses, are in
[traces/](traces/README.md) and the state of every module is in
[COVERAGE.md](COVERAGE.md).

Lenovo's bundled libraries remain in `vendor/lenovo/`, and `wwan-orch` remains
in the tree, **only for SAR**. No unlock uses either.

## The three mechanisms

| Mechanism | Modules | Tool |
|---|---|---|
| `foxunlock` | Foxconn T99W696 | our own QMI-over-MBIM sender |
| `at-gtfcclock` | Rolling RW101R-GL, Fibocom FM350-GL, Intel L860R+ on Lenovo machines | the dispatcher, with `sha256sum` |
| `intel-fcc-mbim` | Intel L860R+ on Dell machines | stock `mbimcli`, Intel Mutual Authentication service, CID 1 |
| `mbimcli` | Quectel EM160R-GL, EM061K, RM520N-GL, EM05-G, EM05-CN | stock `mbimcli`, Quectel radio state |

The L860R+ needs two of those in one dispatcher, because three OEMs ship that
module and Lenovo's AT sequence is right for only one of them. The DMI system
vendor picks the method, which is what Lenovo's own library does.

## `foxunlock` — Foxconn T99W696, `17cb:0308`

Vendor path: `setFccUnlock_fxn` → `libfiisdk.so.2.2.2`, whose `Fox_Attempt()`
calls `FoxApSetFccLockStatus()` → `QMIFOXAPSetFccLockStatus()` (`0x12372`).

That function composes service byte **0xE4** and message id **0x5571**:

```
1238c: movb $-0x1c,-0x7(%rbp)      # 0xE4
12390: movw $0x5571,-0x6(%rbp)
```

and passes both to `SendMessage()`, which stores the service as a byte at
`0xf00f`, reloads it at `0xf033` and hands it to `ComposeUniversalQMUXMsg()` —
so the byte reaches the QMUX service field.

| Field | Value |
|---|---|
| TLV `0x01` | 4-char salt + lowercase md5 hex (32) = 36 bytes |
| TLV `0x02` | one byte, `'0'` to unlock |
| md5 input | mcfg version (last two dot-fields dropped) + apps version + IMEI + salt + `FDE2` |

The magic is not a stored string: bytes `69 67 68 55` (`ighU`) are written to the
stack at `0xb83a`–`0xb84f` and each is passed through `b_char_value()`
(`0xc059`), which is `c ? c - 0x23 : 0`, giving `F D E 2`. Firmware versions come
from FOX (`0xE3`) msg `0x555E`; the IMEI from DMS (`0x02`).

`foxunlock` is used rather than `qmicli` because libqmi does not know service
`0xE4` yet — that is [libqmi!473](https://gitlab.freedesktop.org/mobile-broadband/libqmi/-/merge_requests/473). Once it ships, the dispatcher
can become `qmicli --foxap-set-fcc-authentication`.

## `at-gtfcclock` — Rolling and Fibocom

Vendor paths:

- `fccunlock_rw101` → `libmodemauthRW101.so.1.1`. `get_usb_wwan_module` probes
  the USB product id with `lsusb | grep 'RW101R-GL'` and looks it up in the
  library's `modules` table, which maps `01a8`, `01a9`, `0301` and `0302` to
  device type 3. `init_modemauth_srvc` dispatches type 3 to
  `fcc_at_modem_unlock_101r` on `/dev/cdc-wdm0`.
- `fccunlock_fm350_l860` → `libmodemauth.so`. Called with transport selector 1 at
  every call site, for the FM350-GL and the L860R+ alike, which resolves
  `init_modemauth_srvc()` on `/dev/wwan0at0`. The `init_modemauth_srvc_mbim()`
  branch is resolved but never chosen.

They do not all run the same sequence. There are three functions:

`event_monitor_at()`, reached by the L860R+ (device type 2):

| Command | Sent via | On failure |
|---|---|---|
| `at+gtfcclockgen` | `at_send_command_singleline` | log, `exit(1)` |
| `at+gtfcclockver=%lu` | `at_send_command_singleline` | log, `exit(1)`; a value other than 1 retries |
| `at+gtfcclockmodeunlock` | `at_send_command` | log, `exit(1)` |
| `at+cfun=1` | `at_send_command` | log, `exit(1)` |
| `at+gtfcclockstate` | `at_send_command_singleline` | log, `exit(1)` |

`event_monitor_at_fm350()`, reached by the FM350-GL:

| Command | Sent via | On failure |
|---|---|---|
| `at+gtfcclockgen` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `at+gtfcclockver=%lu` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `at+cfun=1` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `AT+GTFCCEFFSTATUS?` | `send_at_of_mm` | `LOGE`, `exit(1)` |

`event_monitor_at_101r()`, reached by every Rolling id (device type 3):

| Command | Sent via | On failure |
|---|---|---|
| `ate0` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `at+gtfcclockgen` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `at+gtfcclockver=0x<hex>` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `at+cfun=1` | `send_at_of_mm` | `LOGE`, `exit(1)` |
| `AT+GTFCCEFFSTATUS?` | `send_at_of_mm` | `LOGE`, `exit(1)` |

The FM350 path sends no `at+gtfcclockmodeunlock`, and neither does the Rolling
one; their status commands differ from `event_monitor_at`'s. The Rolling path
never inspects the `at+gtfcclockver` reply: the status read decides, non-zero
meaning unlocked. Nothing on any path is best effort: every failure branch ends
in `exit(1)`, which terminates `DPR_Fcc_unlock_service` itself.

On the `event_monitor_at` and `event_monitor_at_fm350` paths the challenge is
located with `get_dev_code()`, advanced past the `0x` and parsed with `strtoul`
base 16, and the response comes from `compute_sha256()`:

```
Sha256_Init -> Sha256_Update(key, 0xe) -> Sha256_Final
Sha256_Init -> Sha256_Update(4) -> Sha256_Update(4) -> Sha256_Final
```

That is `sha256( sha256(key)[0:4] ++ challenge[0:4] )[0:4]`, sent as a decimal.
The Rolling path instead uses `compute_sha256_101r`, which does the same two
stages on hex strings, swaps nothing, and is sent as `0x<8 hex chars>`.
`0xe` is 14 bytes, the length of every model id: `KHOIHGIUCCHHII`, whose digest
begins `3df8c719`. That value is not hard-coded. Each dispatcher derives the hash
from the machine's own SMBIOS type 133 string, walking every
`/sys/firmware/dmi/entries/133-*` entry and taking the first 14 character string,
and falls back to known values only when the firmware publishes none. Reading the
id from SMBIOS is the OEMs' own mechanism, not ours: HP's firmware tool names the
record `FCC type` and copies 14 bytes out of it, in
[traces/smbios-type-133.md](traces/smbios-type-133.md).

The Lenovo `3df8c719` is the fallback every AT dispatcher carries. Each other id
is scoped to the cards its own tool serves: `bb23be7f` to `8086:7560`,
`4909b5a4` to `14c3:4d75`, `93555146` to `14c0:4d75`, `576f0ae0` to `03f0:09c8`.
See [VENDOR-SEQUENCES.md](VENDOR-SEQUENCES.md).

One detail taken from Lenovo rather than assumed, and it is specific to
`event_monitor_at`: that function passes an empty prefix to
`at_send_command_singleline`, then parses the reply with `strtoul` base 16 and
requires 1. `at_send_command_singleline` rejects a reply carrying no value line,
returning `AT_ERROR_INVALID_RESPONSE`, so the modem has to answer with one.
`event_monitor_at_fm350` is not the same: it sends through `send_at_of_mm`, and
upstream's `14c3`, which has unlocked FM350s for years, matches
`^+GTFCCLOCKVER:`.

The dispatchers depart from the vendor in two places, both deliberate:

- `at+cfun=1` is not sent at all. ModemManager sets the power state itself once
  the dispatcher returns 0. For `14c3:4d75` there is direct evidence for that
  id: upstream's `14c3` script has unlocked FM350s for years without it.
- a failure of `at+gtfcclockmodeunlock` or the state read is not fatal here,
  where the vendor `exit(1)`s. The unlock is already effected once
  `at+gtfcclockver` replies 1; those commands complete and read back the state.

## `mbimcli` — Quectel

Vendor path: `setFccUnlock_cs24` → `libmbimtools.so`, whose
`mbim_radio_state_set()` composes two MBIM SET commands:

| uuid | cid | meaning |
|---|---|---|
| `11223344-5566-7788-99aa-bbccddeeff11` | 1 | Quectel service, radio state |
| `a289cc33-bcbb-8b4f-b6b0-133ec2aae6df` | 3 | Basic Connect, radio state |

The first uuid is libmbim's `uuid_quectel` and cid 1 is
`MBIM_CID_QUECTEL_RADIO_STATE` — exactly what `--quectel-set-radio-state=on`
sends. The second is `uuid_basic_connect` with
`MBIM_CID_BASIC_CONNECT_RADIO_STATE`, which only powers the radio up;
ModemManager does that itself, so the dispatchers omit it.

EM05 routes here too. `setFccUnlock_em05` exists in `DPR_Fcc_unlock_service` but
`dlopen()`s `/usr/lib/mbim2sar_em05.so`, which no Lenovo package ships, so that
path is inert and the live EM05 FCC path is the `cs24` one.

## Rolling serial AT port

The `at-gtfcclock` unlock on the RW101R-GL needs the `option` driver bound, which
kernels before 6.18 (and the stable backports) do not do for `33f8:01a8`, `01a9`,
`0301` or `0302`. The installer writes `99-rw101r-serial.rules` for all four and
binds immediately; the dispatcher also binds and re-checks before attempting the
unlock, so a system without the rule still works.

## The US-SIM gate

In every family the country check lives in the `DPR_Fcc_unlock_service` caller
(`GetCountry`, `get_country_code`, `location_is_USA`), never in the message
builder. Omitting it changes nothing about the unlock itself.

## Upstream

These same mechanisms are being submitted to ModemManager and libqmi. State as
of 2026-09-26:

| Merge request | Covers | State |
|---|---|---|
| [libqmi!473](https://gitlab.freedesktop.org/mobile-broadband/libqmi/-/merge_requests/473) | FOXAP service `0xE4` | open |
| [[!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492) | `17cb:0308` Foxconn T99W696 | open |
| [[!1493](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1493) | Rolling `33f8`, EM160R-GL, EM061K | open |
| [[!1496](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1496) | `8086:7560`, Lenovo and Dell methods | open, supersedes [!1141](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1141) |
| [[!1499](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1499) | `14c0:4d75` Dell DW5933e | open, confirmed on hardware |
| [[!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500) | `105b:e0f5`, `105b:e0f9` Dell DW5932e | draft |
| [[!1501](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1501) | `03f0:09c8` HP DRMR-H01 | draft, untested hardware |
| [[!1491](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1491) | Andreas Haerter's `14c3` SMBIOS derivation | open, not ours |

Where upstream ships an unlock for an id, the installer prefers it.
