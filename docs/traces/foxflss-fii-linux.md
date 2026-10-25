# FoxFlss: Foxconn's own Linux unlock tool (`fii_linux`)

Mapped 2026-10-25 from the binary in
[foxconn-pc/fii_linux](https://github.com/foxconn-pc/fii_linux),
`Application/FoxFlss/bin/FoxFlss`, release 1.0.17. ELF 64-bit PIE, x86-64,
**not stripped**, sha256 `4ad9f617c113ab54e7008b4707b4dde9c0401780e29f0ca0fa4fa9d0870c5864`,
BuildID `680f6eb1853503abcba65a909b86783bd94bb628`. Static disassembly only; the
binary was never run.

This is the closed tool the SySS write-up and
[105b-dw5932e-dell.md](105b-dw5932e-dell.md) refer to. Because it keeps its
symbols, it is the clearest confirmation of the Foxconn FCC unlock, and it
settles that the DW5932e uses FOX while the DW5934e uses FOXAP.

## What it is

A Dell/Foxconn service for the DW5932e and DW5934e that does far more than FCC
unlock: it also writes per-platform RF calibration, manages SAR and DPR, drives
GNSS, reads UART logs, and reads and writes modem EFS/NV. The function set, from
the symbol table:

- **FCC unlock:** `foxattempt`, `FoxGetFccLockStatus` / `FoxApGetFccLockStatus`,
  `FoxSetFccLockStatus` / `FoxApSetFccLockStatus`, and the QMI layer
  `QMIFOXGetFccLockStatus` / `QMIFOXSetFccLockStatus` (FOX, `0xE3`) and
  `QMIFOXAPGetFccLockStatus` / `QMIFOXAPSetFccLockStatus` (FOXAP, `0xE4`).
- **Hash:** `Compute_string_md5`, `md5_init/update/transform/final`,
  `b_char_value`.
- **Version and identity:** `FoxGetFwVersion`, `DmsGetFwVersion`,
  `DmsGetSoftwareVersion`, `DmsGetDeviceModelID`, `DmsGetImei`, `SplitData`.
- **Platform gate:** `ReadSystemSKU`, `CheckRFSSKUType`, `DLL_CheckRFSSKU`,
  `Find_platform_files`, `GetSingleSKUInfoMapFromPlatformFolder`,
  `FoxGetSingleSKUInfo`.
- **RF calibration:** `Set_RF_Files`, `DLL_SetRFSSKU`, `RFFileInsert`,
  `PraseRFFileName`, `CleanModuleRFData`, `fox_file_encrypt` / `fox_file_decrypt`,
  `fox_crc_calc`, `DLL_SetMIPIInfo_SSKU`, `DLL_SetDPRInfo_SSKU`.
- **SAR / DPR / GNSS / EFS:** `QMISARRFSetState`, `Fox*DprInfo`,
  `FoxGetEFSNVValue` / `FoxSetEFSNVValue`, `Loc*` / `QMILoc*`, UART log readers.

Only the FCC unlock is of interest here; the rest is why the installer calls it
rather than shipping a shell script.

## Top level: `foxattempt` `0x105b7`

1. `ModuleConnect`.
2. Branch on the global `module_type` (`0x226340`), set at connect time from
   `popen("lspci -nn | grep -e '105b:e11d\\|105b:e11e'")`. A match is the
   **DW5934e**; anything else, including the DW5932e and a module whose customer
   id has been cleared, is treated as the **DW5932e**.
3. Read the current lock state (`FoxGetFccLockStatus` for FOX, else
   `FoxApGetFccLockStatus`). Stop unless it is locked.
4. "Start to unlock Fcc Lock":
   - DW5932e: `FoxSetFccLockStatus` (FOX).
   - DW5934e: first `FoxGetFwVersion` apps, `atoi`, and require
     `AP version AP%03d >= AP047` ("meets ... Target version AP047"), then
     `FoxApSetFccLockStatus` (FOXAP). This is the secure-boot firmware gate the
     OpenWrt DW5934e thread hit.

## `FoxSetFccLockStatus` `0xc5cb` (DW5932e, FOX `0xE3`)

Traced end to end:

| step | address | what |
|---|---|---|
| salt alphabet | `0xc654`–`0xc6bc` | the 46 bytes `abcdefghijklmnopqrstuvwxyz00112233445566778899`, built on the stack |
| salt | `0xc772`–`0xc78a` | `time` → `srand` → `rand`; 4 chars, each `alphabet[rand() % 46]` |
| magic bytes | `0xc6d4`–`0xc6e9` | the 4 bytes `i g h T` (`0x69 0x67 0x68 0x54`) |
| version mcfg | `0xc7ea` | `FoxGetFwVersion(0)` ("DmsGetFwAndMcfgVersion"), then `SplitData` strips the trailing fields, `strcpy` |
| version apps | `0xc8c4` | `FoxGetFwVersion(2)` ("DmsGetAppsVersion"), `strcat` onto the mcfg part |
| IMEI | `0xc96d` | `DmsGetImei` |
| magic | `0xc9f3` | `b_char_value` over `ighT` → **`FDE1`** |
| hash input | `0xca4b` | `sprintf(buf, "%s%s%s%s", version, imei, salt, magic)` |
| md5 | `0xca83` | `Compute_string_md5`, lower-case hex |
| payload | `0xca9d` | `sprintf(buf, "%s%s", salt, md5hex)` |
| send | `0xcbae` | `QMIFOXSetFccLockStatus` (FOX service `0xE3`) |

## `FoxApSetFccLockStatus` `0xd467` (DW5934e, FOXAP `0xE4`)

Identical shape, two differences:

- magic bytes are `i g h U` (`0x69 0x67 0x68 0x55`) → `b_char_value` → **`FDE2`**.
- the message is built as TLVs and sent over FOXAP: `0xd956` sets TLV type `0x01`
  with length `0x24` (36) carrying the payload, `0xd9aa` sets TLV type `0x02`
  length 1 carrying one byte `0x30`, then `QMIFOXAPSetFccLockStatus`.

## `b_char_value` `0xf4e4`

```
f4fd: subl $0x23, %eax     ; return c ? c - 0x23 : 0
```

so `i`→`F`, `g`→`D`, `h`→`E`, `T`→`1`, `U`→`2`. This is the same obfuscation
Lenovo's `libfiisdk.so` uses for the T99W696 magic
([T99W696-FCC-unlock-findings.md](../T99W696-FCC-unlock-findings.md)), which
places FoxFlss and Lenovo's library in the same Foxconn code family. No `FDE1`
or `FDE2` appears as a literal, which is why `strings` finds neither.

## The platform gate

`ReadSystemSKU` `0x10bd7` reads the machine's SKU through
`RegGetValueA` `0x10a5e`, which runs a shell command with `popen(..., "r")`; the
binary carries `cat /sys...` fragments and no `dmidecode` literal, so this build
reads the DMI SKU from sysfs rather than shelling out to `dmidecode`. The SKU is
matched against a per-platform folder (`Find_platform_files`,
`GetSingleSKUInfoMapFromPlatformFolder`). The unlock proceeds only on a listed
platform: `Your platform %s support FccLock!` versus
`Current platform:%s do not support FccLock!` and `There is no matched System SKU`.
A listed SKU with no matching RF data still unlocks and logs
`Don't find any RF Files for the %s platform!`, so the RF write and the FCC
unlock are independent.

This gate is why the tool refuses on a non-Dell or unlisted machine, and why the
OpenWrt users faked the SKU. Our dispatcher has no equivalent and writes no RF
data; it only computes and sends the FCC response.

## What this confirms for our work

- **The DW5932e uses FOX (`0xE3`) with `FDE1`; the DW5934e uses FOXAP (`0xE4`)
  with `FDE2`.** Read straight from the two set functions. So
  [ModemManager!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)'s
  routing (Dell DW5932e subsystem → `FDE1`/FOX) is correct, and a FOXAP probe is
  firmware-dependent, not a module property.
- **FoxFlss reads the version the same way
  [!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
  does**: `FoxGetFwVersion` mcfg (minus its trailing fields via `SplitData`) plus
  apps, not the DMS revision that Dell's `SIMService.exe` uses
  ([105b-dw5932e-dell.md](105b-dw5932e-dell.md)). Both tools unlock the same
  DW5932e, which is the strongest evidence that the two version readings produce
  the same md5 input.
- **The hash is exactly ours**: salt from a 46-char alphabet, `%s%s%s%s` of
  version, IMEI, salt, magic, md5 lower-case hex, payload `salt+hex`, one-byte
  `0x30` as the second argument. FoxFlss uses a random salt where our scripts use
  a static one; both work because the modem re-derives from whatever salt is sent.
- **`module_type` comes from `105b:e11d` / `105b:e11e`** (DW5934e). A T99W373 /
  DW5932e on `17cb:0308` with its customer id cleared is not those ids, so
  FoxFlss would treat it as a DW5932e and take the FOX/`FDE1` path, which is the
  path [!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
  falls back to for an unknown subsystem.

## Sources

- [foxconn-pc/fii_linux](https://github.com/foxconn-pc/fii_linux), `FoxFlss` 1.0.17 (binary carved locally, not redistributed here)
- [T99W696-FCC-unlock-findings.md](../T99W696-FCC-unlock-findings.md) for the shared `b_char_value` obfuscation
- [105b-dw5932e-dell.md](105b-dw5932e-dell.md) and [17cb-0308-foxconn.md](17cb-0308-foxconn.md)
- SySS, Foxconn FCC Unlock: https://blog.syss.com/posts/foxconn-fcc-unlock/
