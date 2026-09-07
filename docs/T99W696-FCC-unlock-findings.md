# Foxconn T99W696 (SDX61/SDX62) FCC Unlock — Reverse-Engineering Findings

Target: Foxconn T99W696 (PCI `17cb:0308`), e.g. Lenovo ThinkPad X1 Carbon Gen 14.
Source of truth: Lenovo `lenovo-wwan-unlock` (latest), binaries `DPR_Fcc_unlock_service`
and `libfiisdk.so.2.2.2` (both non-stripped, with symbols).

## Why stock qmicli doesn't unlock this modem
`qmicli --fox-set-fcc-authentication` targets the **FOX** service (`0xE3`). This modem
takes the unlock on **FOXAP** (`0xE4`); see "Vendor-code evidence" below, which settles
this from the vendor library rather than from the symptom.
`--dms-foxconn-set-fcc-authentication-v2` targets the SDX55 DMS path and returns
`WmsInvalidMessageId`. Transport is not the issue: `qmicli -d /dev/wwan0mbim0` already
tunnels QMI over MBIM, and the firmware-version reads on `0xE3` work fine on that same
node.

## Call chain (open, vendor-provided)
```
fcc-unlock.d/17cb:0308  ->  DPR_Fcc_unlock_service
    main -> setFccUnlock_fxn
        dlopen("/opt/fcc_lenovo/lib/libfiisdk.so.2.2.2")
        ModuleConnect("/dev/wwan0mbim0")
        Fox_Attempt()
            FoxApGetFccLockStatus()
            FoxApSetFccLockStatus()   <-- builds + sends the unlock
            CheckOperatingMode()
        ModuleDisconnect()
```
The US-SIM check (`GetCountry`) lives in DPR_Fcc_unlock_service / Fox_Attempt, NOT in the
message builder. It gates nothing in the crypto.

## The unlock algorithm (FoxApSetFccLockStatus) — decoded from disassembly

Contiguous read of `FoxApSetFccLockStatus` (0xb738) and its callees. Every field
below is pinned to specific instructions, not inferred.

1. **Salt** — 4 chars (`cmp idx,0x3 / jle` loop), each `alphabet[rand() % 46]`.
   Alphabet built on the stack = `abcdefghijklmnopqrstuvwxyz00112233445566778899`
   (a-z then "00112233445566778899", 46 bytes, null-terminated). The salt is
   arbitrary — the modem re-derives from whatever salt we send, so its RNG source
   is irrelevant to correctness.
2. **firmware** = `SplitData(FoxGetFwVersion(0), count=2)` **strcat** `FoxGetFwVersion(2)`
   - `FoxGetFwVersion(i)`: QMI-FOX msg **0x555e** (service 0xE3), request TLV
     `01 01 00 <i>`, response TLV type 1 = version string. i=0 → "FwAndMcfg",
     i=2 → "Apps" (from the failure strings `DmsGetFwAndMcfgVersion fail!` /
     `DmsGetAppsVersion fail!`).
   - `SplitData(str,&len,&count=2)`: recursive `strtok` on `.`; strips the **last
     2 dot-separated fields** (e.g. `FDE.F0.3.2.0.3.AT.002` → `FDE.F0.3.2.0.3`).
     Applied only to the mcfg string; apps is appended raw.
   - libqmi's `QmiFoxFirmwareVersionType` maps `firmware-mcfg`→0, `apps`→2, so
     `qmicli --fox-get-firmware-version=firmware-mcfg|apps` returns the exact
     sub-0 / sub-2 strings.
3. **imei** = `DmsGetImei()` = QMI-DMS Get Device Serial Numbers, TLV **0x11**.
   The IMEI is a real hash field. The literal `011223344556677` in the binary is
   **only the fallback** written to the IMEI buffer when `DmsGetImei` fails — it
   sits inside the `jne` (error) branch at 0xbad4, not the main path. (This was
   the decisive earlier mistake: treating the fallback as a hash component and
   dropping the IMEI.)
4. **magic** = `"FDE2"` — `b_char_value("ighU")`, each byte − 0x23
   (`i`→F `g`→D `h`→E `U`→2). `b_char_value(c) = c ? c-0x23 : 0`. Analog of SySS `FDE1`.
5. **hash input** = `sprintf("%s%s%s%s", firmware, imei, salt, magic)`
   (arg order confirmed at the sprintf call 0xbb8e: rdx=firmware, rcx=imei,
   r8=salt, r9=magic).
6. `Compute_string_md5` = stock `md5_init/update/final` → 16 bytes → lowercase hex.
7. **auth payload** = `sprintf("%s%s", salt, md5hex)` = 36 bytes (4 + 32).

## The wire message (QMIFOXAPSetFccLockStatus → SendMessage → ComposeUniversalQMUXMsg)
Standard QMUX frame, header decoded byte-for-byte:
- QMI service byte (QMUX off 4): **0xE4** (FOXAP — distinct from libqmi's FOX 0xE3,
  which the firmware query uses). Pinned to the vendor library, not to a symptom;
  see "Vendor-code evidence" below.
- message ID (off 9): **0x5571**
- payload (off 13), total **0x2b = 43 bytes**, two TLVs:
  - **TLV 0x01**, len 36 = the auth payload (salt + md5hex)
  - **TLV 0x02**, len 1 = `'0'` (0x30). `Fox_Attempt` calls
    `FoxApSetFccLockStatus(0)` for an unlock → flag `'0'`; `1`→`'1'` = lock.
- Transport: QMI-over-MBIM on `/dev/wwan0mbim0` (libqmi handles the tunnel).

## Clean-room implementation
`foxunlock.c` builds exactly the frame above with stock libqmi-glib, using none
of Lenovo's binaries at runtime. The offline `--emit-frame` output matches the
vendor QMUX frame byte-for-byte (service 0xE4, msg 0x5571, 0x2b payload, both TLVs).

Run:  `sudo ./foxunlock -d /dev/wwan0mbim0`  (reads fw/imei via qmicli, sends).
`--service` overrides the service id for debugging; `227` (`0xE3`) is not expected to
work on this modem, for the reasons below.

## Vendor-code evidence

Everything above is confirmed against the binaries in Lenovo's `lenovo-wwan-unlock`
package (unstripped, with symbols). Addresses are in `libfiisdk.so.2.2.2` unless noted.

| Fact | Where | Evidence |
|---|---|---|
| Unlock is msg `0x5571` on service **`0xE4`** | `QMIFOXAPSetFccLockStatus` `0x12372` | `movb $-0x1c` (= `0xE4`) + `movw $0x5571`, then `SendMessage` |
| A FOX (`0xE3`) twin exists but is **dead code** | `QMIFOXSetFccLockStatus` `0x1213b` | identical but `movb $-0x1d` (= `0xE3`); **no call site** in the library, and no other shipped binary names it for `dlsym` |
| Live unlock path | `Fox_Attempt` `0xa1bb` | calls `FoxApGetFccLockStatus` @`0xa1da`, `FoxApSetFccLockStatus` @`0xa22c`; the latter calls `QMIFOXAPSetFccLockStatus` @`0xbce2`, `0xbd1c` |
| Service byte reaches the QMUX frame | `SendMessage` `0xeff6` | arg1 stored as a byte @`0xf00f`, reloaded @`0xf033` and passed to `ComposeUniversalQMUXMsg` @`0xf03d` |
| Firmware versions from FOX `0xE3` msg `0x555E` | `QMIFOXGetFWVersion` `0x12048` | `movb $-0x1d` + `movw $0x555e`; called 3x from `FoxGetFwVersion` |
| IMEI from DMS `0x02` | `QMIDMSGetDeviceSerialNumbers` `0x11b38` | `movb $0x2`; reached via `DmsGetImei` `0xa29a` |
| magic = `"FDE2"` | `0xb83a`-`0xb84f`, `b_char_value` `0xc059` | writes `69 67 68 55` (`ighU`); transform is `c ? c-0x23 : 0` -> `F D E 2`. Not a stored literal, which is why `strings` finds neither |
| Hash assembled from those | `FoxApSetFccLockStatus` `0xb738` | calls `FoxGetFwVersion` x2, `DmsGetImei`, `SplitData`, `srand`/`rand`, `sprintf` x2, `Compute_string_md5` |
| `17cb:0308` -> this path | `DPR_Fcc_unlock_service` | strings `17cb:0308`, `WWAN device SDX61 found`, `FCC unlock for SDX61 is triggered` |
| Module name / chassis | `configservice_lenovo` | `sar_config_files/cs26/fxn/0304_T99W696_ThinkPad-X1-Carbon_21V7.bin` |

Note the vendor strings say **SDX61**, not SDX62; `SDX62` appears nowhere in the
package. The `FDE1` magic and the Dell DW5932e attribution come from SySS' write-up and
libqmi MR !417, not from this package.
