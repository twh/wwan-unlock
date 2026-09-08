# wwan-unlock

FCC unlock for Lenovo ThinkPad WWAN modems on Linux.
Lenovo cannot release software to unlock WWAN modules in the US for Linux because
the FCC requires onerous red tape to recertify them, even though they are already
certified to run on Windows. This is unacceptable, so we will do it ourselves.

When a WWAN radio is FCC locked the radio stays powered down until the
host sends a vendor-specific unlock message after every modem power-on. Lenovo
distributes closed binaries that do this, but they gate the unlock on the SIM's
country and their license forbids modifying them.

This project removes **only** the country gate. **Every FCC unlock here is
clean-room**: each was derived by reading Lenovo's own code path for that modem
and reproducing the messages it sends with stock tooling, so no Lenovo code runs
during an unlock. Lenovo's bundled libraries and `wwan-orch` remain in the tree
for RF/SAR provisioning only. Full derivation, per modem, with the addresses it
came from: [docs/CLEANROOM-UNLOCKS.md](docs/CLEANROOM-UNLOCKS.md).

## Supported hardware

The installer picks ModemManager's own unlock when one exists (maintained upstream),
and falls back to this implementation otherwise.

| Modem | ID(s) | Unlock | Derived from |
|---|---|---|---|
| Foxconn T99W696 (SDX61) | `17cb:0308` | `foxunlock` | `setFccUnlock_fxn` → `libfiisdk` |
| Rolling RW101R-GL | `33f8:0301/01a4/01a8/01a9/0302` | `at-gtfcclock` | `fccunlock_rw101` → `libmodemauthRW101` |
| Fibocom FM350-GL | `14c3:4d75` | `at-gtfcclock` | `fccunlock_fm350_l860` → `libmodemauth` |
| Fibocom L860R+ | `8086:7560` | `at-gtfcclock` | `fccunlock_fm350_l860` → `libmodemauth` |
| Quectel EM160R-GL | `1eac:100d` | `mbimcli` | `setFccUnlock_cs24` → `libmbimtools` |
| Quectel RM520N-GL | `1eac:1007` | `mbimcli` | `setFccUnlock_cs24` → `libmbimtools` |
| Quectel EM061K | `2c7c:6008` | `mbimcli` | `setFccUnlock_cs24` → `libmbimtools` |
| Quectel EM05-G | `2c7c:030a` | `mbimcli` | `setFccUnlock_cs24` → `libmbimtools` |
| Quectel EM05-CN | `2c7c:0310` | `mbimcli` | `setFccUnlock_cs24` → `libmbimtools` |

That is every id in Lenovo's own `fcc-unlock.d` list, plus the two EM05 variants.
Each row's unlock is the message that vendor path composes, reproduced with stock
tooling — the derivation, down to the instruction, is in
[docs/CLEANROOM-UNLOCKS.md](docs/CLEANROOM-UNLOCKS.md) and
[docs/HARDWARE-STATUS.md](docs/HARDWARE-STATUS.md).

Where upstream ModemManager already ships an unlock for an id, the installer
prefers it. These same mechanisms are being upstreamed: libqmi!473,
ModemManager!1492 and ModemManager!1493.

## Requirements

- ModemManager 1.22+ and `libmbim` (present on any modern desktop Linux)
- `build-essential`, `pkgconf`, `libmbim-glib-dev`, `libqmi-glib-dev` (for the helper)

```sh
sudo apt install build-essential pkgconf libmbim-glib-dev libqmi-glib-dev
```

## Install

```sh
git clone https://github.com/<you>/wwan-unlock
cd wwan-unlock
./install.sh --detect          # what do I have?
sudo ./install.sh              # detect, build, install
```

Force a specific module, or list what's bundled:

```sh
./install.sh --list
sudo ./install.sh --id 17cb:0308
sudo ./install.sh --uninstall
```

The installer builds the helper into `/usr/local/lib/wwan-unlock/` and installs a
dispatcher at `/etc/ModemManager/fcc-unlock.d/<id>`. ModemManager then invokes it
automatically on every modem power-on — boot, resume, or modem reset. Any existing
entry is backed up to `<id>.orig` and restored on uninstall.

## Verify

ModemManager logs *nothing* on a successful FCC unlock (it only warns on failure),
so the dispatcher logs its own result:

```sh
journalctl -t fcc-unlock-foxconn -b
mmcli -m any | grep -iE 'state:|power state'
```

A good run looks like:

```
invoked: modem=/org/freedesktop/ModemManager1/Modem/0 port=/dev/wwan0mbim0
  FCC unlock: SUCCESS
result: rc=0 elapsed=0s
```

## SAR / RF configuration

RF/SAR is applied the same way as the unlock: **gate-free, via `wwan-orch`**. For
the Foxconn module the installer runs `wwan-orch --sar`, which `dlopen()`s Lenovo's
`libfiisdk` and calls its own `Set_RF_Files` (chassis-matched from the bundled SAR
tables) with the US-SIM SAR gate omitted. `Set_RF_Files` does its own
compare-and-skip, so on an already-provisioned modem it is a no-op. A boot-time
oneshot (`wwan-sar.service`) re-checks on each boot.

SAR is implemented gate-free for **every family** — Foxconn (`Set_RF_Files`),
FM350/L860/RW101/RW350 (`configservice_*`), Quectel `cs24` (EM160/EM061K/RM520, via
`setSARConfig_common`), and Quectel `em05` (EM05-CN + EM05-G, via `set_sar_value`'s
EM05 `mbim_set_dprconfig` branch). All reuse the bundled Lenovo libraries; the EM05-G
DPR band tables (`DPRConfig.xml`) are extracted at install from Lenovo's own
unmodified `configservice_lenovo`. Full mechanism map:
[docs/configservice_lenovo-map.md](docs/configservice_lenovo-map.md). SAR data
persists in modem NV/EFS, so on a modem that was ever provisioned (e.g. ran Windows
once) this changes nothing.

Skip it with `--no-sar` (unlock only); re-apply it alone with `--sar-only`.

## How it works

**Every unlock is clean-room.** For each modem, Lenovo's own code path was read
out of `DPR_Fcc_unlock_service` and the worker library it loads, and the messages
it sends are reproduced here with stock tooling. No Lenovo code runs during an
unlock. Three mechanisms cover all ten modules:

- **`foxunlock`** — Foxconn T99W696. Computes the auth hash and sends the
  QMI-over-MBIM message itself (service `0xE4`, msg `0x5571`). Built and
  installed by the installer.
- **`at-gtfcclock`** — Rolling RW101R-GL, Fibocom FM350-GL and L860R+. The
  `at+gtfcclockgen` / `at+gtfcclockver` challenge/response, computed in the
  dispatcher with stock `sha256sum`.
- **`mbimcli`** — every Quectel. `mbimcli --quectel-set-radio-state=on`, which is
  the Quectel-service MBIM command the vendor library sends.

Where each came from in the vendor binaries, down to the instruction:
[docs/CLEANROOM-UNLOCKS.md](docs/CLEANROOM-UNLOCKS.md). The Foxconn derivation in
particular: [docs/T99W696-FCC-unlock-findings.md](docs/T99W696-FCC-unlock-findings.md).

The RW101R-GL answers its FCC challenge on a `ttyUSB` port rather than the wwan
AT service. That needs the `option` driver bound to the device; Linux 6.18 added
`33f8:01a8`, `01a9`, `0301` and `0302` to its id table (also in the stable
backports), and on an earlier kernel the installer adds a udev rule to bind it.
See [docs/HARDWARE-STATUS.md](docs/HARDWARE-STATUS.md).

**`wwan-orch` — RF/SAR only.** Lenovo's SAR logic lives in their libraries
(`libfiisdk`, `configservice_*`); only their orchestrator *binaries* hold the
US-SIM gate. `wwan-orch` reimplements just that orchestrator, `dlopen()`s
Lenovo's own unmodified libraries and calls the same functions without the gate.
It is not used by any unlock. The libraries are bundled unmodified under their
license (`vendor/lenovo/`, see its `NOTICE.md`).

## Scope

- Every module records the vendor path it was derived from in
  `MODULE_VENDOR_SEQ`, and the installer prints it before touching anything.
- A failed FCC unlock leaves your radio disabled — recoverable with
  `--uninstall`, but not something to try on a machine you can't afford offline.
- Every unlock is clean-room and loads no Lenovo library at runtime. Our code
  (`foxunlock`, the dispatchers, `wwan-orch`, the installer) contains **no Lenovo
  code** and **modifies none of Lenovo's binaries**. Lenovo's own worker libraries
  and data are bundled **unmodified** in `vendor/lenovo/`, used only for RF/SAR,
  under the terms of their license, which grants the right to use and distribute
  them unmodified — see [vendor/lenovo/NOTICE.md](vendor/lenovo/NOTICE.md).

## License

MIT — see [LICENSE](LICENSE).

Not affiliated with or endorsed by Lenovo, Foxconn, or Qualcomm.
