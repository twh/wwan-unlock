# configservice_lenovo — Complete Reverse-Engineering Map

This is an end-to-end analysis of the original Lenovo unlock orchestration binary.
Source of truth: `~/Downloads/lenovo-wwan-unlock/` (Lenovo WWAN unlock package).
Binary: `configservice_lenovo` — ELF64 PIE, x86-64, **not stripped**, with `.debug_info`.
BuildID `ec10eb544ff86c44cc7ab716578f039aafa28d4f`.
All addresses are file/virtual addresses from this exact binary. The old
`lenovo-wwan-dpr` 1.3.0 snap is used only as an evolution reference; every claim
below is grounded in the Lenovo binaries.

---

## 0. EM05 info

* **EM05-CN (USB `2c7c:0310`, `module.0 == 6`)** → routed to `setSARConfig_common`
  (0x8aa0), which `dlopen`s the **bundled** `/opt/fcc_lenovo/lib/libmbimtools.so`
  and drives its `mbim_sar_ops`. **Fully reimplementable with bundled artifacts**
  — this is the same path cs24 uses (already implemented in `wwan-orch.c`). It
  consumes the `EM05CN` `.bin` NV tables from `sar_config_files.tar.gz`.
* **EM05-G (USB `2c7c:030a`, `module.0 == 3`)** → routed to `setSARConfig_em05`
  (0x8324), which `dlopen`s `/usr/lib/mbim2sar_em05.so` (dlsym `sar_ops`). **That
  lib is NOT in the package**, and **no bundled lib exports a bare `sar_ops`**.
  BUT the bundled `libmbimtools.so` contains the complete EM05-G apply logic:
  `mbim_get_module_type` (via `at+qgmr`) recognises `"EM05G"`, `set_sar_value`
  has an explicit `EM05G` branch, and `mbim_set_dprconfig` parses the
  `DPRConfig_xml` blob and issues `at+qcfg="sarcfg"` commands. **So EM05-G SAR can
  be reimplemented with only bundled artifacts** by driving `mbim_sar_ops`
  (or the named exports directly) exactly as for EM05-CN. `mbim2sar_em05.so` is
  **not irreducibly required**.
* The **US-SIM country gate is NOT in the SAR path at all** — it lives only in
  `setFccUnlock_em05` (FCC unlock). SAR needs no gate removed.

Full detail follows.

---

## 1. Function inventory (`nm` T/t symbols)

### Entry / dispatch / detection
| addr | symbol | role |
|------|--------|------|
| 0x63cc | `main` | getopt(`-v` verbose), `get_product` → sets `MACHINE` (0x10010), device/MM readiness waits, jump-table dispatch on `MACHINE`, inner `module.0` (0x10018) switch → selects SAR handler. |
| 0x26e9 | `get_product_name` | Sets `MACHINE`/`PRODUCT_CPU` globals from product family + CPU vendor. |
| 0x3e54 | `get_product` | `open`/`read` `/sys/class/dmi/id/product_family`, `strncmp` against a long product table (ThinkPad X1 Yoga Gen6, ThinkEdge SE30/SE10, …) → integer product id. |
| 0x4acd | `isWWANModuleInstalled` | `popen("/usr/bin/lspci -nn")`; strstr PCI IDs (M.2 modems). Sets `module.0`: EM120R/`1eac:1002/1001`, FM350 `8086:7560` / "T700 5G Modem", `1eac:1007/100d`, `14c3:4d75`, `17cb:0308`. |
| 0x4f9d | `isusbWWANModuleInstalled` | `popen("lspci"→ actually lsusb-style)`; strstr USB VID:PID → `module.0` (see §3 table). **EM05-G `2c7c:030a`→3, EM05-CN `2c7c:0310`→6**, `2c7c:6008`→8, `33f8:01a4/01a8/01a9/0301/0302`→11. |
| 0x52b9 | `deviceExists` | Waits for MBIM char device to appear. |
| 0x54bc | `isModemManagerReady` | Polls ModemManager readiness. |
| 0x77f0 | `usbdeviceExists` | `popen("ls /dev/cdc*")`; strstr `/dev/cdc-wdm0` → sets `devPath_em05`. |
| 0x77d9 | `enable_verbose_log` | Sets verbose flag. |
| 0x7af1 | `disable_auto_suspend` | Writes USB autosuspend control (called from `rebootModule`). |

### Per-family SAR handlers
| addr | symbol | dlopen target (bundled?) | dlsym | notes |
|------|--------|--------------------------|-------|-------|
| 0x560c | `checkSARConfig_l860` | `/opt/fcc_lenovo/lib/libconfigserviceR+.so` ✅ | `configservice_rplus` | Intel L860 (`module.0==5`). |
| 0x56e0 | `checkSARConfig_fm350` | `/opt/fcc_lenovo/lib/libconfigservice350.so` ✅ | `configservice_fm350` | FM350 (`module.0==4`). |
| 0x57b4 | `checkSARConfig_rw350` | `/opt/fcc_lenovo/lib/libconfigservice350.so.1.2` ✅ | `configservice_350` | RW350 (`module.0==4`). |
| 0x5888 | `checkSARConfig_rw101` | `/opt/fcc_lenovo/lib/libconfigservice101.so.1.2` ✅ | `configservice_101` | RW101 (`module.0==11`). |
| 0x595c | `checkSARConfig_SDX61` | `/opt/fcc_lenovo/lib/libfiisdk.so.2.2.2` ✅ | `ModuleConnect` | SDX61 (`module.0==10`). |
| 0x8aa0 | `setSARConfig_common` | `/opt/fcc_lenovo/lib/libmbimtools.so` ✅ | `mbim_sar_ops` | **EM05-CN, EM060, EM160, RM520N, EM061 …** (`module.0` in {6,7,8,9,10,…}). Uses `mbim_sar_ops` slots + `input_files_*` + `get_nv_*`. |
| 0x8324 | `setSARConfig_em05` | `/usr/lib/mbim2sar_em05.so` ❌ **absent** | `sar_ops` | **EM05-G only** (`module.0==3`, MACHINE 19/20). Writes `DPRConfig_xml`→file, RSA-verify, external `sar_ops`. |

### `get_nv_*` family (map model string → NV item id; passed to `preprocess_inpput_files` as callback)
| addr | symbol | behaviour |
|------|--------|-----------|
| 0x89c0 | `get_nv_em05cn` | **stub: always returns 0xffffffff** (EM05CN uses no NV item — `.bin`/dpr path only). |
| 0x89d3 | `get_nv_em160` | strstr `"WCDMA"` → returns 0x73b3. |
| 0x8a0c | `get_nv_r520n` | strstr `"WCDMA"`→0x73b3, `"LTE"`→0x7537. |
| 0x8a67 | `get_nv_061` | strstr `"WCDMA"`→0xbb81. |

### Helpers / crypto
| addr | symbol | role |
|------|--------|------|
| 0x80d5 | `rebootModule` | Reboots modem via **external** `sar_ops` slots +0x18 (ready), +0x20, +0x68; calls `disable_auto_suspend`, `stat(devPath_em05)`. EM05-G path only. |
| 0x7d89 | `setFccUnlock_em05` | **FCC unlock** for EM05 — **contains the US-SIM country gate** (strings at 0xdc50 "WWAN is not enabled for USA", 0xdc8e "SIM inserted is of non-USA"). Not part of SAR. |
| 0xb7d1 | `file_rsa_verify` | `fopen`/`fread` a file + its `.sig`, RSA-verify (`rsa_verify_sig`). Used by `setSARConfig_em05` on `.DPRConfig.xml`/`DPRConfig.xml.sig`. |
| 0xb62f | `rsa_verify_sig` | Raw RSA signature verify (OpenSSL). |
| 0xbbb0/0xbc40/… | `strnlen_s`,`strcpy_s`,`set_str_constraint_handler_s`,`invoke_safe_str_constraint_handler`,`ignore_handler_s` | Safe-C string helpers. |

---

## 2. Control flow from `main` (0x63cc)

1. `getopt` — only `-v` (bumps `options`/verbose, global 0x261f4).
2. `get_product()` (0x6426) → on error, exit. Sets `MACHINE` (0x10010) and, via
   `get_product_name` (0x65b6), `PRODUCT_CPU` (0x10014).
3. `isWWANModuleInstalled()` then `isusbWWANModuleInstalled()` → `module.0`
   (0x10018). Negative on both → exit "no module".
4. Guard: only continue if `module.0 ∈ {3,6,8,11}` (0x64a9) *or* fall through to
   device/MM readiness waits (`deviceExists`, `isModemManagerReady`).
5. **Two-level dispatch** at 0x65bb:
   * `idx = MACHINE − 8`; if `idx > 0x3c` → default "sar config not needed".
   * Indexed jump table at **0xd970** (61 entries) selects a per-machine block.
   * Inside each block, an inner `switch(module.0)` selects the SAR handler.

### Dispatch table (machine → module.0 → handler)
```
MACHINE 19,20         module==3  -> setSARConfig_em05      (EM05-G; L13 Gen4 / L13 Yoga Gen4)
MACHINE 8,9,15,16     module==5  -> checkSARConfig_l860
                      module==4  -> checkSARConfig_fm350
MACHINE 21..26,46     module==5  -> checkSARConfig_l860
MACHINE 35            module==6  -> setSARConfig_common     (EM05-CN)
                      module==9  -> setSARConfig_common
MACHINE 36            module==9  -> setSARConfig_common
MACHINE 37,38         module==8  -> setSARConfig_common
MACHINE 39            module==6  -> setSARConfig_common     (EM05-CN)
                      module==8  -> setSARConfig_common
                      module==4  -> checkSARConfig_fm350
MACHINE 40            module==6/8/9 -> setSARConfig_common  (EM05-CN incl.)
MACHINE 41            module==6/7/8/9 -> setSARConfig_common(EM05-CN incl.)
MACHINE 42,43         module==7/8 -> setSARConfig_common
MACHINE 45,47,48,49,
        50,53,54,55   module==7/8/9 -> setSARConfig_common
MACHINE 51,52         module==8  -> setSARConfig_common
MACHINE 56,57         module==4  -> checkSARConfig_rw350
                      module==8  -> setSARConfig_common
MACHINE 58,59         module==4  -> checkSARConfig_rw350
MACHINE 68            module==10 -> checkSARConfig_SDX61
MACHINE 60,61,62,64,66 module==10 -> checkSARConfig_SDX61
                      module==11 -> checkSARConfig_rw101
```
(The `0x1/0x2/0x3` compares inside the MACHINE 47/48/50 blocks are against
`PRODUCT_CPU` (0x10014), **not** `module.0` — verified at 0x6dfd.)

### `setSARConfig_em05` is reachable live
`module.0==3` (EM05-G) on MACHINE 19/20 (ThinkPad L13 Gen 4 / L13 Yoga Gen 4)
calls `setSARConfig_em05` at the single call site 0x6603 (`edi = MACHINE`).
This is a **real live path**, unlike FCC where EM05 is funneled through
`setFccUnlock_cs24`. EM05-CN never reaches `setSARConfig_em05` (module 6 ≠ 3).

### US-SIM country gate
`location_is_USA` is exported by `libmbimtools.so` (0x12cd2) but **has no internal
callers** there. The visible gate ("WWAN is not enabled for USA") is referenced
only from `setFccUnlock_em05` (0x7fa1/0x7fc6). **Neither `setSARConfig_em05` nor
`setSARConfig_common` nor `libmbimtools` `set_sar_value`/`mbim_set_dprconfig`
invoke any USA/country check.** SAR is not country-gated.

---

## 3. `module.0` enum (from `isusbWWANModuleInstalled`, USB) 

| module.0 | USB VID:PID / marker | modem |
|----------|----------------------|-------|
| 3 | `2c7c:030a` / "EM05-G" | **Quectel EM05-G** |
| 6 | `2c7c:0310` / "EM05-CN" | **Quectel EM05-CN** |
| 8 | `2c7c:6008` | Quectel (EM05 variant / other) |
| 11 | `33f8:01a4/01a8/01a9/0301/0302` | Rolling RW modules |
| 4,5,7,9,10 | (PCI, from `isWWANModuleInstalled`) | FM350/L860/EM120/EM160/SDX61 etc. |

---

## 4. `setSARConfig_em05` (0x8324) — complete flow

Signature: `int setSARConfig_em05(int MACHINE)`.

1. `MACHINE` switch (0x8348): 0x12→"ThinkPad Z13 Gen 1", **0x13→"ThinkPad L13 Gen 4"**,
   **0x14→"ThinkPad L13 Yoga Gen 4"**; anything else → syslog "sar config not needed",
   return 0. (`[rbp-0x140]` = machine/project-name string, used later as the
   `sar_ops[+0x70]` project arg.) Only 0x13/0x14 arrive live.
2. `usbdeviceExists()` retry loop (up to 30×, `sleep(2)`), requires `/dev/cdc-wdm0`.
3. `popen("cat /proc/cpuinfo | grep vendor_id", "r")`; `fgets`; `strstr` "Intel"
   (0xde23) / "AMD" (0xde29) → copies the matched literal into `[rbp-0x11a]`
   (processor-manufacturer string). Neither → syslog "processor information not
   found", `pclose`.
4. `dlopen("/usr/lib/mbim2sar_em05.so", RTLD_LAZY)` → `dlHandle_em05` (0x26210).
   Fail → syslog "Failed to load quectel library", return 1.
5. `dlsym(handle, "sar_ops")` → `sar_ops_em05` (0x26200). Fail → dlclose, return 1.
6. `fopen("/opt/fcc_lenovo/.DPRConfig.xml", "wb")`.
7. `fwrite(DPRConfig_xml, 1, DPRConfig_xml_len=50954, f)` — writes the embedded
   blob (0x10020, len @0x1c72c) to disk. `fclose`.
8. `file_rsa_verify("/opt/fcc_lenovo/.DPRConfig.xml",
   "/opt/fcc_lenovo/DPRConfig.xml.sig")` (0xdef8). Nonzero → return err.
9. **sar_ops slot calls** (external struct; argument regs shown):
   * `sar_ops[+0x8]( rdi = &devPath_em05 )` — **init/open MBIM device**.
     Nonzero → "ERROR: SAR init fails".
   * `sar_ops[+0x18]( edi=0, rsi=&status[rbp-0x150] )` — **MBIM-ready check**;
     retried up to 10× with `sleep(10)` on failure ("Retrying …"). Still bad →
     "MBIM device is not ready", return 1.
   * If `status ([rbp-0x150]) != 0`:
     `sar_ops[+0x70]( edi=0, rsi="/opt/fcc_lenovo/.DPRConfig.xml",
     rdx=[rbp-0x140] (project name), rcx=&[rbp-0x11a] (Intel/AMD) )`
     — **the actual SAR/DPR apply**. Ret 0 → "Error while setting SAR
     configuration"; <0 → "Set SAR config is failed", `remove` file, return 1.
   * On success (>0): `rebootModule( rdi = sar_ops_em05 )` (0x80d5) →
     uses slots +0x18/+0x20/+0x68 to reboot the modem.
10. `remove("/opt/fcc_lenovo/.DPRConfig.xml")`; `sar_ops[+0x10]()` — **deinit**;
    `dlclose(dlHandle_em05)`.

**Functional identity of the external `sar_ops` slots** (confirmed by the
bundled-lib equivalents below): +0x8 = init/open (`mbim_ctx_init`), +0x18 =
ready-check (`mbim_is_ready`), +0x70 = DPR-config apply (`mbim_set_dprconfig`),
+0x10 = deinit (`mbim_ctx_deinit`). Note the external struct has slots out to
**+0x70/+0x68** — larger than `mbim_sar_ops`.

---

## 5. `setSARConfig_common` (0x8aa0) — the bundled path (EM05-CN et al.)

Signature: `int setSARConfig_common(int module, int MACHINE, char *prodName)`.

1. `dlopen("/opt/fcc_lenovo/lib/libmbimtools.so", RTLD_NOW)` → `dlHandle_em05`.
2. `dlsym(handle, "mbim_sar_ops")` → 7-pointer ops struct (see §6).
3. `popen("cat /proc/cpuinfo | grep vendor_id")` → Intel/AMD detection (same
   literals 0xde23/0xde29) as the em05 path.
4. Big `switch(MACHINE, module)` builds the correct **`input_files_*` descriptor**
   and **`get_nv_*` callback**, then calls **`mbim_sar_ops[6]` = `preprocess_inpput_files`
   (slot +0x30)** with `rax = &input_files_*`, `rdx = get_nv_*`. Examples
   (0x8fcd–0x90c2):
   * `input_files_em05cn_X1CG12` (0x1c740) + `get_nv_em05cn`
   * `input_files_em05cn_X13G5` (0x1c840) + `get_nv_em05cn`
   * `input_files_em05cn_X13G52in1` (0x1c940) + `get_nv_em05cn`
   * `input_files_em05cn_T14G5` (0x1ca40) + `get_nv_em05cn`
   (plus EM160/RM520N/EM061 descriptors for other modules).
5. Later calls **`mbim_sar_ops[5]` = `set_sar_value` (slot +0x28)** (0xb579) and
   reads slot +0x10 / uses init/uninit as needed.

The `input_files_em05cn_*` descriptors begin with an **inline path string**, e.g.
`input_files_em05cn_X1CG12` = `/opt/fcc_lenovo/sar_config_files/1016__EM05CN__ThinkPad-X1-Carbon-Gen-12__Intel.bin`
— exactly the files shipped in `sar_config_files.tar.gz`:
```
1016__EM05CN__ThinkPad-X1-Carbon-Gen-12__Intel.bin
1212__EM05CN__ThinkPad-X13-Gen-5__Intel.bin
1212__EM05CN__ThinkPad-X13-2-in-1-Gen-5__Intel.bin
1226__EM05CN__ThinkPad-T14-Gen-5__Intel.bin
```

---

## 6. `libmbimtools.so` `mbim_sar_ops` (0x1cac0, 0x38 bytes = 7 slots)

| idx | offset | target | symbol | role |
|-----|--------|--------|--------|------|
| 0 | +0x00 | 0x18ed7 | (static) | (aux) |
| 1 | +0x08 | 0x1399b | `init` | `mbim_ctx_init` + `mbim_is_ready` + **`mbim_get_module_type` → strcpy into `s_module_type`** |
| 2 | +0x10 | 0x13b29 | `uninit` | `mbim_ctx_deinit` |
| 3 | +0x18 | 0x13bb7 | `set_sar_level` | |
| 4 | +0x20 | 0x13c78 | `get_sar_level` | |
| 5 | +0x28 | 0x14643 | `set_sar_value` | **dispatch: EM05 → `mbim_set_dprconfig`; else → `send_nv_to_modem`** |
| 6 | +0x30 | 0x142f2 | `preprocess_inpput_files` | reads the `.bin` NV tables named in `input_files_*` |

**There is NO slot at +0x70** → `mbim_sar_ops` cannot back the external
`setSARConfig_em05` struct directly (which uses +0x70/+0x68). Confirms task point 3b.
**No bundled `.so` exports a bare `sar_ops`** (checked `nm -D` on every lib);
only `libmbimtools.so` exports `mbim_sar_ops`. Confirms task point 3a.

### `init` (0x1399b) — module autodetect
`getenv("VERBOSE")` → `mbim_ctx_init(mbim_dev)` → `mbim_is_ready` →
`mbim_get_module_type()` → `strcpy(s_module_type, <ret>)`.

### `mbim_get_module_type` (0xf48b)
Sends **`at+qgmr`** to the modem, `strstr` the firmware string:
`EM160RGL→"EM160"`, `EM060KGL→"EM060KGL"`, `EM061KGL→"EM061KGL"`,
`RM520N→"RM520"`, **`EM05CN→"EM05CN"`**, **`EM05G→"EM05G"`**. Unknown → "not
supported". So `s_module_type` is set to `"EM05G"` or `"EM05CN"` straight from the
modem — no caller has to pass it.

### `set_sar_value` (0x14643)
`set_sar_value(a1=struct*, a2=int, a3=ptr, a4=ptr, a5=int)`:
* `strcmp(s_module_type,"EM05CN")==0` **or** `strcmp(s_module_type,"EM05G")==0`
  → EM05 branch:
  `mbim_set_dprconfig( rdi = a1+0x10 (dpr-xml path), rsi = s_module_type,
  rdx = a3, rcx = a4 )`, then (post) `strncmp(a1,"EM05",4)` → AT commands +
  `mbim_sar_update_commit`.
  (`em05g_sar_tool_usage` at 0x13908 is only the missing-arg **usage/error** stub.)
* else → `send_nv_to_modem( rdi=a1, esi=a2, edx=a5 )` (the `.bin`/NV path).

### `mbim_set_dprconfig` (0x11a31)
`fopen(dpr_xml, "rb")`; XML-parses: match `<project name="…">` (the machine),
match `<processor manufacturer="Intel"|"AMD">`, iterate `<dpr mode band maxpower
rowgrads columngrads>` and for each send **`at+qcfg="sarcfg","<mode>"`** /
**`at+qcfg="sarcfg","<mode>",<band>`** (`ate0` first, special-cases `CDMA`).
This is the exact functional twin of the external `sar_ops[+0x70]`.

---

## 7. Data symbols consumed

| symbol | addr | size | what |
|--------|------|------|------|
| `DPRConfig_xml` | 0x10020 | `DPRConfig_xml_len`=**50954** (@0x1c72c) | EM05-**G** DPR/SAR power-table XML. Projects: L13 Gen4, L13 Yoga Gen4, T14 Gen4, T16 Gen2, X13 Yoga Gen4, P14s Gen4, P16s Gen2, X13 Gen4, T14s Gen4, Z13 Gen2. Processors Intel+AMD. Modes LTE, WCDMA. `<dpr … maxpower rowgrads columngrads>` per band. |
| `input_files_em05cn_X1CG12` | 0x1c740 | struct | inline `.bin` path → X1 Carbon Gen12 EM05CN table |
| `input_files_em05cn_X13G5` | 0x1c840 | struct | X13 Gen5 |
| `input_files_em05cn_X13G52in1` | 0x1c940 | struct | X13 2-in-1 Gen5 |
| `input_files_em05cn_T14G5` | 0x1ca40 | struct | T14 Gen5 |
| `input_file_number*`, `input_files_em160/r520n/061*` | 0x1c730… | | other-modem tables |
| globals | 0x10010 `MACHINE`, 0x10014 `PRODUCT_CPU`, 0x10018 `module.0`, 0x261f4 `options` | | |

DPRConfig_xml first bytes:
```
<?xml version="1.0" encoding="utf-8"?>
<COLLECTION xmlns:dt="urn:schemas-microsoft-com:datatypes">
  ...
  <project name="ThinkPad L13 Gen 4">
    <processor manufacturer="Intel">
      <module method="single band">
        <dpr mode="WCDMA" band="35" maxpower="240" rowgrads="65" columngrads="0"/><!--WCDMA B1-->
        ...
```

---

## 8. External dependency map

### dlopen paths + dlsym symbols
| caller | dlopen path | in package? | dlsym |
|--------|-------------|-------------|-------|
| `setSARConfig_common` | `/opt/fcc_lenovo/lib/libmbimtools.so` | ✅ (=`libmbimtools.so`) | `mbim_sar_ops` ✅ |
| `setSARConfig_em05` | `/usr/lib/mbim2sar_em05.so` | ❌ **ABSENT** | `sar_ops` ❌ (no bundled lib exports it) |
| `checkSARConfig_l860` | `/opt/fcc_lenovo/lib/libconfigserviceR+.so` | ✅ | `configservice_rplus` |
| `checkSARConfig_fm350` | `/opt/fcc_lenovo/lib/libconfigservice350.so` | ✅ | `configservice_fm350` |
| `checkSARConfig_rw350` | `/opt/fcc_lenovo/lib/libconfigservice350.so.1.2` | ✅ | `configservice_350` |
| `checkSARConfig_rw101` | `/opt/fcc_lenovo/lib/libconfigservice101.so.1.2` | ✅ | `configservice_101` |
| `checkSARConfig_SDX61` | `/opt/fcc_lenovo/lib/libfiisdk.so.2.2.2` | ✅ | `ModuleConnect` |

### External files / commands
* `/sys/class/dmi/id/product_family` (get_product)
* `popen("/usr/bin/lspci -nn")`, `popen("lspci"…)` USB, `popen("ls /dev/cdc*")`,
  `popen("cat /proc/cpuinfo | grep vendor_id")`
* `/dev/cdc-wdm0` (MBIM device, `devPath_em05`)
* `/opt/fcc_lenovo/.DPRConfig.xml` (+ `.sig`) — written/verified by em05 path
* `/opt/fcc_lenovo/sar_config_files/*.bin` — EM05CN NV tables
* `at+qgmr`, `at+qcfg="sarcfg",…`, `ate0` — modem AT commands

### libmbimtools.so relevant named exports
`mbim_ctx_init`, `mbim_ctx_deinit`, `mbim_is_ready`, `mbim_get_module_type`,
`mbim_set_sar_value`, `mbim_get_sar_value`, `mbim_set_sar_level`,
`mbim_set_dprconfig`, `send_nv_to_modem`, `mbim_sar_update_commit`,
`mbim_send_at_command`, `mbim_get_efs_md5`, `mbim_sar_ops`, `decrypt_bin_to_file`,
`location_is_USA` (exported, uncalled internally).

---

## 9. Per-family apply recipes (gate-free)

### EM05-CN (`module.0==6`) — via bundled `libmbimtools.so` `mbim_sar_ops`
1. `dlopen` bundled `libmbimtools.so`, get `mbim_sar_ops`.
2. `mbim_sar_ops.init` → opens `/dev/cdc-wdm0`, `mbim_get_module_type`(at+qgmr)
   sets `s_module_type="EM05CN"`.
3. `mbim_sar_ops.preprocess_inpput_files(&input_files_em05cn_<machine>,
   get_nv_em05cn)` — parse the `.bin` table for the detected machine.
4. `mbim_sar_ops.set_sar_value(...)` → EM05CN branch → `mbim_set_dprconfig` /
   `send_nv_to_modem` applies to modem.
5. `mbim_sar_ops.uninit` (`mbim_ctx_deinit`).
Data: the four `EM05CN` `.bin` files in `sar_config_files.tar.gz`, keyed by
machine (X1C-G12/X13-G5/X13-2in1-G5/T14-G5) + Intel.

### EM05-G (`module.0==3`) — reconstructable via bundled `libmbimtools.so`
Instead of the missing `/usr/lib/mbim2sar_em05.so`, drive the **same
`mbim_sar_ops`** (or the named exports directly):
1. Write `DPRConfig_xml` (extract 50954 bytes @0x10020 from `configservice_lenovo`)
   to a file, e.g. `/opt/fcc_lenovo/.DPRConfig.xml`.
2. `mbim_ctx_init("/dev/cdc-wdm0")` → `mbim_is_ready` (retry).
   (`mbim_get_module_type` returns `"EM05G"`.)
3. `mbim_set_dprconfig( dpr_xml_path, "EM05G", project_name, processor )`
   where `project_name` = the machine ("ThinkPad L13 Gen 4" / "…Yoga Gen 4"),
   `processor` = "Intel"|"AMD" from `/proc/cpuinfo`. This parses the XML and
   emits `at+qcfg="sarcfg",…` per band — identical to `sar_ops[+0x70]`.
4. (Optional) reboot the modem via `mbim_send_at_command` (`AT+CFUN=1,1`).
5. `mbim_ctx_deinit`.
The RSA `.sig` verify in the stock path is an integrity gate on the on-disk XML,
not part of the RF write; it can be dropped when the XML is the trusted embedded
blob.

### Other families (reference)
`checkSARConfig_{l860,fm350,rw350,rw101,SDX61}` each `dlopen` a **bundled**
`/opt/fcc_lenovo/lib/*.so` and call one exported entry (`configservice_*` /
`ModuleConnect`). All present in the package.

---

## 10. EM05 

**EM05 SAR (both CN and G) CAN be reimplemented gate-free using ONLY the bundled
Lenovo artifacts** — `libmbimtools.so` + `sar_config_files.tar.gz` (EM05CN `.bin`)
+ `DPRConfig_xml` extracted from `configservice_lenovo`.

* **EM05-CN**: the stock software already does this — `setSARConfig_common`
  `dlopen`s the bundled `libmbimtools.so` and uses `mbim_sar_ops` + the shipped
  `EM05CN .bin` tables. Nothing external is involved.
* **EM05-G**: the stock software routes it through the **absent**
  `/usr/lib/mbim2sar_em05.so`, but that lib's function is **fully duplicated** in
  the bundled `libmbimtools.so`: `mbim_get_module_type` recognises `"EM05G"`,
  `set_sar_value` has an `EM05G` branch, and `mbim_set_dprconfig` parses
  `DPRConfig_xml` (which contains the L13 Gen4/Yoga Gen4 EM05-G tables) and sends
  the `at+qcfg="sarcfg"` commands. The external `sar_ops` slots map 1:1 onto
  `mbim_ctx_init` / `mbim_is_ready` / `mbim_set_dprconfig` / `mbim_ctx_deinit`.

**Nothing is irreducibly missing for EM05 SAR.** `mbim2sar_em05.so` is a
convenience wrapper; its RF-writing behaviour is reproduced by bundled
`libmbimtools.so` exports + the embedded `DPRConfig_xml`. The only stock-path
extra (RSA `.sig` verification of the on-disk XML) is an integrity check, not an
RF operation, and the US-SIM country gate does not exist in the SAR path at all
(it is confined to `setFccUnlock_em05`).
