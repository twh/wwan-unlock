# Adding hardware support

A "module" is one directory under `modules/`, named for the modem's PCI or USB id
in `<vendor>:<device>` form, lowercase — e.g. `modules/17cb:0308/`. The installer
detects hardware by matching those directory names against every id present on the
PCI and USB buses, so adding support means adding a directory.

```
modules/<vendor>:<device>/
├── module.conf      # metadata (sourced by install.sh)
└── fcc-unlock.sh    # the dispatcher ModemManager executes
```

## Before you start: check upstream

Most Lenovo WWAN cards are already handled by ModemManager itself:

```sh
ls /usr/share/ModemManager/fcc-unlock.available.d/
```

If your id (or its vendor-wide script) is there, you don't need a module here —
enable the upstream one instead. This project targets cards upstream doesn't cover.

## Do you need a C helper?

Usually **no**. Stock tooling already exposes most vendor unlock paths:

| Mechanism | Stock command |
|---|---|
| Qualcomm DMS FCC auth | `qmicli --dms-set-fcc-authentication` |
| Foxconn DMS (v1/v2) | `qmicli --dms-foxconn-set-fcc-authentication{,-v2}` |
| Foxconn FOX service `0xE3` | `qmicli --fox-set-fcc-authentication` |
| Foxconn FOXAP service `0xE4` | `qmicli --foxap-set-fcc-authentication` (libqmi 1.40+) |
| Quectel MBIM radio state | `mbimcli --quectel-set-radio-state=on` |
| Rolling / Fibocom challenge | `at+gtfcclockgen` then `at+gtfcclockver=<n>` |
| Intel mutual authentication | `mbimcli --help-intel-mutual-authentication` |
| AT over MBIM | `mbimcli --help-intel-at-tunnel` |

Every module in this repo uses one of these, so a new one almost certainly can
too: write a **shell-only dispatcher** and set `MODULE_UNLOCK`.

Work out which by reading the vendor's own path for your id in Lenovo's
`DPR_Fcc_unlock_service` — find the `setFccUnlock_*` or `fccunlock_*` it
dispatches to, then read the worker library it `dlopen()`s to see the messages it
actually sends. [CLEANROOM-UNLOCKS.md](CLEANROOM-UNLOCKS.md) shows that worked
through for all three mechanisms already in the tree, including the addresses.

Reproduce what you find with stock tooling. Do not assume a mechanism carries
across from a similar modem — read the library for the modem you have.

`wwan-orch` is not an unlock path. It exists only for RF/SAR, where
`MODULE_SAR_FAMILY` selects the family.

## `module.conf`

```sh
MODULE_NAME="Vendor Model (chipset)"
MODULE_IDS="1234:5678"            # one or more vendor:device ids (space separated)
MODULE_BUS="pci"                  # pci | usb
MODULE_VENDOR_SEQ="..."           # the vendor path this was derived from
MODULE_UNLOCK="mbimcli"           # clean-room unlock: foxunlock|at-gtfcclock|mbimcli
MODULE_DISPATCHER="fcc-unlock.sh" # the dispatcher ModemManager runs
MODULE_LENOVO_SAR="yes"           # optional: run wwan-orch --sar after unlock
MODULE_SAR_FAMILY="cs24"          # SAR only: fxn|cs24|rw101|rw350|fm350|l860|em05
MODULE_NOTES="How the unlock works, and why a helper is or isn't needed."
```

`MODULE_VENDOR_SEQ` is the important field. It records the vendor path the
unlock was derived from — the `setFccUnlock_*` or `fccunlock_*` that
`DPR_Fcc_unlock_service` dispatches to, the worker library it loads, and the
message that library composes. Anyone reviewing the module should be able to
follow it back into the binaries. Do not add a module without it.

## The dispatcher contract

ModemManager runs the dispatcher as:

```
<script> <dbus-path> <control-port> [<port>...]
```

It must:

1. Resolve the right control port from the argument list (usually the MBIM one).
2. Perform the unlock.
3. **Exit 0 only on success.** ModemManager retries on non-zero.
4. **Never block.** MM's fcc-unlock is synchronous and it force-kills a stuck
   dispatcher (`forcing exit on fcc unlock operation`) then retries — a dispatcher
   that hangs turns into a retry loop and the radio never comes up. Keep the whole
   run well under ~8 seconds and put a hard timeout on anything that can stall.
5. **Log its own result.** ModemManager logs *nothing* on success, so without this
   there is no way to tell "ran and succeeded" from "never ran". Use
   `logger -t fcc-unlock-<something>`.

`modules/17cb:0308/fcc-unlock.sh` is a working reference for all five.

## Testing before you submit

```sh
./install.sh --detect                 # does detection see your card?
sudo ./install.sh --id <your:id>
sudo mmcli -m any --reset             # force a power-cycle -> re-lock -> dispatcher runs
sleep 25
journalctl -t <your-tag> -b
mmcli -m any | grep -iE 'state:|power state'
```

A reboot is the honest final test: it guarantees the modem loses power and comes
up FCC-locked, which is the condition the dispatcher actually has to handle.

## Submitting

Open a PR with the module directory and, in the description:

- the machine(s) and modem firmware you tested on
- the `journalctl` output showing a successful unlock
- how you derived the algorithm (so it can be reviewed independently)

Please don't submit vendor binaries, or code copied out of them. Protocol facts —
message ids, TLV layouts, hash constructions — are fine and are what this project
is built from. Vendor object code is not, and several vendor licenses explicitly
forbid redistributing modified binaries.
