# Foxconn T99W696 (SDX61/SDX62) FCC Unlock — Reverse-Engineering Findings

Target: Foxconn T99W696 (PCI `17cb:0308`), e.g. Lenovo ThinkPad X1 Carbon Gen 14.
Source of truth: Lenovo `lenovo-wwan-unlock` (latest), binaries `DPR_Fcc_unlock_service`
and `libfiisdk.so.2.2.2` (both non-stripped, with symbols).

## Why raw qmicli failed
`qmicli --fox-set-fcc-authentication` returns `MalformedMessage` because the unlock is
sent as **QMI tunneled over MBIM** on `/dev/wwan0mbim0`, not as a raw QMI message on the
QMI port. The service/message identifiers also matter (below).

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
- QMI service byte (QMUX off 4): **0xE4** (FOXAP — distinct from libqmi FOX 0xE3;
  the firmware query uses 0xE3). Sending 0x5571 to 0xE3 returns QMI error 1
  (MALFORMED_MSG) — exactly the earlier symptom.
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
If service 0xE4 is refused by libqmi/modem, retry `--service 227` (0xE3).
