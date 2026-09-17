# Vendor unlock sequences

Every FCC unlock family in Lenovo's `lenovo-wwan-unlock`, traced from
`DPR_Fcc_unlock_service` through the worker library it loads to the message on
the wire. Addresses are from the shipped, unstripped binaries. This is the
reference the clean-room dispatchers in `modules/` are derived from; where one
of them departs from the vendor, the departure is stated and justified here.

## Dispatch

`main` matches the modem's vid:pid and calls one of:

| Family | Function | Library it loads | ids |
|---|---|---|---|
| `fxn` | `setFccUnlock_fxn` | `libfiisdk.so.2.2.2` | `17cb:0308` |
| `cs24` | `setFccUnlock_cs24` | `libmbimtools.so` | `1eac:1007`, `1eac:100d`, `2c7c:6008` |
| `em05` | `setFccUnlock_em05` | `/usr/lib/mbim2sar_em05.so` | `2c7c:030a` |
| `rw101` | `fccunlock_rw101` | `libmodemauthRW101.so.1.1` | `33f8:01a4/01a8/01a9/0301/0302` |
| `rw350` | `fccunlock_rw350` | `libmodemauth.so.1.1` | none in `fcc-unlock.d` |
| `fm350` | `fccunlock_fm350_l860`, selector 1 | `libmodemauth.so` | `14c3:4d75` |
| `l860` | `fccunlock_fm350_l860`, selector 1 | `libmodemauth.so` | `8086:7560` |

`libmodemauth.so` and `libmodemauth.so.1.1` are byte identical.

The US-SIM gate (`GetCountry`, `get_country_code`, `location_is_USA`) sits in
these dispatch functions in every family, never in the message builder.

## `fxn` — Foxconn T99W696, `17cb:0308`

```
setFccUnlock_fxn -> dlopen libfiisdk.so.2.2.2 -> ModuleConnect("/dev/wwan0mbim0")
Fox_Attempt (0xa1bb)
  FoxApGetFccLockStatus   (called 0xa1da)
  FoxApSetFccLockStatus   (called 0xa22c)
  CheckOperatingMode
ModuleDisconnect
```

`FoxApSetFccLockStatus` (0xb738) builds the auth payload:

| Step | Where | Detail |
|---|---|---|
| salt | 0xb738+ | 4 chars from `abcdefghijklmnopqrstuvwxyz00112233445566778899`, `rand() % 46` |
| firmware | `FoxGetFwVersion` x2 | mcfg with last two dot fields stripped by `SplitData`, then apps appended |
| imei | `DmsGetImei` -> `QMIDMSGetDeviceSerialNumbers` (0x11b38, `movb $0x2` = DMS) | TLV 0x11 |
| magic | 0xb83a-0xb84f | bytes `69 67 68 55` (`ighU`) through `b_char_value` (0xc059), `c ? c-0x23 : 0` -> `FDE2` |
| hash input | `sprintf` 0xbb8e, fmt 0x133d0 `"%s%s%s%s"` | firmware, imei, salt, magic |
| digest | `Compute_string_md5` | md5, lowercase hex |
| auth | `sprintf` fmt 0x133d9 `"%s%s"` | salt ++ md5hex, 36 bytes |

Firmware versions come from `QMIFOXGetFWVersion` (0x12048): `movb $0xE3` (FOX),
`movw $0x555e`.

The message is `QMIFOXAPSetFccLockStatus` (0x12372):

```
1238c: movb $-0x1c,-0x7(%rbp)     service 0xE4  (FOXAP)
12390: movw $0x5571,-0x6(%rbp)    message id
       -> SendMessage -> ComposeUniversalQMUXMsg
          TLV 0x01  auth payload, 36 bytes
          TLV 0x02  one byte, '0' unlock / '1' lock
```

`QMIFOXSetFccLockStatus` (0x1213b) is the same message on service `0xE3`. It has
zero call sites, and no other shipped binary names it for `dlsym`.

**Ours:** `foxunlock`, which builds this frame with libmbim and allocates the
`0xE4` client over QMI CTL. Hardware verified.

## `cs24` — Quectel, `1eac:1007`, `1eac:100d`, `2c7c:6008`

```
setFccUnlock_cs24 -> dlopen libmbimtools.so -> dlsym "mbim_fcc_ops"
mbim_fcc_ops = { "v1.0.7", init 0x12981, uninit 0x12a57, fcc_unlock 0x12ae5 }

fcc_unlock (0x12ae5):
  dmidecode_query_lenovo_fcc_string   0x12b76   result logged only
  mbim_radio_state_query(1)           0x12bea
  mbim_radio_state_set(1, 1)          0x12c0a
  mbim_radio_state_query(1)           0x12c24   verify
```

`dmidecode_query_lenovo_fcc_string` reads
`/sys/firmware/dmi/tables/smbios_entry_point` and `DMI`, or `/dev/mem`. At
0x12b7f a NULL result only skips a log block: `jne 0x12bde`, and the NULL path
falls through to the same radio state code. **It does not gate the unlock and
contributes nothing to the message.**

`mbim_radio_state_set` composes two MBIM SET commands:

| uuid | cid | payload | meaning |
|---|---|---|---|
| `11223344-5566-7788-99aa-bbccddeeff11` | 1 | u32 1 | `uuid_quectel`, `MBIM_CID_QUECTEL_RADIO_STATE` |
| `a289cc33-bcbb-8b4f-b6b0-133ec2aae6df` | 3 | u32 1 | `uuid_basic_connect`, `MBIM_CID_BASIC_CONNECT_RADIO_STATE` |

**Ours:** `mbimcli --quectel-set-radio-state=on`, which is the first command.
The second only powers the radio up; ModemManager does that itself once the
dispatcher returns 0.

## `em05` — Quectel EM05, `2c7c:030a`

```
setFccUnlock_em05 -> dlopen "/usr/lib/mbim2sar_em05.so" -> dlsym "fcc_ops"
  on failure: "Failed to load quectel library"
```

That library is in no Lenovo package: not in `lenovo-wwan-unlock`, nor any of its
tarballs. The path is therefore inert as shipped, and `main` dispatches EM05
through `setFccUnlock_cs24` instead.

**Ours:** the `cs24` mechanism, same as the other Quectel modules.

## `rw101` — Rolling RW101R-GL, `33f8:01a4/01a8/01a9/0301/0302`

```
fccunlock_rw101 -> dlopen libmodemauthRW101.so.1.1
                -> dlsym get_country_code, init_modemauth_srvc
detect_wwan_module -> get_usb_wwan_module
     ExecuteCustCmd[5]:  lsusb | grep 'RW101R-GL' | awk '{print $6}' | cut -d: -f2
     sscanf "%x", looked up in the `modules` table -> wireless_module
init_modemauth_srvc: wireless_module == 3 -> "101r unlock called. "
                     -> fcc_at_modem_unlock_101r("/dev/cdc-wdm0")
                     -> init_thread_monitor_101r -> event_monitor_at_101r (0xdd9e)
```

The `modules` table at `0x196c0` decides which case runs:

| usb pid | wireless_module | path |
|---|---|---|
| `4d75` | 1 | FM350, `event_monitor_at_350` |
| `7560` | 2 | 7560 R+, `event_monitor_at` |
| `01a8`, `01a9`, `0301`, `0302` | 3 | RW101R-GL, `event_monitor_at_101r` |

So every Rolling id takes `event_monitor_at_101r`, not `event_monitor_at`.
`01a4` does not appear in that table.

| Order | Command | Send primitive | On failure |
|---|---|---|---|
| 1 | `ate0` | `send_at_of_mm` | `LOGE` then `exit(1)` |
| 2 | `at+gtfcclockgen` | `send_at_of_mm` | `LOGE` then `exit(1)` |
| 3 | `at+gtfcclockver=0x<hex>` | `send_at_of_mm` | `LOGE` then `exit(1)` |
| 4 | `at+cfun=1` | `send_at_of_mm` | `LOGE` then `exit(1)` |
| 5 | `AT+GTFCCEFFSTATUS?` | `send_at_of_mm` | `LOGE` then `exit(1)` |

There is no `at+gtfcclockmodeunlock` here and no `at+gtfcclockstate`, and the
`at+gtfcclockver` reply is never inspected. The verdict comes from the status
read: `strtoul(reply + 0x14, &end, 16)` at `e277`, zero means still locked and
`exit(1)`, non-zero means unlocked and `exit(0)`. The retry tail is
`usleep(100000)` back to the challenge.

Challenge: 8 characters copied with `strncpy` from offset `0x12` of the
`at+gtfcclockgen` reply, which lands just past `+GTFCCLOCKGEN: 0x`.

Response: `compute_sha256_101r` (0x81c2). It is string based and swaps nothing:

```
StrSHA256(model_id)                  -> hex digest
snprintf(buf,   9, "%s", challenge)  -> challenge hex, 8 chars
snprintf(buf+8, 9, "%s", keydigest)  -> key digest hex, 8 chars
hex_to_bin(buf, 8)                   -> those 16 characters back to 8 bytes
StrSHA256(bin, 8)                    -> hex digest
snprintf(out,   9, "%s", ...)        -> first 8 hex chars, returned as text
```

`event_monitor_at_101r` formats that as `0x%s` and sends `at+gtfcclockver=%s`.

**Ours:** commands 1, 2, 3 and 5, omitting `at+cfun=1`, with the same hex
response and the same status test.

## `fm350` — Fibocom FM350-GL, `14c3:4d75`

```
fccunlock_fm350_l860(edi=1, esi=module) -> dlopen libmodemauth.so
  selector 1 -> init_modemauth_srvc("/dev/wwan0at0")
  selector 2 -> init_modemauth_srvc_mbim("/dev/wwan0mbim0")   never selected
init_modemauth_srvc: cmpl $0x1 -> "FM350 unlock called. " -> fcc_at_modem_unlock_fm350
                                  -> event_monitor_at_fm350 (0xbf05)
```

| Order | Command | Send primitive | On failure |
|---|---|---|---|
| 1 | `at+gtfcclockgen` | `send_at_of_mm` | `LOGE` then `exit` |
| 2 | `at+gtfcclockver=%lu` | `send_at_of_mm` | `LOGE` then `exit` |
| 3 | `at+cfun=1` | `send_at_of_mm` | `LOGE` then `exit` |
| 4 | `AT+GTFCCEFFSTATUS?` | `send_at_of_mm` | `LOGE` then `exit` |

**No `at+gtfcclockmodeunlock` on this path**, and the status command differs from
the `event_monitor_at` one.

Response: `compute_sha256_fm350` (0x6dbd).

```
6e00: shrl $0x18 -> movb %al, -0x30(%rbp)    MSB at the lower address
6e24:              movb %al, -0x2d(%rbp)     LSB at the higher
```

**Big endian**, and the response side reverses digest[0..3] into the out buffer
before reading it as a u32, which is also big endian.

**Ours:** commands 1, 2 and 4, omitting `at+cfun=1`. Big endian, unswapped.

## `l860` — Fibocom L860R+, `8086:7560`

Same dispatch function as `fm350`, same selector 1, same library, but:

```
init_modemauth_srvc: cmpl $0x2 -> "7560 R+ unlock called." -> fcc_at_modem_unlock
                                  -> event_monitor_at (0xb99b)
```

That is the same five command sequence as `rw101`, with the same
log-then-`exit(1)` on every failure (`ba58`, `bb54`, `bc2d`, `bca9`, `bd57`) and
the same `usleep` retry tail at `bda0`, and `compute_sha256`, so **little
endian**. `ApprovedHWIDS` holds `8086:7560`; `ApprovedHWIDS_FM350` holds
`14c3:4d75`.

**Ours:** the five commands except `at+cfun=1`, both ends swapped.

## `rw350` — Rolling RW350

```
fccunlock_rw350 -> dlopen libmodemauth.so.1.1 (identical to libmodemauth.so)
                -> dlsym get_country_code, init_modemauth_srvc
                -> init_modemauth_srvc("/dev/wwan0mbim0")
```

`libmodemauth.so`'s `init_modemauth_srvc` has two device cases, 1 (FM350) and 2
(7560 R+), so the RW350 resolves to one of them at runtime through the device
type getter. The separate `libmodemauthRW101.so.1.1` has a third case, 3, for
the RW101R-GL. No `33f8` RW350 id appears in Lenovo's `fcc-unlock.d`, and this
repo ships no module for it, so which case it takes has not been established.

## The vendor id hash

For every AT family the response is keyed on four bytes: `sha256` of a "model
id" string, truncated. Neither OEM derives that value from SMBIOS on hardware
its own tool recognises.

Lenovo's `libmodemauth.so` and `libmodemauthRW101.so.1.1` both read SMBIOS type
133 and then throw the result away:

```
configure_wwan_sec_key -> get_wwan_config_id (dmidecode -t 133) -> set_dev_code
init_modemauth_srvc    -> strstr_s(manufacturer, "Dell")
                            found     -> devCode = dkey ("DW5823EFCCLOCK")
                            not found -> devCode = "KHOIHGIUCCHHII"
```

`configure_wwan_sec_key` runs first, so whatever the type 133 read produced is
overwritten before `get_dev_code` hashes it. The manufacturer string comes from
`dmidecode -t 1 | grep 'Manufacturer'`, entry 1 of the library's `AllowedCMDS`
table.

Dell's tools invert that order. `ModemAuthenticator.exe` runs
`DellFccLock::DellFccCheck` first, which is a PC identity registry check, a
module HWID check and a SKU registry check. When those pass it copies its own
constant into the model id buffer and never reads SMBIOS at all. Only when they
fail does it call `GetSystemFirmwareTable` with the `RSMB` provider, and if that
finds nothing it logs `Not find SMBIOS FCC Type` and carries on with the buffer
left empty.

| model id | sha256[0:4] | where it comes from | carried by |
|---|---|---|---|
| `KHOIHGIUCCHHII` | `3df8c719` | Lenovo's libraries, non-Dell branch | every AT dispatcher |
| `DW5823EFCCLOCK` | `bb23be7f` | Dell `ModemAuthenticator.exe`, DW5823e | `8086:7560` only |
| `DW5931EFCCLOCK` | `4909b5a4` | Dell `IntelWWANModemAuthenticator.exe`, DW5931e | `14c3:4d75` only |

Dell names the constant after the card and ships one per package, so a Dell
value belongs only in the dispatcher for the card it names. Lenovo uses a single
id for every module it ships. Chapter 17.4 of Fibocom's public FM350 AT command
manual documents the same string as the machine's "model id in bios", with NVM
hash `0x3d,0xf8,0xc7,0x19`.

Deriving the value from SMBIOS type 133 is **our** choice, not theirs. A
dispatcher cannot repeat Dell's registry checks, and it runs on machines neither
tool would recognise. Where the firmware publishes the model id as the first
string of a type 133 record, hashing it gives the right answer without guessing,
and the constants above remain as the fallback for firmware that publishes none.
Upstream !1491 takes the same approach for `14c3`.

## Where ours departs from the vendor

| Departure | Applies to | Why |
|---|---|---|
| `at+cfun=1` omitted | all AT families | ModemManager sets the power state itself once the dispatcher returns 0. For `14c3` there is also direct evidence for that id: upstream's `14c3` script has unlocked FM350s for years without it |
| trailing commands non-fatal | `event_monitor_at` families | the vendor `exit(1)`s if `at+gtfcclockmodeunlock` or the state read fails; the unlock is already effected once `gtfcclockver` replies 1, so ours logs and returns 0 |
| Basic Connect radio state omitted | Quectel | same reason |
| US-SIM gate omitted | all | it is in the dispatch function, not the message |
| `dmidecode_query_lenovo_fcc_string` omitted | Quectel | its result is logged and never used |
