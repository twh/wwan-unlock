"""Minimal PE x64 analysis: .pdata bounds, reachability walk, string/import notes."""
import re, struct, sys
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RIP


class PE:
    def __init__(self, path):
        self.pe = pefile.PE(path)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.img = self.pe.get_memory_mapped_image()
        self.imports = {}
        for d in getattr(self.pe, 'DIRECTORY_ENTRY_IMPORT', []):
            for i in d.imports:
                n = i.name.decode() if i.name else f'ord{i.ordinal}'
                self.imports[i.address] = f'{d.dll.decode()}!{n}'
        self.funcs = []
        for s in self.pe.sections:
            if s.Name.rstrip(b'\0') == b'.pdata':
                d = s.get_data()
                for o in range(0, len(d) - 11, 12):
                    b, e, u = struct.unpack_from('<III', d, o)
                    if b:
                        self.funcs.append((self.base + b, self.base + e, u))
        self.funcs.sort()
        self.md = Cs(CS_ARCH_X86, CS_MODE_64)
        self.md.detail = True

    def rd(self, va, n):
        o = va - self.base
        return self.img[o:o + n] if 0 <= o < len(self.img) else b''

    def string(self, va):
        b = self.rd(va, 400)
        m = re.match(rb'([\x20-\x7e\t\r\n]{3,})\x00', b)
        if m:
            return repr(m.group(1).decode())
        m = re.match(rb'((?:[\x20-\x7e\t\r\n]\x00){3,})\x00\x00', b)
        if m:
            return 'L' + repr(m.group(1).decode('utf-16le'))
        return None

    def find_bytes(self, needle):
        out, i = [], 0
        while True:
            i = self.img.find(needle, i)
            if i < 0:
                return out
            out.append(self.base + i)
            i += 1

    def sweep(self):
        """Resilient linear sweep of .text: restart one byte past bad data."""
        if getattr(self, '_sweep', None) is not None:
            return self._sweep
        out = []
        for s in self.pe.sections:
            if s.Name.rstrip(b'\0') != b'.text':
                continue
            start = self.base + s.VirtualAddress
            size = s.Misc_VirtualSize
            data = self.rd(start, size)
            off = 0
            while off < size:
                got = False
                for ins in self.md.disasm(data[off:], start + off):
                    got = True
                    out.append(ins)
                    off = ins.address - start + ins.size
                if not got:
                    off += 1
        self._sweep = out
        return out

    def xrefs_to(self, *vas):
        """Instructions whose rip-relative operand resolves to any given va."""
        want = set(vas)
        hits = {v: [] for v in want}
        for ins in self.sweep():
            for op in ins.operands:
                if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                    t = ins.address + ins.size + op.mem.disp
                    if t in want:
                        hits[t].append(ins.address)
        return hits

    def func_of(self, va):
        for b, e, u in self.funcs:
            if b <= va < e:
                return b, e
        return None

    def note(self, ins):
        out = []
        for op in ins.operands:
            if op.type == X86_OP_MEM and op.mem.base == X86_REG_RIP:
                t = ins.address + ins.size + op.mem.disp
                if t in self.imports:
                    out.append(self.imports[t])
                else:
                    s = self.string(t)
                    out.append(s if s else f'[{t:#x}]')
        return '  ; ' + ' '.join(out) if out else ''

    def walk(self, entry):
        seen, work, calls = {}, [entry], []
        while work:
            va = work.pop()
            while va not in seen:
                ins = next(self.md.disasm(self.rd(va, 16), va), None)
                if ins is None:
                    break
                seen[va] = ins
                m = ins.mnemonic
                if m == 'call' and ins.operands[0].type == X86_OP_IMM:
                    calls.append((va, ins.operands[0].imm))
                if m.startswith('j') and ins.operands[0].type == X86_OP_IMM:
                    work.append(ins.operands[0].imm)
                    if m == 'jmp':
                        break
                if m in ('ret', 'retf', 'int3') or (m == 'jmp' and ins.operands[0].type != X86_OP_IMM):
                    break
                va += ins.size
        return seen, calls

    def show(self, entry, names=None, filt=None):
        names = names or {}
        seen, calls = self.walk(entry)
        for va in sorted(seen):
            ins = seen[va]
            txt = f'{va:x}: {ins.mnemonic:<7} {ins.op_str}'
            if ins.mnemonic == 'call' and ins.operands[0].type == X86_OP_IMM:
                t = ins.operands[0].imm
                txt += f'  ; -> {names.get(t, hex(t))}'
            txt += self.note(ins)
            if filt is None or filt(txt):
                print(txt)
        return seen, calls


if __name__ == '__main__':
    p = PE(sys.argv[1])
    p.show(int(sys.argv[2], 16))
