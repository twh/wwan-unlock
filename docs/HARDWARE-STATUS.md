# Hardware status

Every FCC-unlock family in Lenovo's `DPR_Fcc_unlock_service`, and how `wwan-orch`
handles it. `wwan-orch` is a clean-room transcription of Lenovo's per-family
dispatch that `dlopen()`s Lenovo's own bundled libraries and calls their unlock
functions in the same order — omitting only the US-SIM check
(`GetCountry` / `get_country_code` / `location_is_USA`). Every claim below was
verified against the (non-stripped) vendor binaries.

| `--family` | Modem(s) | Lenovo dispatch | Lenovo lib | Gate omitted |
|---|---|---|---|---|
| `fxn`   | Foxconn T99W696 (SDX61/62) | `setFccUnlock_fxn` | `libfiisdk.so.2.2.2` | `GetCountry` |
| `cs24`  | Quectel RM520N/EM160/EM061/**EM05** | `setFccUnlock_cs24` | `libmbimtools.so` | `location_is_USA` |
| `rw101` | Rolling RW101R-GL | `fccunlock_rw101` | `libmodemauthRW101.so.1.1` | `get_country_code` |
| `rw350` | Rolling RW350 | `fccunlock_rw350` | `libmodemauth.so.1.1` | `get_country_code` |
| `fm350` | Fibocom FM350 | `fccunlock_fm350_l860` | `libmodemauth.so` | `get_country_code` |
| `l860`  | Fibocom L860R+ | `fccunlock_fm350_l860` | `libmodemauth.so` | `get_country_code` |

Bundled modules and their family:

| ID(s) | Module | Family | Status |
|---|---|---|---|
| `17cb:0308` | Foxconn T99W696 | `fxn` | **verified on hardware** (X1 Carbon Gen 14) |
| `1eac:100d` | Quectel EM160R-GL | `cs24` | transcribed, unverified |
| `33f8:01a4/01a8/01a9/0301/0302` | Rolling RW101R-GL | `rw101` | transcribed, unverified |
| `8086:7560` | Fibocom L860R+ | `l860` | transcribed, unverified |

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
`..._<nvver>.bin` integer. `unverified` (no Quectel hardware); only `fxn` SAR is
hardware-tested. (An earlier build that mis-modelled the apply — passing a bare path
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
not in any SAR path (only in the FCC `setFccUnlock_em05`). `unverified` (no EM05 hw).

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

- **"Transcribed"** = calls Lenovo's *tested* library exactly as their orchestrator
  does, so correctness follows from the disassembly, not a reimplemented algorithm.
  Only `fxn` has been run on real hardware by the maintainer; the installer warns
  before installing an unverified module. A failed FCC unlock is not destructive —
  it leaves the radio disabled, recoverable by `--uninstall`.
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
