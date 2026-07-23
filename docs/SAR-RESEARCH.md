# SAR deployment across Lenovo WWAN cards — research + strategy

## How Lenovo deploys SAR, per card family

SAR (RF power / TAS / DPR tables) is deployed by `configservice_lenovo`, which
dlopens a **different library per modem family**. The mechanisms are genuinely
different — there is no single "write SAR" path to reimplement.

| Family | Card(s) | Library | Deploy mechanism | SAR data lives |
|---|---|---|---|---|
| Foxconn SDX61 | T99W696 `17cb:0308` | `libfiisdk.so` | QMI-over-MBIM EFS write (`QMIFOXSetEFSInfo`) | separate `.bin` (XOR+tar+zlib+protobuf) |
| Fibocom FM350 | `14c3:4d75` | `libconfigservice350.so` | **AT commands** (`at+bodys…`) | **embedded in the library** (`_binary_sartables350_bin`) |
| Fibocom L860 | `8086:7560` | `libconfigserviceR+.so` | `sar_table` C++ objects, `cur_sar_version` compare | in-library tables |
| Rolling RW101 | `33f8:*` | `libconfigservice101.so` | **devicepack burn** (`at+gtdevpackver?`, `at+syscmd=sys_reboot bootloader`) + SAR | OTA devicepack |
| Quectel EM061K/EM160/RM520 | `1eac:*` etc. | logic **in `configservice_lenovo`** (`setSARConfig_common`) → `libmbimtools.so` | in-binary, calls worker lib | signed `.bin` (`cs25/`) |
| Quectel EM05-CN | `2c7c:0310` (X1C G12, X13 G5, T14 G5) | logic **in `configservice_lenovo`** (`setSARConfig_common`, module.0==6) → `libmbimtools.so` | in-binary, same path as cs24 | signed `.bin` (`*EM05CN*`) |
| Quectel EM05-G | `2c7c:030a` | stock: `setSARConfig_em05` (module.0==3) → external `/usr/lib/mbim2sar_em05.so` (unshipped); we use bundled `libmbimtools.so` `mbim_set_dprconfig` | `at+qcfg="sarcfg"` runtime DPR | `DPRConfig.xml` (embedded in `configservice_lenovo`) |

Key structural facts:

- **Only Foxconn keeps SAR in separate `.bin` files that map to a writable EFS
  path.** That is why it was cleanly reimplementable, and why it was done.
- **Fibocom bakes the SAR tables *into the .so***, applied via AT commands. There
  is no external file to point a writer at — the data is compiled in.
- **Quectel `.bin` files are digitally SIGNED.** `configservice_lenovo` links
  `libcrypto` and calls `EVP_DigestVerify*` — it verifies a signature over each
  SAR blob before applying. (This is signature *verification*, not encryption,
  which is why XOR-0x95 produced garbage on `cs25` files.)
- **Rolling is not just SAR** — the 101 path flashes a firmware devicepack and
  reboots to bootloader. Much higher stakes than a table write.
- Every path through the original `configservice_lenovo` carries the **US-SIM SAR
  gate** (`WWAN is not enabled for USA`, `COUNTRY_IS_USA`), which is why we're 
  re-implementing everything without it. There is no reason hardware verified
  for Windows is invalidated by running Linux.

## Licensing — can we reuse Lenovo's tool and files?

**Yes, redistributing and running them unmodified is permitted.** The Lenovo wwan
license grants "a royalty free license to **use and distribute** the Software",
and forbids only **modifying** the binaries/libraries. So:

- Bundling the SAR `.bin` files unmodified: allowed (we already do, for Foxconn).
- Bundling and invoking `configservice_lenovo` + the `libconfigservice*` libs
  **unmodified**: allowed.
- Patching any of those binaries (e.g. to remove the US-SIM SAR gate): **not**
  allowed.

Additionally, per `ThirdPartyNotices.txt`, the **Fibocom** components are
**BSD-3-Clause** — permissive, so those specifically could even be modified/
redistributed. The Lenovo-authored wrapper is the restrictive part.

### Decision

SAR is applied gate-free through `wwan-orch --sar`, which transcribes
configservice_lenovo's per-family `checkSARConfig_*` dispatch minus the US-SIM gate
(verified: the gate strings exist only in `configservice_lenovo`, 0 in any
`libconfigservice*` lib). Each family dlopens its Lenovo lib (bundled unmodified in
`vendor/lenovo/`) and calls its self-contained SAR entry:

| Family | Lib | SAR entry |
|---|---|---|
| `fxn`   | `libfiisdk` | `Set_RF_Files` (chassis-matched) |
| `fm350` | `libconfigservice350.so` | `configservice_fm350` |
| `l860`  | `libconfigserviceR+.so` | `configservice_rplus` |
| `rw101` | `libconfigservice101.so.1.2` | `configservice_101` |
| `rw350` | `libconfigservice350.so.1.2` | `configservice_350` |

**Quectel `cs24` SAR (EM160/EM061K/RM520) and `em05` SAR (EM05-CN + EM05-G) are both
implemented.** Unlike the
five families above (whose `checkSARConfig_*` is a thin `dlopen`+call wrapper),
Quectel has no wrapper — the logic lives *inside `configservice_lenovo` itself*:
`setSARConfig_common` (~11 KB, 22 live call sites from `main`). `wwan-orch`'s
`sar_quectel()` transcribes that orchestrator gate-free.

The `libmbim2sar.so` "requirement" was a **red herring**: `setSARConfig_common` logs
`"Open libmbim2sar.so failed!"` but the string it actually passes to `dlopen` is
`/opt/fcc_lenovo/lib/libmbimtools.so` (verified at `.rodata` 0xdff0) — the lib we
already bundle for `cs24` FCC, which exports the SAR worker set as the `mbim_sar_ops`
struct. The per-model `.bin` tables ship in `sar_config_files.tar.gz` (90 RM520 · 34
EM061K · 24 EM160 · 4 EM05). `sar_quectel()` reuses Lenovo's own code end to end:

- `ops[1].init(dev)` — `mbim_ctx_init`; also calls `mbim_get_module_type()` and
  `strcpy`s the result into the lib's `s_module_type` (no manual setup needed).
- `ops[6].preprocess_inpput_files(bin, 1, 1, get_nv)` — `decrypt_bin_to_file()`
  (RSA-decrypt the signed `.bin`) and build `sar_file_info` (path at +0x10,
  **+0x110 = `get_nv(path)`** the NV-version, +0x114 = decrypted flag).
- `ops[5].set_sar_value(info, 1, 0, 0, 0)` — for non-em05g modules,
  `send_nv_to_modem(info, count, force=0)` (md5-compare vs modem EFS, skip if equal,
  else `mbim_set_sar_enable`/`set_sar_value`) + AT commands + `mbim_sar_update_commit`
  + `unlink()` of the temp files.
- `ops[2].uninit()`.

The **only** reimplemented piece is `get_nv_<model>` (it lives in the gated binary):
each `strstr`s the NV-version from the filename and returns it (`get_nv_em160`:
`"29619"`→29619, `"30007"`→30007; `get_nv_061`: `"48001"`; …). `quectel_get_nv()`
reproduces this by reading the trailing `..._<nvver>.bin` integer — the exact value
that lands in `sar_file_info+0x110`. `unverified` (no Quectel hardware). (An earlier
build mis-modelled the apply — passing a bare path where a `sar_file_info*` was
expected, which an automated review caught as an out-of-bounds read — and was
rewritten to reuse `ops[6]`/`ops[5]` as above.)

**EM05 SAR is also implemented** (`sar_em05()`), after a full end-to-end map of
`configservice_lenovo` ([configservice_lenovo-map.md](configservice_lenovo-map.md))
showed the `mbim2sar_em05.so` "requirement" is, once again, avoidable. EM05-CN is
routed through `setSARConfig_common` (bundled `libmbimtools.so`, `EM05CN .bin` tables);
EM05-G is routed through `setSARConfig_em05` (absent `/usr/lib/mbim2sar_em05.so`) but
that lib's function is **fully duplicated in the bundled `libmbimtools.so`** —
`mbim_get_module_type` recognises `"EM05G"`, `set_sar_value` has an EM05 branch, and
`mbim_set_dprconfig` parses the embedded `DPRConfig.xml` and issues the same
`at+qcfg="sarcfg"` commands as the external `sar_ops[+0x70]`. Both converge on
`set_sar_value`'s EM05 branch. `DPRConfig.xml` is extracted at install from Lenovo's
own unmodified `configservice_lenovo`. Only `fxn` SAR has been run on hardware by the
maintainer; the rest (incl. EM05) are `unverified`.
