# `105b:e0f5` / `105b:e0f9`: Dell DW5932e, from `SIMService.exe`

Traced 2026-10-25 from Dell's own Windows driver, to confirm the unlock already
shipping as [ModemManager!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500)
and to settle how the version string is read. The DW5932e is Foxconn's T99W373
(Snapdragon X62); the same silicon reaches [`17cb:0308`](17cb-0308-foxconn.md)
when its customer id is cleared.

## Package

`Dell-Wireless-5932e-and-Qualcomm-Snapdragon-X62-Firmware_GJ2XY_WIN64_3.2.0.22_A09.EXE`,
sha256 `86d47d8846ff4beaa6014f58910940132b7dfdcd57801a2f9ce9e4177dec530e`. A Dell
Update Package: a 9.9 MB `DUPFramework.exe` with a 506 MB 7z overlay at offset
`0x972e00`. The unlock lives in `SIMService.exe`, sha256
`ed6141ef88cd…` (identical in the `17134` and `22000` driver trees). `fxcrypt.dll`
beside it is AES FOTA-package decryption only, not the FCC hash.

This is a newer package than SySS read in September 2025 (`3.2.0.22`, A09). The
unlock is unchanged, so the method is stable across firmware revisions.

## `CServiceMain::BiosUnlock`, `0x140019069`

Static disassembly only; the binary was never run. All addresses are in
`SIMService.exe` above.

| step | address | what the code does |
|---|---|---|
| IMEI | `0x14001944x` | `GetIMEINumber`; aborts on `IMEI is null!` |
| version | `GetToEncryptFWVersionNumber` `0x140019720` | below |
| salt | `0x140019598` | four characters, each `alphabet[rand() % 46]` (the `imul …, 0x2e`) |
| magic | `0x140019074`–`0x1400190ce` | `sprintf(buf, "%c%c%c%c", 0x46, 0x44, 0x45, 0x31)` → `FDE1`. Built character by character, which is why `strings` finds no `FDE1` |
| hash input | `0x1400195f6` | `sprintf(buf, "%s%s%s%s", version, IMEI, salt, magic)` |
| md5 | `0x140019aa0` | MD5: initialises `0x67452301 0xefcdab89 0x98badcfe 0x10325476`, emits lower case hex through `'0123456789abcdef'` |
| payload | `0x140019632` | `sprintf(buf, "%s%s", salt, md5hex)`, 4 + 32 = 36 characters |
| send | `GobiDLL::SetRadioFlagNew` `0x140009a70` | FOX service `0xE3`; logs `FOXSetRadioFlag success.` / `fail.`. TLV `0x02` is one byte `0x30`, the character `'0'`. Sent once, with no `'1'` retry |

### The version number, `GetToEncryptFWVersionNumber` `0x140019720`

The module firmware version comes from `GetModuleFWVersion`, which issues DMS Get
Device Revision and logs `DMSGetDeviceRevision in module: %s`. The revision is
split on `.` by the helper at `0x140041a28`, called once per field. The function
reads **version1 through version6** and requires each to be non-NULL (the
`versionN is NULL` log branches), then builds:

```
sprintf(out, "%s%s%s%s%s%s%s%s%s%s%s%03d",
        v1, ".", v2, ".", v3, ".", v4, ".", v5, ".", v6, n)
```

the first six fields rejoined with `.`, then an integer `n` as three digits. `n`
is loaded at `0x140019781` from `[rbx+0x300]`, a field the version parser filled
from the ninth dot-separated token. On the real revision `FDE.F0.3.0.1.1.DT.005.074`
from [issue 1100](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/1100)
this yields `FDE.F0.3.0.1.1074`.

## This confirms !1500, and the magic and service from the binary

Everything [!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500)
transcribed is present here: the DMS-revision version with six fields plus the
three-digit suffix, the `FDE1` magic, the four-part md5 input, the salt-plus-hex
payload, the FOX service, and the single `0x30` TLV sent once. The magic and the
FOX service were previously taken from SySS's write-up; they are now read
straight from the code.

## The two version readings produce the same hash

[!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
reads the version a different way for the same module: the FOX firmware-version
query, mcfg minus its last two fields, then the FOX apps version, passed as two
separate `%s`. Dell passes one combined `%s`. They concatenate to the same md5
input:

```
Dell (DMS revision):  "FDE.F0.3.0.1.1" + "074"   as one string
SySS/!1492 (FOX):     "FDE.F0.3.0.1.1" + "074"   as two strings
md5 input begins:     FDE.F0.3.0.1.1074<IMEI><salt>FDE1   either way
```

so the two paths compute the same response.

**One caveat on the FOX reading.** Dell forces the suffix to three digits with
`%03d`. The FOX apps query in [!1492](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1492)
uses whatever the modem returns, unpadded. They agree only while the apps version
is already three digits, which it is in every sample seen (`074`, `068`). A module
reporting a two-digit apps version would hash differently on the FOX path than
Dell does. Reading the version from the DMS revision, as `105b:e0f5` does, avoids
the question, since the `%03d` is reproduced in the script.

## Sources

- Dell DUP `3.2.0.22_A09`, carved locally; not redistributed here.
- SySS, Foxconn FCC Unlock (Jan Wütherich, 2025-09-15): https://blog.syss.com/posts/foxconn-fcc-unlock/
- [ModemManager!1500](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1500) and [issue 1100](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/1100)
- [17cb-0308-foxconn.md](17cb-0308-foxconn.md) for how a T99W373 reaches Qualcomm's own id
