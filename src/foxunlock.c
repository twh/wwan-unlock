/* SPDX-License-Identifier: MIT
 *
 * foxunlock — clean-room FCC unlock for Foxconn T99W696 (SDX61/62, PCI 17cb:0308)
 *
 * Reimplemented from Lenovo's libfiisdk.so.2.2.2 disassembly
 * (algorithm + wire format, not copyrightable), using none of their code at
 * runtime. Transport is raw QMI-over-MBIM via libmbim.
 *
 * Why libmbim and not qmicli/libqmi:
 *   The unlock message lives on QMI service 0xE4 (FOXAP). libqmi only knows the
 *   FOX service (0xE3) and refuses to allocate a client on 0xE4 ("Clients for
 *   service (null) not supported"); qmicli's --fox-set-fcc-authentication (0xE3)
 *   returns MalformedMessage, and --dms-foxconn-set-fcc-authentication-v2 returns
 *   WmsInvalidMessageId. So we do exactly what libfiisdk does: allocate a client
 *   id for 0xE4 via QMI CTL, then send the FOXAP Set-FCC message, both as raw
 *   QMUX frames tunnelled through MBIM_CID_QMI_MSG.
 *
 * Algorithm (FoxApSetFccLockStatus / Fox_Attempt):
 *   firmware = fox-get-firmware-version(firmware-mcfg), drop last 2 dot-fields,
 *              concatenated with fox-get-firmware-version(apps)
 *   imei     = dms-get-ids IMEI
 *   salt     = 4 chars ; magic = "FDE2" (b_char_value("ighU"), byte-0x23)
 *   auth     = salt + md5_hex( firmware + imei + salt + magic )   [36 bytes]
 *
 * Wire (QMIFOXAPSetFccLockStatus): service 0xE4, msg 0x5571,
 *   TLV 0x01 = auth (36), TLV 0x02 = 1 byte '0' (unlock) / '1' (lock).
 *
 * Build:  make    (needs libmbim-glib-dev libqmi-glib-dev pkgconf)
 * Run:    sudo ./foxunlock -d /dev/wwan0mbim0
 */

#include <glib.h>
#include <gio/gio.h>
#include <libmbim-glib.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>

/* Hard watchdog: when invoked as a ModemManager fcc-unlock dispatcher the call
 * is synchronous, so we must never block. This backstops both the qmicli reads
 * (g_spawn_sync has no timeout) and the MBIM I/O. On expiry the process exits
 * non-zero; MM then simply retries later instead of stalling. */
#define WATCHDOG_SECS 12
static void on_alarm(int sig)
{
    (void)sig;
    static const char m[] = "foxunlock: watchdog timeout\n";
    ssize_t r = write(2, m, sizeof(m) - 1); (void)r;
    _exit(3);
}

#define QMI_SERVICE_FOXAP    0xE4
#define QMI_MSG_SET_FCC      0x5571
#define FCC_MAGIC            "FDE2"

static const char *SALT_ALPHABET =
    "abcdefghijklmnopqrstuvwxyz00112233445566778899";

static char   *opt_device  = NULL;
static char   *opt_fw_mcfg = NULL;
static char   *opt_fw_apps = NULL;
static char   *opt_imei    = NULL;
static gint    opt_salt_len = 4;
static gint    opt_service  = QMI_SERVICE_FOXAP;
static gboolean opt_lock     = FALSE;
static gboolean opt_emit_only = FALSE;

static GOptionEntry entries[] = {
    { "device",   'd', 0, G_OPTION_ARG_FILENAME, &opt_device,   "MBIM device (/dev/wwan0mbim0)", "PATH" },
    { "fw-mcfg",   0,  0, G_OPTION_ARG_STRING,   &opt_fw_mcfg,  "firmware mcfg version (override, already truncated)", "STR" },
    { "fw-apps",   0,  0, G_OPTION_ARG_STRING,   &opt_fw_apps,  "firmware apps version (override)", "STR" },
    { "imei",      0,  0, G_OPTION_ARG_STRING,   &opt_imei,     "IMEI (override)", "STR" },
    { "salt-len",  0,  0, G_OPTION_ARG_INT,      &opt_salt_len, "salt length (default 4)", "N" },
    { "service",   0,  0, G_OPTION_ARG_INT,      &opt_service,  "QMI service id (default 228 = 0xE4)", "N" },
    { "lock",      0,  0, G_OPTION_ARG_NONE,     &opt_lock,     "send lock flag '1' instead of unlock '0'", NULL },
    { "emit-frame",0,  0, G_OPTION_ARG_NONE,     &opt_emit_only,"print auth + raw frames, do not send", NULL },
    { NULL }
};

/* ---- input acquisition (qmicli; the read side is standard and works) ------ */

static char *run_capture(const char *const argv[])
{
    gchar *out = NULL;
    g_spawn_sync(NULL, (gchar **)argv, NULL,
                 G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                 NULL, NULL, &out, NULL, NULL, NULL);
    return out;
}

static char *grab_quoted(const char *text, const char *label)
{
    if (!text) return NULL;
    const char *p = strstr(text, label);
    if (!p) return NULL;
    p = strchr(p, '\''); if (!p) return NULL; p++;
    const char *q = strchr(p, '\''); if (!q) return NULL;
    return g_strndup(p, q - p);
}

static void autoread(const char *port, char **fw_mcfg, char **fw_apps, char **imei)
{
    if (!*fw_mcfg) {
        const char *a[] = {"qmicli","-d",port,"-p",
                           "--fox-get-firmware-version=firmware-mcfg",NULL};
        char *o = run_capture(a);
        char *v = grab_quoted(o, "Version:");
        if (v) {
            char *d1 = strrchr(v, '.');
            if (d1) { *d1 = 0; char *d2 = strrchr(v, '.'); if (d2) *d2 = 0; }
            *fw_mcfg = v;
        }
        g_free(o);
    }
    if (!*fw_apps) {
        const char *a[] = {"qmicli","-d",port,"-p",
                           "--fox-get-firmware-version=apps",NULL};
        char *o = run_capture(a);
        *fw_apps = grab_quoted(o, "Version:");
        g_free(o);
    }
    if (!*imei) {
        const char *a[] = {"qmicli","-d",port,"-p","--dms-get-ids",NULL};
        char *o = run_capture(a);
        *imei = grab_quoted(o, "IMEI:");
        g_free(o);
    }
}

static char *make_salt(int n)
{
    int alen = strlen(SALT_ALPHABET);
    char *s = g_malloc0(n + 1);
    srand((unsigned)time(NULL));
    for (int i = 0; i < n; i++) s[i] = SALT_ALPHABET[rand() % alen];
    return s;
}

static char *compute_auth(const char *fw_mcfg, const char *fw_apps,
                          const char *imei, const char *salt)
{
    char *firmware   = g_strconcat(fw_mcfg, fw_apps, NULL);
    char *hash_input = g_strconcat(firmware, imei, salt, FCC_MAGIC, NULL);
    GChecksum *c = g_checksum_new(G_CHECKSUM_MD5);
    g_checksum_update(c, (const guchar *)hash_input, strlen(hash_input));
    char *auth = g_strconcat(salt, g_checksum_get_string(c), NULL);  /* 4 + 32 */
    g_free(firmware); g_free(hash_input); g_checksum_free(c);
    return auth;
}

/* ---- raw QMUX frame builders (full frames, incl. 0x01 marker) ------------- */

/* QMI CTL Allocate Client ID (msg 0x0022), TLV 0x01 = service byte. */
static GByteArray *build_ctl_alloc(guint8 service)
{
    guint8 f[] = {
        0x01,             /* IF marker                         */
        0x0f, 0x00,       /* frame length (= total - 1 = 15)   */
        0x00,             /* control flag (host)               */
        0x00,             /* service = CTL                     */
        0x00,             /* client id                         */
        0x00,             /* CTL control flag (request)        */
        0x01,             /* CTL transaction id (1 byte)       */
        0x22, 0x00,       /* message id 0x0022                 */
        0x04, 0x00,       /* TLV block length = 4              */
        0x01, 0x01, 0x00, service  /* TLV 0x01, len 1, service */
    };
    GByteArray *a = g_byte_array_new();
    g_byte_array_append(a, f, sizeof(f));
    return a;
}

/* QMI FOXAP Set-FCC (msg 0x5571) on `service`/`cid`, auth TLV + flag TLV. */
static GByteArray *build_unlock(guint8 service, guint8 cid,
                                const char *auth, guint8 flag)
{
    guint16 alen    = (guint16)strlen(auth);      /* 36 */
    guint16 payload = (guint16)(3 + alen + 4);    /* TLV1 + TLV2 = 43 */
    guint16 lenfld  = (guint16)(13 + payload - 1);
    guint8 h[13] = {
        0x01,
        (guint8)(lenfld & 0xff), (guint8)(lenfld >> 8),
        0x00,
        service, cid,
        0x00,                         /* SDU control flag (request) */
        0x02, 0x00,                   /* transaction id             */
        0x71, 0x55,                   /* message id 0x5571          */
        (guint8)(payload & 0xff), (guint8)(payload >> 8)
    };
    guint8 t1[3] = { 0x01, (guint8)(alen & 0xff), (guint8)(alen >> 8) };
    guint8 t2[4] = { 0x02, 0x01, 0x00, flag };
    GByteArray *a = g_byte_array_new();
    g_byte_array_append(a, h, sizeof(h));
    g_byte_array_append(a, t1, sizeof(t1));
    g_byte_array_append(a, (const guint8 *)auth, alen);
    g_byte_array_append(a, t2, sizeof(t2));
    return a;
}

/* QMI CTL Release Client ID (msg 0x0023), TLV 0x01 = {service, cid}. */
static GByteArray *build_ctl_release(guint8 service, guint8 cid)
{
    guint8 f[] = {
        0x01,
        0x10, 0x00,       /* frame length (= total - 1 = 16)   */
        0x00,             /* control flag                      */
        0x00,             /* service = CTL                     */
        0x00,             /* client id                         */
        0x00,             /* CTL control flag (request)        */
        0x02,             /* CTL transaction id                */
        0x23, 0x00,       /* message id 0x0023                 */
        0x05, 0x00,       /* TLV block length = 5              */
        0x01, 0x02, 0x00, service, cid  /* TLV 0x01, len 2, {service,cid} */
    };
    GByteArray *a = g_byte_array_new();
    g_byte_array_append(a, f, sizeof(f));
    return a;
}

/* CTL Allocate-CID response: TLV 0x01 value = {service, cid}. */
static gboolean parse_ctl_cid(const guint8 *q, guint32 n, guint8 *out_cid)
{
    if (n < 12) return FALSE;
    guint16 tlvtot = q[10] | (q[11] << 8);
    guint32 end = 12 + tlvtot; if (end > n) end = n;
    for (guint32 p = 12; p + 3 <= end; ) {
        guint8 t = q[p]; guint16 l = q[p+1] | (q[p+2] << 8);
        if (p + 3 + l > end) break;
        if (t == 0x01 && l >= 2) { *out_cid = q[p+4]; return TRUE; }
        p += 3 + l;
    }
    return FALSE;
}

/* Service response: FOX result TLV 0x02 = {u16 result, u16 error}. */
static gboolean parse_fox_result(const guint8 *q, guint32 n,
                                 guint16 *result, guint16 *err)
{
    if (n < 13) return FALSE;
    guint16 tlvtot = q[11] | (q[12] << 8);
    guint32 end = 13 + tlvtot; if (end > n) end = n;
    for (guint32 p = 13; p + 3 <= end; ) {
        guint8 t = q[p]; guint16 l = q[p+1] | (q[p+2] << 8);
        if (p + 3 + l > end) break;
        if (t == 0x02 && l >= 4) {
            *result = q[p+3] | (q[p+4] << 8);
            *err    = q[p+5] | (q[p+6] << 8);
            return TRUE;
        }
        p += 3 + l;
    }
    return FALSE;
}

/* ---- MBIM async flow ------------------------------------------------------ */

typedef struct { char *auth; guint8 service; guint8 flag; guint8 cid; } Ctx;
static GMainLoop  *loop;
static MbimDevice *g_dev = NULL;
static int exit_code = 1;

/* single termination point: cleanly close the shared MBIM device (so we never
 * disrupt ModemManager's session on the proxy), then stop the loop. */
static void finish(void)
{
    if (g_dev) {
        g_autoptr(GError) e = NULL;
        mbim_device_close_force(g_dev, &e);
    }
    g_main_loop_quit(loop);
}

static void hexdump(const char *label, const guint8 *b, guint32 n)
{
    GString *h = g_string_new(NULL);
    for (guint32 i = 0; i < n; i++) g_string_append_printf(h, "%02x", b[i]);
    g_print("%s (%u): %s\n", label, n, h->str);
    g_string_free(h, TRUE);
}

static gboolean send_qmux(MbimDevice *dev, GByteArray *frame,
                          GAsyncReadyCallback cb, gpointer u)
{
    g_autoptr(GError) e = NULL;
    MbimMessage *req = mbim_message_qmi_msg_set_new(frame->len, frame->data, &e);
    g_byte_array_unref(frame);
    if (!req) { g_printerr("qmi_msg_set_new failed: %s\n", e->message); return FALSE; }
    mbim_device_command(dev, req, 10, NULL, cb, u);
    mbim_message_unref(req);
    return TRUE;
}

/* best-effort CID release; outcome does not change exit_code */
static void release_ready(MbimDevice *dev, GAsyncResult *res, gpointer u)
{
    (void)u;
    g_autoptr(GError) e = NULL;
    g_autoptr(MbimMessage) resp = mbim_device_command_finish(dev, res, &e);
    (void)resp;
    finish();
}

static void unlock_ready(MbimDevice *dev, GAsyncResult *res, gpointer u)
{
    Ctx *c = u;
    g_autoptr(GError) e = NULL;
    g_autoptr(MbimMessage) resp = mbim_device_command_finish(dev, res, &e);
    guint32 qn = 0; const guint8 *qb = NULL;
    if (!resp || !mbim_message_qmi_msg_response_parse(resp, &qn, &qb, &e)) {
        g_printerr("unlock send failed: %s\n", e ? e->message : "?");
        finish(); return;
    }
    guint16 result = 0xffff, err = 0xffff;
    if (parse_fox_result(qb, qn, &result, &err)) {
        if (result == 0) { g_print("FCC unlock: SUCCESS\n"); exit_code = 0; }
        else g_print("FCC unlock: FAILED (qmi result=%u error=%u)\n", result, err);
    } else {
        hexdump("no result TLV; raw qmux", qb, qn);
    }
    /* release the client id we allocated, then close+quit */
    if (!send_qmux(dev, build_ctl_release(c->service, c->cid),
                   (GAsyncReadyCallback)release_ready, c))
        finish();
}

static void alloc_ready(MbimDevice *dev, GAsyncResult *res, gpointer u)
{
    Ctx *c = u;
    g_autoptr(GError) e = NULL;
    g_autoptr(MbimMessage) resp = mbim_device_command_finish(dev, res, &e);
    guint32 qn = 0; const guint8 *qb = NULL;
    if (!resp || !mbim_message_qmi_msg_response_parse(resp, &qn, &qb, &e)) {
        g_printerr("CTL alloc failed: %s\n", e ? e->message : "?");
        finish(); return;
    }
    guint8 cid = 0;
    if (!parse_ctl_cid(qb, qn, &cid)) {
        hexdump("no CID in CTL response; raw qmux", qb, qn);
        finish(); return;
    }
    g_print("allocated cid=%u on service 0x%02x\n", cid, c->service);
    c->cid = cid;
    if (!send_qmux(dev, build_unlock(c->service, cid, c->auth, c->flag),
                   (GAsyncReadyCallback)unlock_ready, c))
        finish();
}

static void open_ready(MbimDevice *dev, GAsyncResult *res, gpointer u)
{
    Ctx *c = u;
    g_autoptr(GError) e = NULL;
    if (!mbim_device_open_finish(dev, res, &e)) {
        g_printerr("open failed: %s\n", e->message);
        finish(); return;
    }
    if (!send_qmux(dev, build_ctl_alloc(c->service),
                   (GAsyncReadyCallback)alloc_ready, c))
        finish();
}

static void device_new_ready(GObject *src, GAsyncResult *res, gpointer u)
{
    (void)src;
    Ctx *c = u;
    g_autoptr(GError) e = NULL;
    g_dev = mbim_device_new_finish(res, &e);
    if (!g_dev) {
        g_printerr("device_new failed: %s\n", e->message);
        g_main_loop_quit(loop); return;
    }
    mbim_device_open_full(g_dev, MBIM_DEVICE_OPEN_FLAGS_PROXY, 10, NULL,
                          (GAsyncReadyCallback)open_ready, c);
}

int main(int argc, char **argv)
{
    GOptionContext *ctx = g_option_context_new("- Foxconn T99W696 FCC unlock");
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_autoptr(GError) error = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &error)) {
        g_printerr("option error: %s\n", error->message); return 1;
    }
    if (!opt_device) opt_device = g_strdup("/dev/wwan0mbim0");
    guint8 flag = opt_lock ? '1' : '0';

    /* arm the watchdog before anything that can block (qmicli reads, MBIM I/O) */
    if (!opt_emit_only) { signal(SIGALRM, on_alarm); alarm(WATCHDOG_SECS); }

    if (!opt_emit_only)
        autoread(opt_device, &opt_fw_mcfg, &opt_fw_apps, &opt_imei);

    if (!opt_fw_mcfg || !opt_fw_apps || !opt_imei) {
        if (opt_emit_only) {
            if (!opt_fw_mcfg) opt_fw_mcfg = g_strdup("FW?");
            if (!opt_fw_apps) opt_fw_apps = g_strdup("APP?");
            if (!opt_imei)    opt_imei    = g_strdup("000000000000000");
        } else {
            g_printerr("could not auto-read fw/imei from %s.\n"
                       "check: qmicli -d %s -p --dms-get-ids\n"
                       "or pass --fw-mcfg/--fw-apps/--imei manually.\n",
                       opt_device, opt_device);
            return 2;
        }
    }
    g_print("fw_mcfg=%s fw_apps=%s imei=%s\n", opt_fw_mcfg, opt_fw_apps, opt_imei);

    char *salt = make_salt(opt_salt_len);
    char *auth = compute_auth(opt_fw_mcfg, opt_fw_apps, opt_imei, salt);
    g_print("salt=%s auth=%s flag=%c service=0x%02x\n",
            salt, auth, (char)flag, opt_service);

    if (opt_emit_only) {
        GByteArray *ctl = build_ctl_alloc(opt_service);
        hexdump("CTL alloc frame", ctl->data, ctl->len);
        g_byte_array_unref(ctl);
        GByteArray *un = build_unlock(opt_service, 0x22, auth, flag);
        hexdump("unlock frame (cid=0x22 placeholder)", un->data, un->len);
        g_byte_array_unref(un);
        g_free(salt); g_free(auth);
        return 0;
    }

    Ctx c = { .auth = auth, .service = (guint8)opt_service, .flag = flag };
    loop = g_main_loop_new(NULL, FALSE);
    g_autoptr(GFile) file = g_file_new_for_path(opt_device);
    mbim_device_new(file, NULL, (GAsyncReadyCallback)device_new_ready, &c);
    g_main_loop_run(loop);

    g_free(salt);
    g_free(auth);
    g_main_loop_unref(loop);
    return exit_code;
}
