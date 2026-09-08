# Hardware status

Every FCC unlock in `modules/` is clean-room and derived from Lenovo's own SDK.
For each modem the vendor's code path was read out of `DPR_Fcc_unlock_service`
and the worker library it `dlopen()`s, and the messages it sends are reproduced
with stock tooling. No Lenovo library is loaded during an unlock; the bundled
libraries and `wwan-orch` are used for RF/SAR only.

Every claim below is pinned to the (non-stripped) vendor binaries. The full
derivation, with addresses, is in [CLEANROOM-UNLOCKS.md](CLEANROOM-UNLOCKS.md).

## Where each unlock came from

| ID(s) | Modem | Vendor dispatch | Vendor library | Our mechanism |
|---|---|---|---|---|
| `17cb:0308` | Foxconn T99W696 (SDX61) | `setFccUnlock_fxn` | `libfiisdk.so.2.2.2` | `foxunlock` |
| `33f8:0301` | Rolling RW101R-GL | `fccunlock_rw101` | `libmodemauthRW101.so.1.1` | `at-gtfcclock` |
| `33f8:01a4/01a8/01a9/0302` | Rolling RW101R-GL | `fccunlock_rw101` | `libmodemauthRW101.so.1.1` | `at-gtfcclock` |
| `14c3:4d75` | Fibocom FM350-GL | `fccunlock_fm350_l860` | `libmodemauth.so` | `at-gtfcclock` |
| `8086:7560` | Fibocom L860R+ | `fccunlock_fm350_l860` | `libmodemauth.so` | `at-gtfcclock` |
| `1eac:100d` | Quectel EM160R-GL | `setFccUnlock_cs24` | `libmbimtools.so` | `mbimcli` |
| `1eac:1007` | Quectel RM520N-GL | `setFccUnlock_cs24` | `libmbimtools.so` | `mbimcli` |
| `2c7c:6008` | Quectel EM061K | `setFccUnlock_cs24` | `libmbimtools.so` | `mbimcli` |
| `2c7c:030a` | Quectel EM05-G | `setFccUnlock_cs24` | `libmbimtools.so` | `mbimcli` |
| `2c7c:0310` | Quectel EM05-CN | `setFccUnlock_cs24` | `libmbimtools.so` | `mbimcli` |

That is every id in Lenovo's own `fcc-unlock.d` list, plus the two EM05 variants.

The US-SIM gate (`GetCountry`, `get_country_code`, `location_is_USA`) lives in
the `DPR_Fcc_unlock_service` caller in every family, never in the message
builder, so omitting it changes nothing about the unlock.

## Three mechanisms

**`foxunlock`** — `Fox_Attempt()` -> `FoxApSetFccLockStatus()` ->
`QMIFOXAPSetFccLockStatus()`, which composes service byte `0xE4` and message id
`0x5571`. TLV `0x01` is a 4-char salt plus lowercase md5 hex; TLV `0x02` is one
byte, `'0'` to unlock. The md5 input is the mcfg version with its last two
dot-separated fields dropped, the apps version, the IMEI, the salt and the magic
`FDE2`. The magic is built on the stack as `ighU` and put through
`b_char_value()`, which is `c ? c - 0x23 : 0`.

**`at-gtfcclock`** — two functions, not one. `event_monitor_at()` runs
`at+gtfcclockgen`, `at+gtfcclockver=<n>` (which must reply 1), then
`at+gtfcclockmodeunlock`, `at+cfun=1` and `at+gtfcclockstate`.
`event_monitor_at_fm350()` runs `at+gtfcclockgen`, `at+gtfcclockver=<n>`,
`at+cfun=1` and `AT+GTFCCEFFSTATUS?` — no `at+gtfcclockmodeunlock`, and a
different status command. None of them is non-fatal: on either path every
failure branch logs and calls `exit(1)`, terminating `DPR_Fcc_unlock_service`.
Only a `gtfcclockver` value other than 1 retries. The response is
`compute_sha256()`: `sha256( sha256(key)[0:4] ++ challenge[0:4] )[0:4]`, sent as
a decimal. The key is 14 bytes and its digest begins `3df8c719`. Byte order is
per device — `compute_sha256()` is little endian, `compute_sha256_fm350()` big.
See [VENDOR-SEQUENCES.md](VENDOR-SEQUENCES.md).

`fccunlock_rw101` resolves only `init_modemauth_srvc` — no MBIM variant, no
device path — so one code path serves all five Rolling ids and the library finds
the AT port itself. `fccunlock_fm350_l860` is called with transport selector 1 at
every call site, for the FM350-GL and the L860R+ alike, resolving
`init_modemauth_srvc()` on `/dev/wwan0at0`.

**`mbimcli`** — `mbim_radio_state_set()` composes an MBIM SET on uuid
`11223344-5566-7788-99aa-bbccddeeff11` cid 1, then one on
`a289cc33-bcbb-8b4f-b6b0-133ec2aae6df` cid 3. The first is libmbim's
`uuid_quectel` with `MBIM_CID_QUECTEL_RADIO_STATE`, which is what
`--quectel-set-radio-state=on` sends; the second is Basic Connect radio state,
which only powers the radio up, and ModemManager does that itself.

EM05 routes here too: `setFccUnlock_em05` `dlopen()`s `/usr/lib/mbim2sar_em05.so`,
which no Lenovo package ships, so that path is inert.

## Rolling serial AT port

The RW101R-GL answers its challenge on a `ttyUSB` port rather than the wwan AT
service, so the `option` driver must have bound to the device. Linux 6.18 added
`33f8:01a8`, `01a9`, `0301` and `0302` to that driver's id table in commit
`523bf0a59e67`, also present in the stable backports; only `33f8:01a4` is older.
On an earlier kernel the driver never binds and no `/dev/ttyUSB` is created.

Two things handle that, so no system has to be configured by hand:

- The installer always writes `99-rw101r-serial.rules` to
  `/etc/udev/rules.d/`, covering all four ids the 6.18 commit added. Each line
  runs `modprobe option` before writing `new_id`, because
  `/sys/bus/usb-serial/drivers/option1/` does not exist until the module is
  loaded. The rule is a no-op on a kernel that already knows the ids, and
  `--uninstall` removes it. The installer also binds immediately so the current
  boot works without a replug.
- The dispatcher checks before attempting the unlock. If ModemManager passed no
  `ttyUSB`, it loads `option`, writes the four ids to `new_id` and waits up to
  1.2s for a port to appear — bounded well inside ModemManager's five-second
  dispatcher timeout. That covers a system where the rule was never installed,
  or a card moved between machines.

Neither `usbserial` nor `usb_wwan` needs loading separately: `option` selects
`USB_SERIAL_WWAN` and links its symbols, so modprobe resolves both.

ModemManager 1.24.2 has a hard-coded five-second FCC dispatcher timeout. The
vendor's own retry sequence can exceed that, so a longer timeout may be needed.

## SAR

`wwan-orch --sar --family <X>` applies RF/SAR the same gate-free way, transcribing
`configservice_lenovo`'s per-family SAR dispatch (`main` calls each directly) minus
the US-SIM gate. For five families the SAR logic lives in a worker lib and
`configservice_lenovo`'s `checkSARConfig_*` is a thin (~200-byte) `dlopen`+call
wrapper, so we reuse the lib:

| Family | `checkSARConfig_*` → lib | SAR entry |
|---|---|---|
| `fxn`   | `libfiisdk` | `Set_RF_Files` (chassis-matched) |
| `fm350` | `libconfigservice350.so` | `configservice_fm350` |
| `l860`  | `libconfigserviceR+.so` | `configservice_rplus` |
| `rw101` | `libconfigservice101.so.1.2` | `configservice_101` |
| `rw350` | `libconfigservice350.so.1.2` | `configservice_350` |

**Quectel `cs24` SAR (EM160/EM061K/RM520) is implemented** by reusing Lenovo's own
apply code. Its logic has no `checkSARConfig_*` wrapper — it lives inside
`configservice_lenovo` itself (`setSARConfig_common`) — so `wwan-orch`'s
`sar_quectel()` transcribes that orchestrator gate-free, calling the bundled
`libmbimtools.so`'s exported `mbim_sar_ops` slots in the same order:

```
ops[1].init(dev)            mbim_ctx_init; also mbim_get_module_type() -> s_module_type
[US-SIM gate]               <- omitted
ops[6].preprocess_inpput_files(bin,1,1,get_nv)
                            decrypt_bin_to_file() (RSA-decrypt signed .bin) + build
                            sar_file_info (path, +0x110 = get_nv(path) NV-version)
ops[5].set_sar_value(info,1,0,0,0)
                            send_nv_to_modem (md5 vs modem EFS, skip if equal, else
                            mbim_set_sar_enable/value) + AT + mbim_sar_update_commit
ops[2].uninit()
```

The `"libmbim2sar.so"` in Lenovo's log string is misleading — the real `dlopen`
path baked into `setSARConfig_common` is `/opt/fcc_lenovo/lib/libmbimtools.so`
(verified at `.rodata` 0xdff0), the lib we ship for `cs24` FCC. The `.bin` tables
ship in `sar_config_files.tar.gz`; the `cs25/` filenames embed the machine type, so a
glob picks the same chassis file. The **only** reimplemented piece is `get_nv_<model>`
(it lives in the gated binary): each `strstr`s the NV-version from the filename and
returns it (e.g. `29619`), which `sar_quectel()` reproduces by reading the trailing
`..._<nvver>.bin` integer. (An earlier build that mis-modelled the apply — passing a bare path
where a `sar_file_info*` was expected — was caught by review and rewritten to reuse
`ops[6]`/`ops[5]` as above.)

**Quectel `em05` SAR (EM05-CN + EM05-G) is also implemented** (`sar_em05()`). A full
end-to-end map of `configservice_lenovo` is in
[configservice_lenovo-map.md](configservice_lenovo-map.md); the short version:
- **EM05-CN** (`2c7c:0310`) is routed by the stock software through
  `setSARConfig_common` — the same bundled `libmbimtools.so` path as cs24 — consuming
  the shipped `EM05CN .bin` tables.
- **EM05-G** (`2c7c:030a`) DPR is a **first-class capability compiled into the bundled
  `libmbimtools.so`**, not a gap. Its `mbim_sar_ops` slot[5] (`set_sar_value`, 0x14643)
  `strcmp`s `s_module_type` and, for `"EM05G"`, calls `mbim_set_dprconfig` (0x11a31),
  which parses `DPRConfig.xml` and emits `at+qcfg="sarcfg"` (strings `quec_DPRConfig.c`,
  `em05g_sar_tool_usage` confirm this is Quectel's DPR-tool source, built in).
  `mbim_get_module_type` (`at+qgmr`) recognises `"EM05G"`. `sar_em05()` drives exactly
  this bundled path.

  Stock `configservice_lenovo` *also* has a second, redundant EM05-G route —
  `setSARConfig_em05`, which `dlopen`s `/usr/lib/mbim2sar_em05.so` (an external Quectel
  library that writes DPR to persistent NV/EFS and then reboots the modem). **Neither
  Lenovo package ships that file** — not `lenovo-wwan-unlock` (not in the repo, tarballs,
  or `fcc_unlock_setup.sh`) nor the older `lenovo-wwan-dpr` snap — so as shipped that
  route is inert (`dlopen` fails → `"Open libmbim2sar.so failed!"`). We deliberately use
  the bundled runtime route instead, which needs no external lib. See the reboot note
  below for why skipping that route's reboot is correct, not a shortcut.

Both variants converge on `set_sar_value`'s EM05 branch, so `sar_em05()` drives the
bundled `mbim_sar_ops` for both: `init` → `preprocess_inpput_files` (EM05-CN decrypts
its `.bin`; EM05-G uses the extracted `DPRConfig.xml`) → `set_sar_value(info, 1,
project, processor, 0)` → `uninit`, where `project` = DMI product family and
`processor` = Intel/AMD. `DPRConfig.xml` is extracted at install from Lenovo's own
unmodified `configservice_lenovo` (bundled in `vendor/lenovo/`). The US-SIM gate is
not in any SAR path (only in the FCC `setFccUnlock_em05`).

Two known behavioural notes vs stock, both confirmed non-defects by a full
disassembly review:
- **EM05-G modem reboot — intentionally omitted, and correct.** Only the *external*
  `setSARConfig_em05` route reboots (via `rebootModule`, whose actual reset primitive is
  an ops call `[sar_ops_em05+0x20](0,1)` *inside* the unshipped `mbim2sar_em05.so`). That
  route reboots because it writes DPR to **persistent NV/EFS**, which the modem only
  loads on restart. The route we use applies DPR at **runtime** via `at+qcfg="sarcfg"`
  (+ `at+qsar=2,1` enable + `mbim_sar_update_commit`), which per Quectel's
  `EC2x/EG2x/EG9x/EM05 QCFG AT Commands Manual` §3.20 takes effect immediately (no
  `<effect>=reboot` parameter, no restart note) and is **not** saved to NV — so it is
  simply re-applied on each boot by `wwan-sar.service`. Decisively, **none of Lenovo's
  *shipped* SAR paths reboot** — `setSARConfig_common` (EM05-CN and every cs24 modem)
  applies + commits with no `rebootModule`. So skipping the reboot matches Lenovo's own
  shipped behaviour for the same mechanism. (If EM05-G hardware ever shows a value not
  taking effect, a manual `AT+CFUN=1,1` is the safe fallback — but the mechanism does
  not call for it.)
- **Device node.** The boot/apply SAR entrypoints now auto-detect the modem control
  node (`/dev/wwan0mbim0` → `/dev/cdc-wdm0`), matching stock `usbdeviceExists`
  detection, rather than assuming `/dev/wwan0mbim0`; override with `-d`.

## Notes

- **The unlocks** are derived from the vendor SDK, not reimplemented from guesswork:
  each message is the one Lenovo's own library composes for that modem, read out of
  the disassembly and reproduced with stock tooling. A failed FCC unlock is not
  destructive — it leaves the radio disabled, recoverable by `--uninstall`.
- **SAR** is different: it still calls Lenovo's libraries through `wwan-orch`, and
  the per-family notes above mark which of those paths have been exercised.
- **EM05 (`mbim2sar_em05.so`)**: for **FCC**, `DPR_Fcc_unlock_service`'s
  `setFccUnlock_em05` (which loads `/usr/lib/mbim2sar_em05.so`) is **dead code —
  zero call sites**; `main` dispatches every Quectel modem, EM05 included, through
  `setFccUnlock_cs24` (`libmbimtools.so`), so `--family em05` correctly routes to the
  `cs24` FCC path and `mbim2sar_em05.so` is never loaded for FCC. It **is** loaded
  for EM05 **SAR** — see below.
- **`foxunlock`** is a separate, fully clean-room FCC unlock for the T99W696 that
  uses no Lenovo code at runtime. It is **not** part of the installer or any module
  — build it standalone with `make foxunlock`. See T99W696-FCC-unlock-findings.md.
- Many cards are also handled directly by upstream ModemManager; the installer
  prefers an upstream fcc-unlock script when one exists for the detected id.
