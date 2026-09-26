# SMBIOS type 133: two records, only one holds the model id

Every AT and MBIM unlock in this project hashes a 14 character "model id in
bios". Where that id lives was established from HP's own firmware tool, which is
the only vendor binary seen so far that parses the SMBIOS records directly and
names them.

## Source

`WWANFirmwareFlash.dll`, inside `M2_7560_NAND.flz` in HP's L860R+ SoftPaqs
sp151712 and sp155280. The `.flz` files are ZIP archives; this one contains
`860Download.zip` with the flashing tools and `L860_Secureboot.zip` with the
firmware and HP's device packs.

Its record walker at `0x18000d790` scans the SMBIOS table and dispatches on the
record type **and its formatted length**:

```
cmp r8d, 2 ; cmp byte [rcx], 0x85 ; cmp byte [rcx+1], 0x2c  -> SMBIOS::ProcWWANConfigIDType
cmp r8d, 1 ; cmp byte [rcx], 0x85 ; cmp byte [rcx+1], 5     -> SMBIOS::ProcFCCType
             cmp byte [rcx], 0x7f ; cmp byte [rcx+1], 4     -> end of table
```

So HP's tooling expects **two distinct type 133 (0x85) records**:

| Record | Formatted length | Handler | Contents |
|---|---|---|---|
| FCC type | **5** | `SMBIOS::ProcFCCType` (`0x18000da10`) | the model id, as a string in the record's string table |
| WWAN config id | **44** (`0x2c`) | `SMBIOS::ProcWWANConfigIDType` (`0x18000dbc0`) | a GUID it verifies, then `WwanModelId.DeviceModel`, `.SizeModel`, `.PinModel`, `.Reserved`, each rejected above 0x0f, plus a product id and a reserved field. No model id string |

`ProcFCCType` copies **exactly 14 bytes** out of the string table
(`cmp rax, 0xe` at `0x18000daef`) into a global. That is the same 14 byte length
Lenovo's `compute_sha256` hashes and the same length as every model id known:
`KHOIHGIUCCHHII`, `DW5823EFCCLOCK`, `DW5931EFCCLOCK`, `DW5933EFCCLOCK`,
`WNCHPCEFCCLOCK`.

## Why this matters for a dispatcher

A machine can carry both records, and the order is not fixed. Reading
`/sys/firmware/dmi/entries/133-0` alone therefore gets the model id on some
machines and nothing on others. Demonstrated with the two records above:

```
old reader, 133-0 only, 44 byte record first : []
new reader, walks 133-*, first 14 char string: [KHOIHGIUCCHHII]
```

The report in
https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/work_items/816
fits this exactly: the Dell Latitude 5540 was found to have a 44 byte type 133
record with no string table, which is the WWAN config id record, while the
machine that worked had a small record holding the string.

So the rule for reading the model id is:

1. Walk every `/sys/firmware/dmi/entries/133-*` entry, not just `133-0`.
2. Skip the formatted area using that entry's own `length`.
3. Take the first string that is 14 characters, which is what `ProcFCCType`
   copies.

`modules/8086:7560/fcc-unlock.sh` and the `8086:7560` dispatcher in
https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1496
now do this.
https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/merge_requests/1491
reads `133-0` only and would benefit from the same change.

## What it says about HP

HP firmware is expected to publish the model id, because HP's own tool looks for
it by that exact shape. That is why HP ships no key in its driver packages: on
this design the key belongs to the machine's firmware, not to the driver. It
also means the SMBIOS derived candidate a dispatcher computes is the right
mechanism for an HP machine, and is the reason to try it before any built in
constant. See [8086-7560-hp.md](8086-7560-hp.md).
