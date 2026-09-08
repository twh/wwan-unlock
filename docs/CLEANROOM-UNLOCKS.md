# Clean-room FCC unlocks

Every FCC unlock in `modules/` is clean-room: it contains no Lenovo code and
loads none of the bundled libraries at runtime. Each was derived by reading the
vendor's own code path for that specific modem in Lenovo's `lenovo-wwan-unlock`
package — `DPR_Fcc_unlock_service` and the worker library it `dlopen()`s — and
reproducing the messages it sends with stock tooling.

Lenovo's bundled libraries remain in `vendor/lenovo/`, and `wwan-orch` remains
in the tree, **only for SAR**. No unlock uses either.

## The three mechanisms

| Mechanism | Modules | Tool |
|---|---|---|
| `foxunlock` | Foxconn T99W696 | our own QMI-over-MBIM sender |
| `at-gtfcclock` | Rolling RW101R-GL, Fibocom FM350-GL, Fibocom L860R+ | the dispatcher, with `sha256sum` |
| `mbimcli` | Quectel EM160R-GL, EM061K, RM520N-GL, EM05-G, EM05-CN | stock `mbimcli` |

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
`0xE4` yet — that is mobile-broadband/libqmi!473. Once it ships, the dispatcher
can become `qmicli --foxap-set-fcc-authentication`.

## `at-gtfcclock` — Rolling and Fibocom

Vendor paths:

- `fccunlock_rw101` → `libmodemauthRW101.so.1.1`. Resolves only
  `init_modemauth_srvc`; there is no MBIM variant and no device path is passed,
  so one code path serves all five Rolling ids and the library finds the AT port
  itself.
- `fccunlock_fm350_l860` → `libmodemauth.so`. Called with transport selector 1 at
  every call site, for the FM350-GL and the L860R+ alike, which resolves
  `init_modemauth_srvc()` on `/dev/wwan0at0`. The `init_modemauth_srvc_mbim()`
  branch is resolved but never chosen.

They do not all run the same sequence. There are two functions:

`event_monitor_at()`, reached by the Rolling modules and the L860R+:

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

It sends no `at+gtfcclockmodeunlock`, and its status command differs. Nothing on
either path is best effort: every failure branch ends in `exit(1)`, which
terminates `DPR_Fcc_unlock_service` itself.

The challenge is located with `get_dev_code()`, advanced past the `0x` and parsed
with `strtoul` base 16. The response comes from `compute_sha256()`:

```
Sha256_Init -> Sha256_Update(key, 0xe) -> Sha256_Final
Sha256_Init -> Sha256_Update(4) -> Sha256_Update(4) -> Sha256_Final
```

That is `sha256( sha256(key)[0:4] ++ challenge[0:4] )[0:4]`, sent as a decimal.
`0xe` is 14 bytes, the length of the key `KHOIHGIUCCHHII`, whose digest begins
`3df8c719`. That value is not hard-coded: each dispatcher derives the hash from
the machine's own SMBIOS type 133 OEM string and falls back to known values only
when the firmware publishes none. The Lenovo `3df8c719` is the fallback every AT
dispatcher carries; the two Dell values are scoped to the single card each one
names — `4909b5a4` to `14c3:4d75`, `bb23be7f` to `8086:7560`. See
[VENDOR-SEQUENCES.md](VENDOR-SEQUENCES.md).

One detail taken from the vendor rather than assumed: it never matches a
response prefix on `at+gtfcclockver`. The prefix argument is the empty string,
and the reply is parsed with `strtoul` and compared to 1.

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

These same mechanisms are being submitted to ModemManager and libqmi:

- mobile-broadband/libqmi!473 — FOXAP service `0xE4`
- mobile-broadband/ModemManager!1492 — `17cb:0308`
- mobile-broadband/ModemManager!1493 — Rolling, L860R+, EM160R-GL, EM061K

Where upstream ships an unlock for an id, the installer prefers it.
