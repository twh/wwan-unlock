# OEM package inventory

Every vendor package examined, what module it serves, and whether it carries a
host side FCC unlock tool. A package without one cannot yield a model id, which
is what an unlock needs.

Extraction: Lenovo's `lenovo-wwan-unlock` ships plain ELF binaries. Dell's and
HP's are self extracting Windows installers whose payload is appended after the
PE image; carve from the archive signature at the overlay offset and read it
with `bsdtar` (CAB, 7z and zip all work).

## Lenovo

| Package | Module ids | Unlock component | Model ids |
|---|---|---|---|
| `lenovo-wwan-unlock` (`DPR_Fcc_unlock_service` + worker libraries) | `14c3:4d75`, `8086:7560`, `17cb:0308`, `1eac:1007`, `1eac:100d`, `2c7c:6008`, `2c7c:030a`, `33f8:01a4/01a8/01a9/0301/0302` | `DPR_Fcc_unlock_service`, dispatching into `libmodemauth.so`, `libmodemauthRW101.so.1.1`, `libfiisdk.so.2.2.2`, `libmbimtools.so` | `KHOIHGIUCCHHII` (`3df8c719`), `DW5823EFCCLOCK` (`bb23be7f`) via `dkey`; `DW5931EFCCLOCK` present as `dkey_fm350` but unreferenced |

## Dell

| Package | Module | Unlock component | Model id |
|---|---|---|---|
| `Dell-Wireless-5823e-and-Intel-L860-R-LTE-Firmware_CXCR2_WIN_6.0.0.19_A01_02.EXE`, and `..._WDYFN_WIN_6.0.0.21_A02_03.EXE`, identical binary | `8086:7560`, subsystems `1028:5823`, `1028:3a17` | `ModemAuthenticator.exe`, sha256 `8c4c8e84…` | `DW5823EFCCLOCK` (`bb23be7f`) |

## HP

| SoftPaq | sha256 (first 16) | Module ids bound | Host unlock tool |
|---|---|---|---|
| sp109038 | `bdea2c7d8d983607` | USB `03f0:036a` flash, `8087:0af1` (XMM7262 class, USB attached) | none |
| sp138232 | `1c362201f0fc5087` | `8086:7560` SUBSYS `00000000`, `8507103C` | none |
| sp148979 | `d324e875996329b6` | `8086:7360` SUBSYS `00000000`, `8337103C` | none |
| sp151712 | `1ceababbcee0eba5` | `8086:7560` SUBSYS `00000000`, `893B103C`, `8A53103C` | none |
| sp155280 | `b384dd89ca2d46f5` | `8086:7560` SUBSYS `00000000`, `893B103C`, `8A53103C` | none |
| sp173756 | `79283b16cbfae852` | `03f0:09c8` SUBSYS `8E6D103C`, `8E6F103C`; also lists `14c3:4d75` | **`WWANModemAuthenticator.exe`**, sha256 `ef1e4b0253876761…`, classes `CFccLock`, `CHpFccLock`, `CDellFccLock`; model ids `WNCHPCEFCCLOCK`, `DW5931EFCCLOCK` |
| sp174156 | `27c1937cc549a95d` | `14c3:4d75` SUBSYS `14C34D75`, `35001CF8`, `8914103C`, `8A3A103C`, `8C4A103C` | none found; `WwanConfigurator.dll` carries the Intel FCC service GUID and is not yet traced |

## Every hardware id in the HP packages

Taken from the INFs, so this is what each package actually claims. PCI ids are
what ModemManager and the kernel dispatch on; the USB ids are functions of the
same modems (flash mode, GNSS, modem control, and the emulated MBIM interface
the UDE driver presents).

| Id | Subsystems | Package |
|---|---|---|
| PCI `8086:7560` | `0000:0000`, `103c:8507` | sp138232 |
| PCI `8086:7560` | `0000:0000`, `103c:893b`, `103c:8a53` | sp151712, sp155280 |
| PCI `8086:7360` | `0000:0000`, `103c:8337` | sp148979 |
| PCI `14c3:4d75` | `103c:8914`, `103c:8a3a`, `103c:8c4a`, `1cf8:3500`, `14c3:4d75` | sp174156 |
| PCI `14c3:4d75` | none listed | sp173756 (`WwanNetM80.inf`) |
| PCI `03f0:09c8` | `103c:8e6d`, `103c:8e6f` | sp173756 |
| USB `03f0:036a` | flash loader | sp109038 |
| USB `03f0:026a` MI_02, `8087:0af1` MI_00 | GNSS | sp109038, sp148979 |
| USB `8087:0ac9` MI_00 | emulated MBIM interface | sp138232, sp148979 |
| USB `8087:0aca`, `8087:0ada` (+ MI_04) | modem control | sp148979; sp138232, sp151712, sp155280 |
| USB `8087:0b46` MI_00 | GNSS | sp138232, sp151712, sp155280 |

`sp109038` is the Fibocom L850-GL (XMM7262) package, which is USB attached and
contains no FCC related content in any file. `sp173756`'s `WwanNetM80.inf`
installs the unlock as a Windows service
(`ServiceBinary=%11%\WWANModemAuthenticator.exe`) and lists `14c3:4d75`
alongside the M80's own ids, so that binary is the place to look for HP's FM350
sequence. `sp174156` ships no unlock but its uninstaller still removes a
service named `WWAN ModemAuthenticator`, which points the same way.

Notes:

- The Intel packages (`8086:7360`, `8086:7560`) all ship `WinIhvRil.dll`, the
  driver side of the Intel FCC lock MBIM service. It is not an unlock tool: it
  has no model id and computes no response. Its value to us is as an
  independent description of the protocol, recorded in
  [8086-7560-hp.md](8086-7560-hp.md).
- HP's emulated USB MBIM interface is the same one Dell's unlock opens:
  `FiboConfigSrvEx.inf` in sp138232 binds `USB\VID_8087&PID_0AC9&MI_00`.
- `FiboConfigSrv.exe` mentions FCC frequently, but every one of those strings is
  about TAS and SAR regulatory tables (`CTasFccSarCfg_8601_MultiMode`), which is
  RF configuration, not the FCC lock.
