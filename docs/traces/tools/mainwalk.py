"""Walk DPR_Fcc_unlock_service main() for one module code.

From each MACHINE jump-table case, follow the control flow with the module
code (-0xc(%rbp)) fixed; every other conditional branch is followed both
ways. Record every call reached and the immediate arguments set up for it.
"""
import struct, sys
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
from capstone.x86 import X86_OP_IMM, X86_OP_MEM, X86_REG_RBP, X86_REG_RIP
from elftools.elf.elffile import ELFFile
import elfdis

PATH = sys.argv[2] if len(sys.argv) > 2 else 'DPR_Fcc_unlock_service'
MODULE = int(sys.argv[1]) if len(sys.argv) > 1 else 5
MAIN_LO, MAIN_HI = 0x3be5, 0x532d
EXIT = 0x532b

e = ELFFile(open(PATH, 'rb'))
text = e.get_section_by_name('.text')
TA, TD = text['sh_addr'], text.data()
ro = e.get_section_by_name('.rodata')
RA, RD = ro['sh_addr'], ro.data()
plt, strs, syms, got = elfdis.load(PATH)
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True


def ins_at(a):
    return next(md.disasm(TD[a - TA:a - TA + 16], a))


def name(t):
    return plt.get(t) or syms.get(t) or hex(t)


def evaluate(cc, lhs, rhs):
    s = lhs - rhs
    return {'je': s == 0, 'jne': s != 0, 'jg': s > 0, 'jge': s >= 0, 'jl': s < 0,
            'jle': s <= 0, 'ja': (lhs & 0xffffffff) > (rhs & 0xffffffff),
            'jbe': (lhs & 0xffffffff) <= (rhs & 0xffffffff),
            'jb': (lhs & 0xffffffff) < (rhs & 0xffffffff),
            'jae': (lhs & 0xffffffff) >= (rhs & 0xffffffff),
            'js': s < 0, 'jns': s >= 0}[cc]


def is_module_mem(op):
    return op.type == X86_OP_MEM and op.mem.base == X86_REG_RBP and op.mem.disp == -0xc


def walk(start):
    calls, seen, work = [], set(), [(start, {})]
    while work:
        a, regs = work.pop()
        pending = None  # (lhs, rhs) of a compare on the module code
        while MAIN_LO <= a < MAIN_HI and a not in seen:
            seen.add(a)
            i = ins_at(a)
            m, ops = i.mnemonic, i.operands
            if m == 'mov' and len(ops) == 2 and is_module_mem(ops[1]) and i.reg_name(ops[0].reg) == 'eax':
                regs = dict(regs, eax='module')
            elif m == 'mov' and len(ops) == 2 and ops[0].type != X86_OP_MEM and ops[1].type == X86_OP_IMM:
                regs = dict(regs, **{i.reg_name(ops[0].reg): ops[1].imm})
            elif m == 'lea' and ops[1].type == X86_OP_MEM and ops[1].mem.base == X86_REG_RIP:
                t = a + i.size + ops[1].mem.disp
                regs = dict(regs, **{i.reg_name(ops[0].reg): strs.get(t, hex(t))})
            if m == 'cmp':
                if is_module_mem(ops[0]) and ops[1].type == X86_OP_IMM:
                    pending = (MODULE, ops[1].imm)
                elif ops[0].type != X86_OP_MEM and regs.get(i.reg_name(ops[0].reg)) == 'module' and ops[1].type == X86_OP_IMM:
                    pending = (MODULE, ops[1].imm)
                else:
                    pending = None
            elif m == 'call':
                t = ops[0].imm if ops[0].type == X86_OP_IMM else None
                calls.append((a, name(t) if t else i.op_str, regs.get('edi'), regs.get('esi'), regs.get('rdi')))
                regs = {}
                pending = None
            elif m.startswith('j') and m != 'jmp':
                tgt = ops[0].imm
                if pending is not None:
                    taken = evaluate(m, *pending)
                    a = tgt if taken else a + i.size
                    pending = None
                    continue
                work.append((tgt, dict(regs)))
            elif m == 'jmp':
                if ops[0].type == X86_OP_IMM:
                    a = ops[0].imm
                    continue
                break
            elif m in ('ret', 'leave'):
                break
            elif m not in ('mov', 'lea', 'test', 'nop', 'endbr64'):
                pending = None if m != 'movzx' else pending
            a += i.size
    return calls


table = 0x7d90
cases = {}
for k in range(0x4a):
    off = struct.unpack_from('<i', RD, table - RA + 4 * k)[0]
    cases.setdefault(table + off & 0xffffffffffffffff, []).append(k + 8)
cases.setdefault(0x52e2, []).append('default')

INTEREST = ('fccunlock', 'setFccUnlock', 'checkSAR', 'usbdevice', 'syslog')
for tgt, machines in sorted(cases.items()):
    calls = walk(tgt)
    unlock = [c for c in calls if any(s in c[1] for s in INTEREST[:4])]
    msgs = sorted({c[3] for c in calls if c[1] == 'syslog' and isinstance(c[3], str)})
    print(f'case {tgt:#x} MACHINE {machines}')
    for a, n, edi, esi, rdi in unlock:
        print(f'    {a:#x} call {n}  edi={edi} esi={esi} rdi={rdi}')
    for s in msgs:
        print(f'    log: {s!r}')
