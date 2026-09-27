# Coverage: every module, its unlock, and where it stands

The goal of this project is one unlock for every WWAN module Linux can bind, so
this file is the checklist. For each PCI or USB id it records which OEMs ship
that module, where the unlock sequence was read from, whether ModemManager
carries it, whether this repo carries it, and what is still missing.

A dispatch id is not an OEM id. Both the kernel and ModemManager match vendor
and product only, so every OEM shipping the same module lands on the same
script, and a script that implements only one OEM's sequence is incomplete for
the rest.

## Modules

| Id | Module | OEMs shipping it | Sequence source | ModemManager | This repo |
|---|---|---|---|---|---|
| `8086:7560` | Intel L860R+ | Lenovo `1cf8:*`, Dell `1028:5823`/`3a17`, HP `103c:8507`/`893b`/`8a53` | Lenovo `libmodemauth.so`; Dell `ModemAuthenticator.exe`; HP ships none | [!1496](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1496), both methods | `modules/8086:7560`, both methods |
| `14c3:4d75` | Fibocom FM350-GL, Rolling RW350R-GL | Lenovo `1cf8:3500`, Dell `1028:5931`, HP `103c:8914`/`8a3a`/`8c4a` | Lenovo `libmodemauth.so` `event_monitor_at_fm350`; Dell `DW5931EFCCLOCK` = `4909b5a4`; HP `WNCHPCEFCCLOCK` = `576f0ae0`, both over the Intel MBIM service | in tree, plus [!1491](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1491), Lenovo's key only | `modules/14c3:4d75`, Lenovo's key only |
| `14c0:4d75` | Dell DW5933e | Dell `1028:5933`/`5966` | Dell `WWANModemAuthenticator.exe` | [!1499](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1499), hardware confirmed | **missing** |
| `03f0:09c8` | HP DRMR-H01 (WNC M80) | HP `103c:8e6d`/`8e6f` | HP `WWANModemAuthenticator.exe`, `CHpFccLock` -> `WNCHPCEFCCLOCK` = `576f0ae0` | [!1501](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1501) (draft) | **missing** |
| `8086:7360` | Intel XMM7360 | Lenovo, HP `103c:8337` | XMM RPC; HP ships no unlock | in tree | **missing** |
| `17cb:0308` | Foxconn T99W696 (SDX61) | Lenovo | Lenovo `libfiisdk.so.2.2.2` | [!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492) | `modules/17cb:0308` |
| `105b:e0f5`, `105b:e0f9` | Dell DW5932e | Dell | Foxconn magic string over QMI | [!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500) (draft) | **missing** |
| `105b:e0ab`, `105b:e0c3`, `05c6:90d5` | Foxconn T77W968 and kin | several | upstream contributors | in tree | not needed here |
| `33f8:01a4/01a8/01a9/0301/0302` | Rolling RW101R-GL | Lenovo | Lenovo `libmodemauthRW101.so.1.1` `event_monitor_at_101r` | [!1493](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1493) | `modules/33f8:01a4`, `33f8:0301` |
| `1eac:1007`, `1eac:100d`, `2c7c:6008` | Quectel RM520N-GL, EM160R-GL, EM061K | Lenovo | Lenovo `libmbimtools.so` `setFccUnlock_cs24` | `1eac:1007` and `2c7c` in tree; `1eac:100d`, `2c7c:6008` in [!1493](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1493) | `modules/1eac:1007`, `1eac:100d`, `2c7c:6008` |
| `2c7c:030a`, `2c7c:0310` | Quectel EM05-G, EM05-CN | Lenovo | Lenovo, EM05 DPR branch | `2c7c:030a` in tree | `modules/2c7c:030a`, `2c7c:0310` |
| `1199:*`, `413c:81a3/81a8`, `03f0:4e1d` | Sierra and Dell rebadges | several | upstream contributors | in tree | not needed here |
| `03f0:026a` | Fibocom L850-GL (XMM7262), USB | HP | **no FCC content in HP's package at all** | none | none |

## Gaps in this repo

Ordered by how much is already known, so the cheapest first:

1. **`14c0:4d75` Dell DW5933e.** Fully traced and confirmed on hardware by a
   reporter, shipping as [!1499](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1499). Porting it here is a transcription job.
2. **`105b:e0f5`, `105b:e0f9` Dell DW5932e.** Traced, shipping as [!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500).
3. **`03f0:09c8` HP DRMR-H01.** Traced, shipping as [!1501](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1501). HP's
   `WWANModemAuthenticator.exe` is the source, model id `WNCHPCEFCCLOCK`.
4. **`8086:7360` Intel XMM7360.** Upstream has a script; this repo has no
   module. The XMM RPC transport is unlike anything else here.
5. **`14c3:4d75` for Dell and HP.** This repo, and upstream, implement
   Lenovo's AT sequence only. The shared authenticator traced in
   [traces/hp-wwanmodemauthenticator.md](traces/hp-wwanmodemauthenticator.md)
   binds `14c3:4d75` and hashes `DW5931EFCCLOCK` (`4909b5a4`) on a Dell
   machine and `WNCHPCEFCCLOCK` (`576f0ae0`) on an HP one, both over the Intel
   FCC lock MBIM service rather than AT. This is the same shape of problem
   `8086:7560` just solved, and the largest remaining gap.

## What each OEM's tooling looks like

- **Lenovo** ships one service, `DPR_Fcc_unlock_service`, which dispatches by
  machine and module into a worker library and does everything over AT, except
  the Quectel modules which go over MBIM. Traced in
  [VENDOR-SEQUENCES.md](VENDOR-SEQUENCES.md) and [traces/](traces/README.md).
- **Dell** ships one small authenticator executable per module package, each
  carrying a model id named after the card, and drives the Intel FCC lock MBIM
  service.
- **HP** ships an unlock only for some modules. Where it does, it is the same
  shared `WWANModemAuthenticator.exe` Dell's packages use, installed as a
  service by the network adapter INF, with an `CHpFccLock` class alongside
  `CDellFccLock`. For its Intel modules it ships no unlock at all. See
  [traces/oem-packages.md](traces/oem-packages.md).

## Rule for adding an OEM to an existing module

Read that OEM's own tool for that exact module. Do not carry a model id across
modules: Dell names its ids after the card (`DW5823EFCCLOCK` for the L860R+,
`DW5931EFCCLOCK` for the FM350, `DW5933EFCCLOCK` for the DW5933e), and HP's
`WNCHPCEFCCLOCK` belongs to the WNC M80 alone. Where an OEM ships no unlock,
say so and let the script fall through to the other methods, as `8086:7560`
does for HP.

## Where to resume

Read in this order: this file for what is and is not covered,
[traces/README.md](traces/README.md) for the per module traces and the tools,
then [VENDOR-SEQUENCES.md](VENDOR-SEQUENCES.md) for the OEM sequences and where
these dispatchers depart from them.

Facts that took the longest to establish, so they are not re-derived:

- A PCI id is not an OEM id. Both the kernel and ModemManager dispatch on vendor
  and product only, so every OEM shipping a module lands on one script.
- The model id is a 14 character string in an SMBIOS type 133 record, type 133
  has two shapes, and a machine can carry both in either order:
  [traces/smbios-type-133.md](traces/smbios-type-133.md).
- A set on the Intel FCC lock service with `ResponsePresent = 0` asks for a
  challenge and `1` submits a response, confirmed from Dell's tool and from HP's
  driver independently.
- `xxd` is not available by default anywhere; `hex_to_bin()` replaces it.
- ModemManager kills a dispatcher after five seconds, so every read that can
  stall needs a bound.

Last reviewed 2026-09-26, against the seven HP SoftPaqs, Dell's DW5823e and
DW5933e packages, and Lenovo's `lenovo-wwan-unlock`.
