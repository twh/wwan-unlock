# wwan-unlock

FCC unlock for Lenovo ThinkPad WWAN modems on Linux.
Lenovo cannot release software to unlock WWAN modules in the US for Linux because
the FCC requires onerous red tape to recertify them, even though they are already
certified to run on Windows. This is unacceptable, so we will do it ourselves.

Some ThinkPad WWAN cards ship **FCC-locked**: the radio stays powered down until the
host sends a vendor-specific unlock message after every modem power-on. Lenovo
distributes closed binaries that do this, but they gate the unlock on the SIM's
country and their license forbids modifying them.

This project removes **only** the country gate. Its primary tool, `wwan-orch`,
reimplements just Lenovo's gated orchestrator and then calls Lenovo's **own,
unmodified** worker libraries (bundled in `vendor/lenovo/`) to do the actual
unlock — omitting only the US-SIM check. So the unlock itself is Lenovo's tested
code; the one thing reimplemented is the ~unneeded gate.

A separate, optional tool — **`foxunlock`** — is a fully clean-room unlock for the
Foxconn T99W696 that uses **no Lenovo code at runtime** (it builds and sends the
QMI-over-MBIM message itself). It is not part of the installer; build it by hand
with `make foxunlock`.

## Supported hardware

The installer picks ModemManager's own unlock when one exists (maintained upstream),
and falls back to this implementation otherwise.

| Modem | ID(s) | Handled by |
|---|---|---|
| Foxconn T99W696 (SDX61/62) | `17cb:0308` | module (X1 Carbon Gen 14) |
| Fibocom FM350 | `14c3:4d75` | upstream ModemManager |
| Quectel RM520N-GL | `1eac:1007` | upstream ModemManager |
| Quectel EM061K | `2c7c:6008` | upstream ModemManager |
| Quectel EM160R-GL | `1eac:100d` | module — unverified |
| Quectel EM05-CN | `2c7c:0310` | module (`em05`) — unverified |
| Quectel EM05-G | `2c7c:030a` | module (`em05`) — unverified |
| Fibocom L860R+ | `8086:7560` | module — unverified (reuses upstream Intel script) |
| Rolling RW101R-GL | `33f8:*` | bundled module (`rw101`) — unverified |

Full detail and the RW101 situation: [docs/HARDWARE-STATUS.md](docs/HARDWARE-STATUS.md).
Only `17cb:0308` has been run on real hardware by twh at waynehendricks dot com; the others are
marked `unverified` and the installer warns before using them. Adding or finishing a
module is exactly what [docs/ADDING-HARDWARE.md](docs/ADDING-HARDWARE.md) is for.

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

**Primary path — `wwan-orch` (gateless orchestrator).** Lenovo's unlock/SAR logic
lives in libraries (`libfiisdk` etc.); only their orchestrator *binaries* hold the
US-SIM country gate. `wwan-orch` reimplements just that orchestrator, `dlopen()`s
Lenovo's own unmodified libraries, and calls the same functions **without** the gate:

```
ModuleConnect("/dev/wwan0mbim0")
GetCountry()        <-- the US-SIM gate; wwan-orch does NOT call this
Fox_Attempt()       <-- Lenovo's real, tested unlock
ModuleDisconnect()
```

So the only thing unimplemented is the ~unneeded gate; the actual unlock and SAR
(`Set_RF_Files`) stay Lenovo's tested code. The libraries are bundled unmodified
under their license (`vendor/lenovo/`, see its `NOTICE.md`).

**Separate standalone tool — `foxunlock`.** A fully clean-room FCC unlock for the
Foxconn T99W696 that uses **no Lenovo code at runtime** — it computes the auth hash
and sends the QMI-over-MBIM message itself (service 0xE4, msg 0x5571). It is *not*
part of the installer or the dispatcher; build and run it by hand if you want a
zero-vendor-code path:

```sh
make foxunlock
sudo ./foxunlock -d /dev/wwan0mbim0
```

Full derivation of both: [docs/T99W696-FCC-unlock-findings.md](docs/T99W696-FCC-unlock-findings.md).

## Scope and honesty

- Modules for both verified and unverified hardware are bundled. Unverified
  modules are marked as such, and the installer warns loudly before touching them.
- A failed FCC unlock leaves your radio disabled — recoverable, but don't run
  unverified modules on a machine you can't afford to have offline.
- Our reimplemented code (`wwan-orch`, `foxunlock`, the installer) contains **no
  Lenovo code** and **modifies none of Lenovo's binaries**. Lenovo's own worker
  libraries and data are bundled **unmodified** in `vendor/lenovo/` under the terms
  of their license, which grants the right to use and distribute them unmodified —
  see [vendor/lenovo/NOTICE.md](vendor/lenovo/NOTICE.md).

## License

MIT — see [LICENSE](LICENSE).

Not affiliated with or endorsed by Lenovo, Foxconn, or Qualcomm.
