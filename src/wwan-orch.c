/* SPDX-License-Identifier: MIT
 *
 * wwan-orch — clean-room GATELESS orchestrator for Lenovo WWAN FCC unlock.
 *
 * The only thing this reimplements is Lenovo's orchestrator binary
 * (DPR_Fcc_unlock_service), with ONE change: it does not call the US-SIM country
 * check (GetCountry / get_country_code). Everything that actually unlocks the
 * modem stays in Lenovo's own, unmodified, freely-distributable libraries, which
 * this loads with dlopen() and calls exactly as Lenovo's binary does.
 *
 * Per-family dispatch, transcribed from DPR_Fcc_unlock_service (not stripped),
 * each minus its country/USA gate:
 *
 *   family  Lenovo lib               call sequence (gate omitted)
 *   ------  -----------------------  ---------------------------------------------
 *   fxn     libfiisdk.so.2.2.2       ModuleConnect(dev); [GetCountry]; Fox_Attempt(); ModuleDisconnect()
 *   rw101   libmodemauthRW101.so.1.1 [get_country_code]; init_modemauth_srvc(mbim)
 *   rw350   libmodemauth.so.1.1      [get_country_code]; init_modemauth_srvc(mbim)
 *   fm350   libmodemauth.so          [get_country_code]; init_modemauth_srvc(at0)
 *   l860    libmodemauth.so          [get_country_code]; init_modemauth_srvc_mbim(mbim)
 *   cs24    libmbimtools.so          mbim_fcc_ops: init(dev); [location_is_USA]; fcc_unlock(); uninit()
 *   em05    -> routes to cs24 (its own setFccUnlock_em05 is dead code; see below)
 *
 * SAR (--sar) mirrors configservice_lenovo's per-family dispatch, gate omitted:
 *   fxn     libfiisdk Set_RF_Files (chassis .bin)      [checkSARConfig_SDX61]
 *   fm350   libconfigservice350.so    configservice_fm350   [checkSARConfig_fm350]
 *   l860    libconfigserviceR+.so     configservice_rplus   [checkSARConfig_l860]
 *   rw101   libconfigservice101.so.1.2 configservice_101     [checkSARConfig_rw101]
 *   rw350   libconfigservice350.so.1.2 configservice_350     [checkSARConfig_rw350]
 *   cs24    libmbimtools.so mbim_sar_ops (init/preprocess/set_sar_value->send_nv)  [setSARConfig_common]
 *   em05    libmbimtools.so mbim_sar_ops (init/preprocess/set_sar_value->dprconfig) [setSARConfig_common/_em05]
 *           (EM05-CN + EM05-G; the "mbim2sar_em05.so" the stock em05 path dlopens is
 *            functionally duplicated in the bundled libmbimtools.so — see sar_em05())
 *
 * Only fxn is hardware-verified by twh at waynehendricks dot com; the others are faithful
 * transcriptions calling Lenovo's own tested libs (so correctness follows from the
 * disassembly, not from a reimplemented algorithm). [] marks the omitted gate.
 *
 * Build:  cc -O2 -Wall -o wwan-orch src/wwan-orch.c -ldl
 * Run:    sudo ./wwan-orch --family fxn
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <glob.h>
#include <unistd.h>

#define DEF_LIBDIR "/opt/fcc_lenovo/lib"
#define DEF_SARDIR "/opt/fcc_lenovo/sar_config_files"
#define DEF_DEV    "/dev/wwan0mbim0"
#define DEF_DPRXML "/opt/fcc_lenovo/DPRConfig.xml"   /* EM05-G DPR band tables (installed) */

static const char *opt_family = NULL;
static const char *opt_dev    = DEF_DEV;
static const char *opt_libdir = DEF_LIBDIR;
static const char *opt_sardir = DEF_SARDIR;
static int          opt_sar   = 0;   /* do SAR config instead of FCC unlock */

/* flags = RTLD_LAZY (fxn/modemauth) or RTLD_NOW (cs24), matching Lenovo. No
 * RTLD_GLOBAL: keep each lib's symbols in local scope, as the vendor does. */
static void *open_lib(const char *name, int flags)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s", opt_libdir, name);
    void *h = dlopen(path, flags);
    if (!h) fprintf(stderr, "dlopen %s failed: %s\n", path, dlerror());
    return h;
}

/* ---- Foxconn T99W696 (SDX61/62) : libfiisdk -------------------------------
 * Mirror of setFccUnlock_fxn, minus GetCountry:
 *   ModuleConnect(dev) ; Fox_Attempt() ; ModuleDisconnect()
 * Fox_Attempt returns a char (1 = success), same as the vendor path. */
static int unlock_fxn(void)
{
    void *h = open_lib("libfiisdk.so.2.2.2", RTLD_LAZY);
    if (!h) return 2;

    int  (*ModuleConnect)(const char *)  = dlsym(h, "ModuleConnect");
    char (*Fox_Attempt)(void)            = dlsym(h, "Fox_Attempt");
    int  (*ModuleDisconnect)(void)       = dlsym(h, "ModuleDisconnect");
    /* GetCountry is deliberately NOT resolved or called. */

    if (!ModuleConnect || !Fox_Attempt || !ModuleDisconnect) {
        fprintf(stderr, "missing symbol in libfiisdk: MC=%p FA=%p MD=%p\n",
                (void *)ModuleConnect, (void *)Fox_Attempt, (void *)ModuleDisconnect);
        dlclose(h); return 2;
    }

    printf("ModuleConnect(%s)...\n", opt_dev);
    int cr = ModuleConnect(opt_dev);
    printf("  connect rc=%d\n", cr);
    if (cr != 0) {   /* vendor skips Fox_Attempt entirely on connect failure */
        fprintf(stderr, "ModuleConnect failed (rc=%d); not attempting unlock\n", cr);
        dlclose(h); return 1;
    }

    printf("Fox_Attempt() [US-SIM gate skipped]...\n");
    char ok = Fox_Attempt();
    printf("  Fox_Attempt=%d\n", (int)ok);

    ModuleDisconnect();
    dlclose(h);

    if (ok) { printf("FCC unlock: SUCCESS\n"); return 0; }
    printf("FCC unlock: FAILED\n");
    return 1;
}

/* First 4 chars of the DMI product name = Lenovo machine type (21V7CTO1WW->21V7). */
static int dmi_machine_type(char *out, size_t n)
{
    FILE *f = fopen("/sys/class/dmi/id/product_name", "r");
    if (!f) return -1;
    char buf[128] = {0};
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return -1; }
    fclose(f);
    /* first token only, then require 4 alphanumeric chars (firmware-controlled). */
    buf[strcspn(buf, " \t\r\n")] = 0;
    for (int i = 0; i < 4; i++)
        if (!((buf[i]>='0'&&buf[i]<='9')||(buf[i]>='A'&&buf[i]<='Z')||(buf[i]>='a'&&buf[i]<='z')))
            return -1;
    snprintf(out, n, "%.4s", buf);
    return 0;
}

/* Full DMI product family, e.g. "ThinkPad L13 Gen 4" — this is the string
 * mbim_set_dprconfig matches against <project name="..."> in the DPR XML, and
 * what configservice_lenovo passes as set_sar_value's a3 (project) arg. */
static int dmi_product_family(char *out, size_t n)
{
    FILE *f = fopen("/sys/class/dmi/id/product_family", "r");
    if (!f) return -1;
    if (!fgets(out, n, f)) { fclose(f); return -1; }
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
    return out[0] ? 0 : -1;
}

/* CPU vendor as "Intel"/"AMD" — set_sar_value's a4 (processor) arg, matched
 * against <processor manufacturer="..."> in the DPR XML. Mirrors Lenovo's own
 * `cat /proc/cpuinfo | grep vendor_id` + strstr "Intel"/"AMD". */
static const char *cpu_vendor(void)
{
    FILE *f = fopen("/proc/cpuinfo", "r");
    if (!f) return NULL;
    char buf[256]; const char *r = NULL;
    while (fgets(buf, sizeof buf, f)) {
        if (strncmp(buf, "vendor_id", 9) != 0) continue;
        if (strstr(buf, "GenuineIntel")) r = "Intel";
        else if (strstr(buf, "AuthenticAMD")) r = "AMD";
        break;
    }
    fclose(f);
    return r;
}

/* lowercase, keep only [a-z0-9] — for loose product-name matching against the
 * EM05CN .bin filenames (which use "ThinkPad-X1-Carbon-Gen-12" style tokens). */
static void norm_alnum(char *out, size_t n, const char *in)
{
    size_t j = 0;
    for (; *in && j + 1 < n; in++) {
        char c = *in;
        if (c >= 'A' && c <= 'Z') c += 32;
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out[j++] = c;
    }
    out[j] = 0;
}

/* ---- Foxconn SAR : libfiisdk Set_RF_Files, gate-free ----------------------
 * Mirror of checkSARConfig_SDX61 -> Set_RF_Files(chassis .bin), minus the
 * US-SIM gate (which lives in configservice_lenovo, not the lib). Set_RF_Files
 * does its own compare-and-skip, battery check, decrypt and write, so on an
 * already-provisioned modem it is a no-op. */
static int sar_fxn(void)
{
    char mt[8];
    if (dmi_machine_type(mt, sizeof mt) != 0) {
        fprintf(stderr, "cannot read DMI machine type\n"); return 2;
    }
    char pat[512];
    snprintf(pat, sizeof pat, "%s/cs26/fxn/*_%s.bin", opt_sardir, mt);
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "no SAR table for machine type %s under %s — skipping\n",
                mt, opt_sardir);
        globfree(&g); return 3;
    }
    char bin[1024];
    snprintf(bin, sizeof bin, "%s", g.gl_pathv[0]);
    globfree(&g);
    printf("chassis %s -> %s\n", mt, bin);

    void *h = open_lib("libfiisdk.so.2.2.2", RTLD_LAZY);
    if (!h) return 2;
    int (*ModuleConnect)(const char *) = dlsym(h, "ModuleConnect");
    int (*Set_RF_Files)(const char *)  = dlsym(h, "Set_RF_Files");
    int (*ModuleDisconnect)(void)      = dlsym(h, "ModuleDisconnect");
    if (!ModuleConnect || !Set_RF_Files || !ModuleDisconnect) {
        fprintf(stderr, "missing symbol in libfiisdk (MC=%p SRF=%p MD=%p)\n",
                (void *)ModuleConnect, (void *)Set_RF_Files, (void *)ModuleDisconnect);
        dlclose(h); return 2;
    }
    int cr = ModuleConnect(opt_dev);
    if (cr != 0) {
        fprintf(stderr, "ModuleConnect failed (rc=%d); not applying SAR\n", cr);
        dlclose(h); return 2;
    }
    printf("Set_RF_Files() [US-SIM gate skipped]...\n");
    int r = Set_RF_Files(bin);
    ModuleDisconnect();
    dlclose(h);
    printf("SAR config rc=%d (Set_RF_Files skips the write if already current)\n", r);
    return r == 0 ? 0 : 1;
}

/* ---- SAR via Lenovo's libconfigservice* : FM350/L860/RW101/RW350 ----------
 * Mirror of configservice_lenovo's checkSARConfig_<fam>: dlopen the family's
 * libconfigservice* and call its self-contained configservice_* entry (no args,
 * returns int). The US-SIM SAR gate lives only in configservice_lenovo (verified:
 * 0 gate strings in any libconfigservice* lib), so calling the entry directly is
 * gate-free.
 *   fm350  libconfigservice350.so       configservice_fm350
 *   l860   libconfigserviceR+.so        configservice_rplus
 *   rw101  libconfigservice101.so.1.2   configservice_101
 *   rw350  libconfigservice350.so.1.2   configservice_350
 */
static int sar_configservice(const char *lib, const char *sym)
{
    void *h = open_lib(lib, RTLD_LAZY);
    if (!h) return 2;
    int (*fn)(void) = (int (*)(void))dlsym(h, sym);
    if (!fn) {
        fprintf(stderr, "missing %s in %s: %s\n", sym, lib, dlerror());
        dlclose(h); return 2;
    }
    printf("%s() [US-SIM SAR gate skipped]...\n", sym);
    int r = fn();
    dlclose(h);
    printf("SAR config rc=%d\n", r);
    return r == 0 ? 0 : 1;
}

/* ---- Quectel SAR (cs24 = EM160/EM061K/RM520) : libmbimtools.so, gate-free ---
 * Faithful transcription of configservice_lenovo's setSARConfig_common minus the
 * US-SIM gate. It reuses Lenovo's OWN apply code end to end — every step below is
 * an exported entry of the bundled libmbimtools.so, called exactly as the
 * orchestrator calls it (all verified against the disassembly of both binaries):
 *
 *   ops = dlsym("mbim_sar_ops")                       // struct of fn pointers
 *   ops[1].init(dev)      (retry <=9x, sleep 3)        // mbim_ctx_init; ALSO calls
 *                                                       // mbim_get_module_type() and
 *                                                       // strcpy()s it into the lib's
 *                                                       // s_module_type global
 *   [US-SIM gate]                                       <- omitted
 *   info = ops[6].preprocess_inpput_files(base,1,1,get_nv)
 *          // for each of `count` entries (base+i*0x100): decrypt_bin_to_file()
 *          //   (RSA-decrypt the signed .bin to a temp) and fill sar_file_info[i]:
 *          //   +0x00 = s_module_type, +0x10 = decrypted path,
 *          //   +0x110 = get_nv(path)  (the NV-version), +0x114 = 1
 *          // returns &sar_file_info.0 (or NULL on decrypt failure)
 *   ops[5].set_sar_value(info,1,0,0,0)
 *          // module_type != em05g -> send_nv_to_modem(info,count,force=0) which
 *          //   md5-compares each file vs the modem's current EFS and SKIPS if
 *          //   equal, else mbim_set_sar_enable + mbim_set_sar_value; then two
 *          //   AT commands + mbim_sar_update_commit(); then unlink()s the temps
 *   ops[2].uninit()
 *
 * The lib name is a red herring: setSARConfig_common logs "Open libmbim2sar.so
 * failed!" but the string it passes to dlopen is "/opt/fcc_lenovo/lib/
 * libmbimtools.so" (verified at .rodata 0xdff0) — the lib we already bundle for
 * cs24 FCC. The per-model .bin tables ship in sar_config_files.tar.gz; the cs25/
 * filenames embed the Lenovo machine type. A machine type can map to more than one
 * modem model, so after init we resolve the installed model via
 * mbim_get_module_type() and apply only that model's table — mirroring Lenovo's
 * model-based dispatch (send_nv_to_modem does not itself check the model).
 *
 * The ONLY thing reimplemented here is get_nv_<model>() (it lives in the gated
 * binary, not the lib): each is strstr(path, "<nvver>") -> that version as an int
 * (get_nv_em160: "29619"->29619, "30007"->30007; get_nv_061: "48001"; etc.), else
 * -1. quectel_get_nv() reproduces that by reading the NV-version integer that the
 * filename ends with (..._29619.bin -> 29619) — the value that lands in
 * sar_file_info+0x110 and is applied via mbim_set_sar_value.
 *
 * The md5 compare-and-skip inside send_nv_to_modem makes this a no-op on an
 * already-provisioned modem, and it only ever writes Lenovo's own unmodified,
 * RSA-signed table. UNVERIFIED: no Quectel hardware was available to the
 * maintainer (same status as fm350/l860/rw101/rw350). (EM05 SAR is implemented
 * separately in sar_em05() via the same bundled libmbimtools.so — see below.) */

/* Reimplementation of get_nv_<model>: the NV-version integer the .bin filename
 * ends with, e.g. ".../0118__EM160RGL__ThinkPad-T14-Gen-5__Intel__29619.bin"
 * -> 29619. Returns -1 when there is no trailing version (Lenovo's default). */
static int quectel_get_nv(const char *path)
{
    const char *slash = strrchr(path, '/');
    const char *base  = slash ? slash + 1 : path;
    const char *dot   = strstr(base, ".bin");
    if (!dot) return -1;
    const char *p = dot;
    while (p > base && p[-1] >= '0' && p[-1] <= '9') p--;
    if (p == dot) return -1;                 /* no trailing digits */
    return (int)strtol(p, NULL, 10);
}

static int sar_quectel(void)
{
    char mt[8];
    if (dmi_machine_type(mt, sizeof mt) != 0) {
        fprintf(stderr, "cannot read DMI machine type\n"); return 2;
    }
    /* cs25/ Quectel tables embed the machine type:
     *   <prefix>__<MODEL>__<product-name>__<MT>__<nvver>.bin  */
    char pat[512];
    snprintf(pat, sizeof pat, "%s/cs25/*__%s__*.bin", opt_sardir, mt);
    glob_t g;
    if (glob(pat, 0, NULL, &g) != 0 || g.gl_pathc == 0) {
        fprintf(stderr, "no Quectel SAR table for machine type %s under %s/cs25 — skipping\n",
                mt, opt_sardir);
        globfree(&g); return 0;              /* no matching table -> no-op */
    }
    void *h = open_lib("libmbimtools.so", RTLD_NOW);
    if (!h) { globfree(&g); return 2; }
    void **ops = (void **)dlsym(h, "mbim_sar_ops");
    if (!ops) {
        fprintf(stderr, "mbim_sar_ops missing in libmbimtools.so: %s\n", dlerror());
        dlclose(h); globfree(&g); return 2;
    }
    int  (*sar_init)(const char *)       = (int  (*)(const char *))ops[1];
    int  (*sar_uninit)(void)             = (int  (*)(void))ops[2];
    void*(*preprocess)(const char *, int, int, int (*)(const char *)) =
        (void *(*)(const char *, int, int, int (*)(const char *)))ops[6];
    int  (*set_sar_value)(void *, int, void *, void *, int) =
        (int  (*)(void *, int, void *, void *, int))ops[5];
    if (!ops[1] || !ops[2] || !ops[5] || !ops[6]) {
        fprintf(stderr, "mbim_sar_ops has null slots\n");
        dlclose(h); globfree(&g); return 2;
    }

    int cr = -1;
    for (int i = 0; i < 9 && cr != 0; i++) {   /* mirrors setSARConfig_common */
        cr = sar_init(opt_dev);
        if (cr != 0) sleep(3);
    }
    if (cr != 0) {
        fprintf(stderr, "SAR init failed (rc=%d); not applying\n", cr);
        dlclose(h); globfree(&g); return 1;
    }

    /* A machine type maps to MORE THAN ONE modem model (e.g. a chassis sold with
     * either EM160/RM520/EM061K), and the shipped cs25/ tables include a file per
     * model. send_nv_to_modem only md5-compares vs the modem's EFS — it does NOT
     * check the model — so applying a wrong-model file would write the wrong SAR
     * table. Lenovo avoids this by dispatching on the DETECTED model. We do the
     * same: mbim_get_module_type() (exported; valid after init, which itself
     * queries it via at+qgmr into s_module_type) returns a static constant —
     * "EM160" / "EM061KGL" / "RM520" — each a unique substring of exactly one
     * filename model token (EM160RGL/EM061KGL/RM520NGL), so we apply only files
     * whose name contains it. (Multiple NV-version files for one model, e.g. RM520
     * 29619 + 30007, are intentionally ALL applied: mbim_get_efs_md5 keys the EFS
     * md5 on the per-file version, so they are independent complementary slots —
     * matching Lenovo's count>1 behavior.)
     *
     * Note: on a modem that answers at+qgmr but matches none of the models
     * mbim_get_module_type knows, the vendor lib returns an uninitialized
     * (possibly non-NULL) pointer — inherited behavior that the guard below cannot
     * fully catch. Unreachable in practice here: cs25 tables only exist for the
     * three models above, which the lib recognizes. */
    const char *(*get_model)(void) = (const char *(*)(void))dlsym(h, "mbim_get_module_type");
    const char *model = get_model ? get_model() : NULL;
    if (!model || !*model) {
        fprintf(stderr, "cannot determine modem model; not applying SAR (would risk wrong-model table)\n");
        sar_uninit(); dlclose(h); globfree(&g); return 1;
    }
    printf("[US-SIM SAR gate skipped] modem model = %s\n", model);
    int rc = 0, applied = 0;
    for (size_t i = 0; i < g.gl_pathc; i++) {
        const char *slash = strrchr(g.gl_pathv[i], '/');
        const char *bn = slash ? slash + 1 : g.gl_pathv[i];
        if (!strstr(bn, model)) continue;              /* skip other models' tables */
        /* preprocess reads entries at base + n*0x100; count=1 uses only base+0.
         * A single-entry buffer >= 0x100 bytes holding the .bin path suffices. */
        char base[0x100] = {0};
        if (snprintf(base, sizeof base, "%s", g.gl_pathv[i]) >= (int)sizeof base) {
            fprintf(stderr, "path too long, skipping: %s\n", g.gl_pathv[i]); rc = 1; continue;
        }
        applied++;
        printf("preprocess %s (nv=%d) ...\n", base, quectel_get_nv(base));
        void *info = preprocess(base, 1, 1, quectel_get_nv);   /* decrypt + build sar_file_info */
        if (!info) { fprintf(stderr, "decrypt/preprocess failed for %s\n", base); rc = 1; continue; }
        printf("set_sar_value() [md5 compare-and-skip, then commit] ...\n");
        if (set_sar_value(info, 1, NULL, NULL, 0) != 0) rc = 1;
    }
    if (!applied)
        fprintf(stderr, "no %s SAR table for machine type %s — nothing applied\n", model, mt);
    sar_uninit();
    dlclose(h); globfree(&g);
    printf("Quectel SAR rc=%d\n", rc);
    return rc;
}

/* ---- EM05 SAR : libmbimtools.so, gate-free -------------------------------
 * Covers BOTH EM05 variants (see docs/configservice_lenovo-map.md). In the Lenovo
 * software EM05-CN (module.0==6) goes through setSARConfig_common and EM05-G
 * (module.0==3) through setSARConfig_em05 (which targets the absent
 * /usr/lib/mbim2sar_em05.so). Both converge on libmbimtools.so's set_sar_value
 * EM05 branch, so we reimplement both here through the bundled mbim_sar_ops:
 *
 *   ops[1].init(dev)                 mbim_ctx_init + mbim_is_ready; mbim_get_module_type
 *                                    (at+qgmr) sets s_module_type = "EM05CN"/"EM05G"
 *   [US-SIM gate]                    <- omitted (map §2: the gate is only in the FCC
 *                                       path setFccUnlock_em05, never in SAR)
 *   ops[6].preprocess_inpput_files(x,1,decrypt,NULL)
 *                                    builds sar_file_info: +0x00=s_module_type,
 *                                    +0x10=DPR-XML path, +0x110=-1 (get_nv_em05cn
 *                                    is a stub returning -1, so NULL callback == it)
 *       EM05-CN: x = shipped EM05CN .bin (encrypted), decrypt=1 -> decrypt_bin_to_file
 *                produces the plaintext DPR XML; matched to this machine by product
 *                family (files: X1C-G12/X13-G5/X13-2in1-G5/T14-G5, __Intel)
 *       EM05-G : x = the extracted DPRConfig.xml (plaintext), decrypt=0 -> path as-is
 *   ops[5].set_sar_value(info,1,project,processor,0)
 *                                    EM05 branch -> mbim_set_dprconfig(info+0x10 xml,
 *                                    s_module_type, project, processor): parses
 *                                    <project name>/<processor manufacturer>/<dpr …>
 *                                    and emits at+qcfg="sarcfg",… ; then AT + commit.
 *                                    set_sar_value unlinks the decrypted temp itself.
 *   ops[2].uninit()
 *
 * project = DMI product_family ("ThinkPad L13 Gen 4"), matched against the XML's
 * <project name>; processor = Intel/AMD from /proc/cpuinfo. The RSA .sig verify the
 * stock EM05-G path does on the on-disk XML is an integrity check, not an RF step,
 * and is omitted (the XML is the trusted embedded blob). UNVERIFIED: no EM05 hw. */
static int sar_em05(void)
{
    const char *proc = cpu_vendor();
    char project[128];
    if (dmi_product_family(project, sizeof project) != 0) {
        fprintf(stderr, "cannot read DMI product family (needed as DPR project name)\n"); return 2;
    }
    if (!proc) { fprintf(stderr, "cannot determine CPU vendor (Intel/AMD)\n"); return 2; }

    void *h = open_lib("libmbimtools.so", RTLD_NOW);
    if (!h) return 2;
    void **ops = (void **)dlsym(h, "mbim_sar_ops");
    const char *(*get_model)(void) = (const char *(*)(void))dlsym(h, "mbim_get_module_type");
    if (!ops || !get_model) {
        fprintf(stderr, "libmbimtools.so missing mbim_sar_ops/mbim_get_module_type\n");
        dlclose(h); return 2;
    }
    if (!ops[1] || !ops[2] || !ops[5] || !ops[6]) {
        fprintf(stderr, "mbim_sar_ops has null slots\n"); dlclose(h); return 2;
    }
    int  (*sar_init)(const char *) = (int (*)(const char *))ops[1];
    int  (*sar_uninit)(void)       = (int (*)(void))ops[2];
    void*(*preprocess)(const char *, int, int, int (*)(const char *)) =
        (void *(*)(const char *, int, int, int (*)(const char *)))ops[6];
    int  (*set_sar_value)(void *, int, const void *, const void *, int) =
        (int (*)(void *, int, const void *, const void *, int))ops[5];

    int cr = -1;
    for (int i = 0; i < 9 && cr != 0; i++) { cr = sar_init(opt_dev); if (cr != 0) sleep(3); }
    if (cr != 0) { fprintf(stderr, "SAR init failed (rc=%d); not applying\n", cr); dlclose(h); return 1; }

    const char *model = get_model();       /* "EM05CN" / "EM05G" */
    if (!model || !*model) {
        fprintf(stderr, "cannot determine EM05 model; not applying\n"); sar_uninit(); dlclose(h); return 1;
    }
    printf("[US-SIM SAR gate skipped] model=%s project=\"%s\" cpu=%s\n", model, project, proc);

    char inbuf[0x100] = {0};
    int decrypt = 0;
    if (strstr(model, "EM05CN")) {
        char pat[512];
        snprintf(pat, sizeof pat, "%s/*EM05CN*.bin", opt_sardir);
        glob_t g; int matched = 0;
        char nf[128]; norm_alnum(nf, sizeof nf, project);
        if (glob(pat, 0, NULL, &g) == 0) {
            for (size_t i = 0; i < g.gl_pathc; i++) {
                char nb[512]; norm_alnum(nb, sizeof nb, g.gl_pathv[i]);
                if (nf[0] && strstr(nb, nf)) {
                    snprintf(inbuf, sizeof inbuf, "%s", g.gl_pathv[i]); matched = 1; break;
                }
            }
        }
        globfree(&g);
        if (!matched) {
            fprintf(stderr, "no EM05CN SAR table for \"%s\" under %s — nothing applied\n", project, opt_sardir);
            sar_uninit(); dlclose(h); return 0;
        }
        decrypt = 1;
    } else if (strstr(model, "EM05G")) {
        if (access(DEF_DPRXML, R_OK) != 0) {
            fprintf(stderr, "EM05-G DPR config %s not installed — nothing applied\n", DEF_DPRXML);
            sar_uninit(); dlclose(h); return 0;
        }
        snprintf(inbuf, sizeof inbuf, "%s", DEF_DPRXML);
        decrypt = 0;
    } else {
        fprintf(stderr, "modem model %s is not an EM05 variant; not applying\n", model);
        sar_uninit(); dlclose(h); return 1;
    }

    printf("preprocess %s (decrypt=%d) ...\n", inbuf, decrypt);
    void *info = preprocess(inbuf, 1, decrypt, NULL);   /* NULL callback == get_nv_em05cn stub (-1) */
    if (!info) { fprintf(stderr, "preprocess/decrypt failed for %s\n", inbuf); sar_uninit(); dlclose(h); return 1; }
    printf("set_sar_value() [EM05 dprconfig branch: at+qcfg sarcfg + commit] ...\n");
    int r = set_sar_value(info, 1, project, proc, 0);
    sar_uninit();
    dlclose(h);
    printf("EM05 SAR rc=%d\n", r);
    return r == 0 ? 0 : 1;
}

/* ---- libmodemauth family : Fibocom FM350/L860, Rolling RW101/RW350 --------
 * Their DPR dispatch (fccunlock_rw101/rw350/fm350_l860) resolves get_country_code
 * (the gate) + init_modemauth_srvc[/_mbim], calls the gate, then the unlock. We
 * call only the unlock. The lib does the rest (incl. RW101's device-derived key
 * via configure_wwan_sec_key). Confirmed: init_modemauth_srvc does not self-gate.
 *   rw101  libmodemauthRW101.so.1.1  init_modemauth_srvc      /dev/wwan0mbim0
 *   rw350  libmodemauth.so.1.1       init_modemauth_srvc      /dev/wwan0mbim0
 *   fm350  libmodemauth.so           init_modemauth_srvc      /dev/wwan0at0
 *   l860   libmodemauth.so           init_modemauth_srvc_mbim /dev/wwan0mbim0
 */
static int unlock_modemauth(const char *lib, const char *sym, const char *dev)
{
    void *h = open_lib(lib, RTLD_LAZY);
    if (!h) return 2;
    int (*fn)(const char *) = dlsym(h, sym);
    if (!fn) {
        fprintf(stderr, "missing %s in %s: %s\n", sym, lib, dlerror());
        dlclose(h); return 2;
    }
    printf("%s(%s) [US-SIM gate skipped]...\n", sym, dev);
    int r = fn(dev);
    dlclose(h);
    if (r == 0) { printf("FCC unlock: SUCCESS\n"); return 0; }
    printf("FCC unlock: FAILED (rc=%d)\n", r);
    return 1;
}

/* ---- Quectel (cs24) : libmbimtools mbim_fcc_ops function-pointer table -----
 * From setFccUnlock_cs24: dlopen libmbimtools, dlsym the mbim_fcc_ops struct,
 * then call, in order: ops[+0x8](dev)=init ; ops[+0x20]()=location_is_USA (GATE,
 * skipped) ; ops[+0x18]()=fcc_unlock. As a pointer array those byte offsets are
 * indices 1, 4, 3. Covers RM520N/EM160/EM061K etc. */
static int unlock_quectel_cs24(void)
{
    void *h = open_lib("libmbimtools.so", RTLD_NOW);
    if (!h) return 2;
    void **ops = (void **)dlsym(h, "mbim_fcc_ops");
    if (!ops) {
        fprintf(stderr, "mbim_fcc_ops not found in libmbimtools: %s\n", dlerror());
        dlclose(h); return 2;
    }
    int  (*fcc_init)(const char *) = (int (*)(const char *))ops[1];  /* +0x8  */
    void (*fcc_uninit)(void)       = (void (*)(void))ops[2];         /* +0x10, teardown */
    int  (*fcc_unlock)(void)       = (int (*)(void))ops[3];          /* +0x18 */
    /* ops[4] (+0x20) = location_is_USA — deliberately NOT called. */
    if (!fcc_init || !fcc_unlock) {
        fprintf(stderr, "mbim_fcc_ops has null members (init=%p unlock=%p)\n",
                (void *)fcc_init, (void *)fcc_unlock);
        dlclose(h); return 2;
    }
    printf("mbim_fcc_ops init(%s)...\n", opt_dev);
    int ir = fcc_init(opt_dev);
    printf("  init rc=%d\n", ir);
    if (ir != 0) {   /* vendor jumps straight to dlclose here — no uninit */
        fprintf(stderr, "mbim_fcc_ops init failed (rc=%d)\n", ir);
        dlclose(h); return 1;
    }
    printf("mbim_fcc_ops fcc_unlock() [location_is_USA skipped]...\n");
    int r = fcc_unlock();
    if (fcc_uninit) fcc_uninit();   /* vendor always calls uninit after unlock */
    dlclose(h);
    if (r == 0) { printf("FCC unlock: SUCCESS\n"); return 0; }
    printf("FCC unlock: FAILED (rc=%d)\n", r);
    return 1;
}

/* ---- families whose entry symbol still needs on-hardware confirmation ------ */
static int unlock_todo(const char *fam, const char *lib)
{
    fprintf(stderr,
        "family '%s' not yet implemented.\n"
        "Its unlock lives in %s (from the DPR_Fcc_unlock_service disassembly),\n"
        "but the exact entry symbol/signature needs confirming on real hardware.\n"
        "See docs/ADDING-HARDWARE.md; contributions with a test report welcome.\n",
        fam, lib);
    return 3;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--family") && i + 1 < argc) opt_family = argv[++i];
        else if (!strcmp(argv[i], "--device") && i + 1 < argc) opt_dev = argv[++i];
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) opt_dev = argv[++i];
        else if (!strcmp(argv[i], "--libdir") && i + 1 < argc) opt_libdir = argv[++i];
        else if (!strcmp(argv[i], "--sardir") && i + 1 < argc) opt_sardir = argv[++i];
        else if (!strcmp(argv[i], "--sar")) opt_sar = 1;
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("usage: wwan-orch --family <fxn|fm350|l860|rw101|rw350|cs24|em05> "
                   "[--sar] [--device %s] [--libdir %s] [--sardir %s]\n",
                   DEF_DEV, DEF_LIBDIR, DEF_SARDIR);
            return 0;
        } else { fprintf(stderr, "unknown arg: %s\n", argv[i]); return 1; }
    }
    if (!opt_family) { fprintf(stderr, "need --family (see --help)\n"); return 1; }

    if (opt_sar) {
        if (!strcmp(opt_family, "fxn"))   return sar_fxn();
        if (!strcmp(opt_family, "fm350")) return sar_configservice("libconfigservice350.so", "configservice_fm350");
        if (!strcmp(opt_family, "l860"))  return sar_configservice("libconfigserviceR+.so", "configservice_rplus");
        if (!strcmp(opt_family, "rw101")) return sar_configservice("libconfigservice101.so.1.2", "configservice_101");
        if (!strcmp(opt_family, "rw350")) return sar_configservice("libconfigservice350.so.1.2", "configservice_350");
        /* cs24 (EM160/EM061K/RM520): faithful transcription of setSARConfig_common
         * reusing libmbimtools.so's own ops[1/6/5/2] — see sar_quectel(). */
        if (!strcmp(opt_family, "cs24")) return sar_quectel();
        /* em05 (EM05-CN + EM05-G): both apply via libmbimtools.so's set_sar_value
         * EM05 branch (mbim_set_dprconfig) — see sar_em05(). No mbim2sar_em05.so
         * needed; its logic is duplicated in the bundled libmbimtools.so. */
        if (!strcmp(opt_family, "em05")) return sar_em05();
        return unlock_todo(opt_family, "SAR");
    }

    if (!strcmp(opt_family, "fxn"))   return unlock_fxn();
    if (!strcmp(opt_family, "rw101"))
        return unlock_modemauth("libmodemauthRW101.so.1.1", "init_modemauth_srvc", opt_dev);
    if (!strcmp(opt_family, "rw350"))
        return unlock_modemauth("libmodemauth.so.1.1", "init_modemauth_srvc", opt_dev);
    if (!strcmp(opt_family, "fm350"))
        return unlock_modemauth("libmodemauth.so", "init_modemauth_srvc",
                                strcmp(opt_dev, DEF_DEV) ? opt_dev : "/dev/wwan0at0");
    if (!strcmp(opt_family, "l860"))
        return unlock_modemauth("libmodemauth.so", "init_modemauth_srvc_mbim", opt_dev);
    if (!strcmp(opt_family, "cs24"))  return unlock_quectel_cs24();
    /* em05: Lenovo's DPR_Fcc_unlock_service DOES contain a setFccUnlock_em05 that
     * loads /usr/lib/mbim2sar_em05.so — but it is DEAD CODE: 0 call sites in the
     * binary. main dispatches EVERY Quectel modem (EM05 included) through
     * setFccUnlock_cs24 (libmbimtools.so / mbim_fcc_ops). "WWAN EM05 device found"
     * is only a log line in the detection routine (isusbWWANModuleInstalled). So
     * the faithful live FCC path for EM05 is exactly the cs24 path — mbim2sar_em05.so
     * is never used for FCC. (It IS used for EM05 *SAR* via setSARConfig_em05 — a
     * separate, in-binary path we don't reimplement; see the --sar dispatch.) */
    if (!strcmp(opt_family, "em05"))  return unlock_quectel_cs24();

    fprintf(stderr, "unknown family '%s'\n", opt_family);
    return 1;
}
