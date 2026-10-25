# `17cb:0308`: Qualcomm's own id, and the Foxconn modules that land on it

Researched 2026-10-25, prompted by Dan Williams reporting on
[ModemManager!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
that his Foxconn T99W373 now enumerates as `17cb:0308` and is sent the T99W696
unlock.

## What `17cb:0308` is

It is not a Foxconn id. `17cb` is Qualcomm and `0308` is its device id for the
SDX6x family. In `drivers/bus/mhi/host/pci_generic.c` (Linux master, read
2026-10-25) the kernel binds it as the generic `qcom-sdx65m` unless the
**subsystem** id says otherwise:

| Subsystem | Module | Kernel config |
|---|---|---|
| `18d7:0301` | Sierra EM929x | `mhi_sierra_em919x_info` |
| `1c5d:2010` | Telit FN990 | `mhi_telit_fn990_info` |
| `1c5d:2015` | Telit FE990A | `mhi_telit_fe990a_info` |
| `105b:*` | Foxconn T99W696, "all variants" | `mhi_foxconn_t99w696_info` |
| anything else | generic SDX65 | `mhi_qcom_sdx65_info` |

ModemManager does not look at the subsystem. `mm_dispatcher_fcc_unlock_run`
builds the script name with `g_strdup_printf ("%04x:%04x", vid, pid)`
(`src/mm-dispatcher-fcc-unlock.c:93`, origin/main), so **every module above
runs the `17cb:0308` script**.

Lenovo's T99W696 is one of them: `17cb:0308`, subsystem `105b:e145`, firmware
`FDE.F0.3.2.0.3.CT.001`, as reported on
[ModemManager!1498](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1498).
Lenovo's `DPR_Fcc_unlock_service` matches the string `17cb:0308` and calls the
module `SDX61`.

## How a T99W373 ends up there

The T99W373 (SDX62) normally enumerates under Foxconn's vendor id: `105b:e0d9`
as Foxconn's own part (kernel comment `T99W373 (sdx62)`), and `105b:e0f5`
(eSIM) or `105b:e0f9` as Dell's DW5932e.

Those ids are chosen per customer by Foxconn's firmware, not fixed in the
silicon. A Foxconn engineer said so on LKML about the T99W175: "Our customer
wants us to use different device ID to separate from each other."

The firmware exposes that choice as `AT^CUSTOMER`. In the OpenWrt DW5934e
thread, `AT+customer?` on a Dell DW5934e returned `+CUSTOMER :33`, and the
same poster wrote, after setting it to 0, "I tried for Customer =0 where the
modem stuck on Qualcomm only also after reboot". That module (SDX72) moved from
Dell's `105b:e11d` to Qualcomm's own `17cb:0309`. Customer 0 is no customer, so
the firmware presents the chip's reference ids, and the setting persists across
reboots.

Dan's T99W373 is the SDX62 equivalent: Qualcomm's SDX6x id `17cb:0308`. He
suspects `AT+CUSTOMER=0` and cannot get it back.

**Not verified:** the exact command Dan ran; what subsystem id his module now
reports; and whether customer 0 removes the FCC lock (a rumour in the OpenWrt
thread says it does, but Dan's module still needs an unlock).

## Dell binds the DW5932e at `17cb:0308`

Dell's DW5932e package (`Dell-Wireless-5932e-and-Qualcomm-Snapdragon-X62-Firmware_GJ2XY_WIN64_3.2.0.22_A09.EXE`,
sha256 `86d47d8846ff4beaa6014f58910940132b7dfdcd57801a2f9ce9e4177dec530e`, a PE
with a 7z overlay at `0x972e00`) treats Qualcomm's id as a normal operating
state of its own module, told apart by subsystem:

| INF | Hardware id | Description |
|---|---|---|
| `MhiHost.inf` | `PCI\VEN_105B&DEV_E0F5&SUBSYS_E0F5105B` | DW5932e-eSIM Snapdragon X62 5G MHI |
| `MhiHost.inf` | `PCI\VEN_105B&DEV_E0F9&SUBSYS_E0F9105B` | DW5932e Snapdragon X62 5G MHI |
| `MhiHost.inf` | `PCI\VEN_105B&DEV_E0F6...`, `DEV_E0FA...` | the two Recovery variants |
| `MhiHost.inf` | `PCI\VEN_17CB&DEV_0308&SUBSYS_E0F5105B` | Snapdragon SDX62 eSIM QcMHI |
| `MhiHost.inf` | `PCI\VEN_17CB&DEV_0308&SUBSYS_E0F9105B` | Snapdragon SDX62 QcMHI |
| `QmuxMdm.inf` | `{B82F6C5F-...}\QcQmux-030817CB` | Snapdragon SDX62 QcQMUX |
| `qcmbbnetadapter.inf` | `{05C9354C-...}\VID_0308&PID_17CB_QcMbbNetAdapter` | Snapdragon SDX62 QcMbbNetAdapter |

So in the `17cb:0308` state Dell's full stack loads: MHI, QMUX, the MBB network
adapter, GNSS, PS auto registration and 5G NR coexistence. It is not a
recovery or flashing mode. The FCC service, `SIMService.exe` 1.0.0.40 installed
as `DW5932e_SIMSVC`, comes from `WWANCONFIG.inf`, which matches the modem's MBIM
firmware id (`MBFW\{30343839-4531-3039-...}` and `...-3043-...`) rather than its
PCI id, so Dell runs the same unlock whichever id the module presents.

This is the discriminator the script needs: on `17cb:0308`, subsystem
`105b:e0f5` or `105b:e0f9` is a DW5932e (T99W373), and Lenovo's T99W696 reports
`105b:e145`. Both Dell's INF and the kernel's MHI table key on the subsystem.

## The unlocks that can arrive at this script

| Module | Magic | Service | Version input | Source | On hardware |
|---|---|---|---|---|---|
| T99W696 | `FDE2` | FOXAP `0xE4`, msg `0x5571` | FOX mcfg minus last two fields, then FOX apps | Lenovo `libfiisdk.so.2.2.2`, [T99W696-FCC-unlock-findings.md](../T99W696-FCC-unlock-findings.md) | ThinkPad X1 Carbon Gen 14 (21V7), Wayne |
| T99W373 | `FDE1` | FOX `0xE3`, msg `0x5571` | Dell: DMS revision fields 1 to 6, then field 9 as `%03d`. SySS: FOX mcfg minus last two, then FOX apps | Dell `SIMService.exe` (our trace, [ModemManager!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500)); SySS from `FoxFlss` and `SIMService.exe` | DW5932e by SySS; DW5932e `105b:e0f5` by the reporter of [issue 1100](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/1100) |
| DW5934e, for comparison | `FDE2` | FOXAP `0xE4` | as DW5932e | `FoxFlss`, per lom in the OpenWrt thread (#52, #56) | reported working (#60) |

Both modules hash the same four inputs in the same order; only the magic and
the service differ.

### The two readings of the T99W373 version produce the same hash

Confirmed by disassembling Dell's `SIMService.exe`, in
[105b-dw5932e-dell.md](105b-dw5932e-dell.md). Issue 1100 gives a real DW5932e
revision: `FDE.F0.3.0.1.1.DT.005.074`, nine fields. Dell's reading gives
`FDE.F0.3.0.1.1` + `074` as one string; the SySS reading this script uses gives
mcfg minus two fields + apps as two strings. They concatenate to the same md5
input, so the two paths compute the same response.

One caveat: Dell forces the suffix to three digits with `%03d`, while the FOX
apps query here uses whatever the modem returns. They agree only while the apps
version is already three digits, which it is in every sample seen (`074`, `068`).
A module reporting a two-digit apps version would hash differently here than
Dell does; reading the DMS revision, as `105b:e0f5` does, avoids the question.

## FOXAP does not identify the module

Dan's proposed patch picks the T99W373 unlock when the modem does not list
FOXAP. That holds for his module but is not a property of the T99W373:

- tesf23 (OpenWrt #7), after reversing `FoxFlss`: "I noticed they used new
  foxap functions instead of fox one for 5932e."
- Dairyman (OpenWrt #23): "the DW5932 can also use the new 0xe4 service and
  this may be used in newer modems."
- lom (OpenWrt #56): the one byte TLV `0x02` is `0x30` for the DW5932e and
  `0x31` for the DW5934e on FOXAP.

So a T99W373 on newer firmware can list FOXAP, and a FOXAP probe alone would
send it the T99W696 unlock with `FDE2`.

## Foxconn's own Linux package

[foxconn-pc/fii_linux](https://github.com/foxconn-pc/fii_linux), `FoxFlss`
1.0.17, closed source. Its dispatcher `Application/FoxFlss/data/DW593Xe` just
runs `FoxFlss` and is installed only as `105b:e0f5`, `105b:e0f9` and
`105b:e11d`. It does not cover `105b:e0d9`, `105b:e11e` or `17cb:0308`, and
`FoxFlss` checks a platform list read from `dmidecode -t 1` SKU (OpenWrt
#48 to #51). The binary has not been downloaded or read here.

## What the dispatcher does

As pushed to
[ModemManager!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
at 29fa5bf8 on 2026-10-25:

1. Walk up sysfs from the MBIM port to the function whose `vendor` and
   `device` read `0x17cb` and `0x0308`, and read its subsystem. Requiring the
   ids means a PCI bridge above the modem can never be read by mistake.
2. Subsystem `0x105b:0xe0f5` or `0x105b:0xe0f9`: the T99W373 unlock, `FDE1`
   over FOX, directly. These are Dell's two subsystems from `MhiHost.inf`.
3. Anything else, including no sysfs link: the T99W696 unlock, `FDE2` over
   FOXAP, as before. If that cannot be sent or is refused, the T99W373 unlock
   once. This is what covers a T99W373 with its customer id cleared whatever
   its subsystem.

Both hashes come from the same reads, so the T99W373 uses SySS's reading of
the version (FOX mcfg minus two fields, then apps). On a T99W696 the script
issues exactly the same `qmicli` calls as before. Checked against a simulated
modem whose `qmicli` stub accepts only an independently computed hash: 40 of
40 under `sh`, `dash`, `bash` and `ksh`. Dan's FOX and `FDE1` path is credited
with `Suggested-by:`.

## What the fii_linux repository shows

Read 2026-10-25 through the GitHub API: history, issues and every text file.
`FoxFlss` itself ships only as a compiled binary and was not disassembled.

- **Publisher.** All nine commits (2025-08-08 to 2026-10-12, v1.0.11 to
  v1.0.17; the history was re-initialised on 2025-08-08) are by Slark Xiao
  `<slark_xiao@163.com>`, who also wrote the kernel's MHI support for
  Foxconn: "Add support for Foxconn T99W373 and T99W368" (13b9b814da2d),
  "Add support for Dell DW5932e" (104a8c5dd943), "Add Foxconn T99W696 modem"
  (0d63055e1406) and "Add support for all Foxconn T99W696 SKU variants"
  (376358bb9770). Answering issues as `SlarkXiao`, he calls it a project
  "responsible to DELL only" and sends people wanting bare modules to
  "Foxconn Sales". So this is Foxconn's Linux tool for the modules it
  supplies to Dell, which SySS had left open.
- **Dell platform gate.** `FoxFlss` unlocks only on Dell system SKUs it
  lists, read from DMI `product_sku` (issue #2 lists `0CB2` to `0D60`), and
  logs `Current platform:0CBE do not support FccLock!` otherwise. It also
  writes a per platform RF tuner table, which is why its maintainer warns
  against faking the SKU. The FCC unlock does not depend on that table: with a
  listed SKU and no RF match it logs `Unlock Fcc Success!` and then
  `There is no matched System SKU`. Our dispatcher writes no RF data.
- **Qualcomm ids are the unconfigured state.** For a DW5934e showing
  `17cb:0309`, he says `[105b:e11d] shall be the expected VID/PID settings`
  (issue #3), the SDX72 counterpart of Dan's T99W373 on `17cb:0308`.
- **The version string.** `FoxFlss` logs `Device Get Software Version:
  FDE.F0.3.0.1.1` then `Device is DW5932e` (issue #2). On that same unit
  ModemManager shows the firmware revision as `FDE.F0.3.0.1.1.SC.003` and
  `068` on two lines, so the module reports the mcfg version and a three
  digit apps version separately. Issue 1100's `FDE.F0.3.0.1.1.DT.005.074` is
  the same two joined with a dot. Both readings of the T99W373 version
  therefore give `FDE.F0.3.0.1.1` plus the three digits.
- **T99W696 subsystems.** Before the wildcard, Foxconn's kernel patch listed
  `105b:e142` (T99W696.01, Lenovo generic), `e143` (.02, X1 Carbon), `e144`
  (.03, X1 2-in-1), `e145` (.04, PRC) and `e146` (.00, Foxconn). None
  collides with Dell's `e0f5` or `e0f9`.
- **FOXAP.** The release notes never mention a service change. The DW5932e
  unlock dates from 1.0.0, the DW5934e one from 1.0.4. The forum report that
  newer `FoxFlss` uses FOXAP for the DW5932e remains unconfirmed here.

## Open questions

- Whether Dan's patched script actually unlocked his module.
- His module's subsystem id: `105b:e0f5`/`e0f9` would make it a DW5932e by
  Dell's own INF; anything else needs its own answer.
- His `--get-service-version-info` list, and the DMS revision against the FOX
  mcfg and apps versions.
- Whether the T99W696 lists FOXAP in `--get-service-version-info` at all.
- The unlock routine inside `SIMService.exe` 1.0.0.40 was not mapped here; the
  sequence for the DW5932e stands on the earlier trace in
  [ModemManager!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500)
  and SySS's public write-up.

## Sources

- Linux `drivers/bus/mhi/host/pci_generic.c`: https://github.com/torvalds/linux/blob/master/drivers/bus/mhi/host/pci_generic.c
- Foxconn on per customer device ids (T99W175): https://lkml.iu.edu/hypermail/linux/kernel/2110.3/06498.html
- T99W373 `105b:e0d9` kernel patch: https://lkml.iu.edu/hypermail/linux/kernel/2205.0/01445.html
- OpenWrt DW5934e thread: https://forum.openwrt.org/t/dw5934e-snapdragon-x72-4g-5g-modem/244169
- SySS, Foxconn FCC Unlock (Jan Wütherich, 2025-09-15): https://blog.syss.com/posts/foxconn-fcc-unlock/
- Foxconn fii_linux: https://github.com/foxconn-pc/fii_linux
- ModemManager issue 1100 (DW5932e on Linux): https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/1100
- Dan Williams on !1492: https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492
