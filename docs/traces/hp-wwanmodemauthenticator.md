# HP `WWANModemAuthenticator.exe`, full unlock trace

The only FCC unlock host tool HP ships in any of the seven WWAN packages
examined. It arrives in sp173756, the WNC M80 package, installed by
`WwanNetM80.inf` as an auto-start service:

```
AddService=%IntelMAService.Name%, %SPSVCSINST_STARTSERVICE%, WwanMA_Service_Inst
ServiceBinary=%11%\WWANModemAuthenticator.exe
IntelMAService.Name="WNC WWAN Modem Authenticator"
```

| Item | Value |
|---|---|
| SoftPaq | sp173756, `src/drivers/netadapter/` |
| sha256 | `ef1e4b025387676f14aa7f7426134c89f902df3eee48764b57f8b0b7e497dbf9` |
| Ids in that INF | `PCI\VEN_03F0&DEV_09C8&SUBSYS_8E6D103C`, `…&SUBSYS_8E6F103C`, and `PCI\VEN_14C3&DEV_4D75` |
| Devices it opens | `PCI#VEN_03F0&DEV_09C8`, then `PCI#VEN_03F0&DEV_0800` |

It is one binary serving more than one OEM: it carries `CFccLock`,
`CHpFccLock` and `CDellFccLock`, picks between them at run time, and logs
`detected vender platform: %d by RTTI`.

## OEM selection, `CFccLock::GetLockPtrInstance` (`0x1400024b0`)

Reads `HKLM\HARDWARE\DESCRIPTION\System\BIOS\SystemManufacturer`, lowercases
it, and compares against two wide strings at `0x1400923b8` and `0x1400923c4`:

| Manufacturer | Class | Model id getter | Model id | sha256[0:4] |
|---|---|---|---|---|
| `dell` | `CDellFccLock` | `0x140002490` | `DW5931EFCCLOCK` | `4909b5a4` |
| `hp` | `CHpFccLock` | `0x1400024a0` | `WNCHPCEFCCLOCK` | `576f0ae0` |

Each getter is a one instruction function, `lea rax, <string>; ret`, sitting in
slot 3 of its class vtable (`0x140092348` for Dell, `0x140092318` for HP). The
class each vtable belongs to was read from its RTTI complete object locator, so
the pairing above is not inferred from the order of the strings.

## Gate (`0x140002260`)

Calls two virtual methods on the selected object and requires both, then logs
`All check passed.`:

- `CheckSkuRegistry`, which for HP logs
  `Sku registry method not implement, always returns true`. HP's version does
  fetch SMBIOS, looks for the manufacturer, and parses the panel size
  (`CHpFccLock::ParserInch`), but the check itself passes.
- `CheckModuleHwid`, `CDellFccLock::CheckModuleHwid` for Dell.

If either fails, `DoFccUnlock` logs `not in whitelist, abort FCC unlock` and
stops. `WriteFccWhitelistStatus` (`0x140024930`) records the outcome.

## `DoFccUnlock` (`0x140021512`)

1. Find the MBIM interface for `PCI#VEN_03F0&DEV_09C8`, else
   `PCI#VEN_03F0&DEV_0800`. On failure log
   `MBIM interface is not available, retry: %d`, `Sleep`, up to 10 tries, then
   `Reach the max retry times`.
2. `PollingModemObject` up to 5 times with `Sleep`.
3. Detect the OEM by RTTI, run the gate above, then log `set fcc code to: %s`.
4. **Query.** `Query_INTEL_FCC_MBIM_Extension` on service
   `{F85D46EF-AB26-4081-9868-4D183C0A3AEC}`, the Intel Mutual Authentication
   service. Logs `lm: %d, ls: %d`, the lock mode and lock state, and proceeds
   only when the mode is nonzero and the state is zero; otherwise `No need`.
5. **Challenge.** `Set_INTEL_FCC_MBIM_Extension` (`0x1400234d0`). On failure
   `Get challenge value from Modem FAILED!`.
6. **Hash**, with `0x1400418b0`, which is SHA-256 (it loads `0x6a09e667`,
   `0xbb67ae85`, `0x3c6ef372`, `0x5be0cd19`):
   - `SHA-256(model id, 14 bytes)`, digest kept at `0x1400bc0c0`.
   - Build 8 bytes: reply bytes `+0x21c4` to `+0x21c7`, which are bytes 4 to 7
     of the challenge reply, then digest bytes `0x1400bc0c0[0..3]`.
   - `SHA-256` of those 8 bytes.
7. **Response.** Payload: dword `1` at offset 0, then the new digest's first 4
   bytes at offsets 4 to 7. `Set_INTEL_FCC_MBIM_Extension` again. Failure logs
   `Send finial hash value to Modem FAILED!`, success logs `DO result: %d`.

This is the same exchange, field for field, as Dell's `ModemAuthenticator.exe`
for the DW5823e, recorded in
[8086-7560-dell-dw5823e.md](8086-7560-dell-dw5823e.md). Only the model id and
the device interface differ.

## What it settles

- **`03f0:09c8`**: HP's model id is `WNCHPCEFCCLOCK`, `576f0ae0`, now tied to
  the HP class through the vtable and its RTTI, not just by being present in
  the binary. That is the value in
  https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1501.
- **`14c3:4d75`**: `DW5931EFCCLOCK`, `4909b5a4`, is what this tool hashes on a
  Dell machine. Until now that value rested on one hardware report in
  https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/work_items/816;
  it now also has a binary source, in a tool whose INF binds `14c3:4d75`.
- The same INF binding means that on an **HP** machine this tool would hash
  `WNCHPCEFCCLOCK` for a `14c3:4d75` module. HP's own FM350 package, sp174156,
  ships no unlock of its own, so if an HP FM350 is FCC locked, `576f0ae0` over
  the Intel MBIM service is the candidate to try. Untested on that hardware.

## The other mechanisms in these packages

- `WinIhvRil.dll` (all Intel packages) is the driver side of the same service:
  it maps MBIM onto the modem's CSI calls and holds no key. Its value is as an
  independent description of the protocol, in [8086-7560-hp.md](8086-7560-hp.md).
- `MIPC_SYS_SET_FCC_LOCK_REQ` and `_CNF` in `libWncT.dll` and `libmipc.dll`
  (sp173756) are two entries in MediaTek's MIPC command set, the modem facing
  leg on that hardware, alongside `MIPC_SYS_AUTH_REQ`. The host still speaks the
  Intel MBIM service, so nothing here changes what a dispatcher sends.
- `WwanConfigurator.dll` (sp173756 and sp174156) carries the service GUID only
  because it builds the MBIM device services table; it computes no response.
