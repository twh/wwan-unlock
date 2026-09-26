import re, subprocess, sys
from elftools.elf.elffile import ELFFile


def load(path):
    e = ELFFile(open(path, 'rb'))
    plt = {}
    rela = e.get_section_by_name('.rela.plt'); dyn = e.get_section_by_name('.dynsym')
    sec = e.get_section_by_name('.plt.sec')
    if rela and sec:
        base = sec['sh_addr']
        for i, r in enumerate(rela.iter_relocations()):
            plt[base + i * 16] = dyn.get_symbol(r['r_info_sym']).name
    strs = {}
    for sn in ('.rodata', '.data'):
        s = e.get_section_by_name(sn)
        if not s or s['sh_type'] == 'SHT_NOBITS':
            continue
        a = s['sh_addr']; d = s.data()
        for m in re.finditer(rb'[\x20-\x7e\t\n]{2,}\x00', d):
            strs[a + m.start()] = m.group()[:-1].decode()
    syms = {}
    st = e.get_section_by_name('.symtab') or dyn
    for y in st.iter_symbols():
        if y['st_value']:
            syms.setdefault(y['st_value'], y.name)
    got = {}
    for s in e.iter_sections():
        if s['sh_type'] == 'SHT_RELA':
            for r in s.iter_relocations():
                if r['r_info_sym']:
                    got[r['r_offset']] = dyn.get_symbol(r['r_info_sym']).name
    return plt, strs, syms, got


def dis(path, lo, hi, filt=None):
    plt, strs, syms, got = load(path)
    out = subprocess.run(['objdump', '-d', '--no-show-raw-insn', f'--start-address={hex(lo)}',
                          f'--stop-address={hex(hi)}', path],
                         capture_output=True, text=True).stdout
    lines = []
    for line in out.splitlines():
        m = re.match(r'\s+([0-9a-f]+):\s+(\S+)\s*(.*)', line)
        if not m:
            continue
        a = int(m.group(1), 16); mn = m.group(2); ops = m.group(3)
        note = ''
        r = re.search(r'#\s*0x([0-9a-f]+)', ops)
        if r:
            t = int(r.group(1), 16)
            if t in strs: note = f'   -> {strs[t]!r}'
            elif t in got: note = f'   -> GOT<{got[t]}>'
            elif t in syms: note = f'   -> <{syms[t]}>'
        if mn.startswith('call') or mn.startswith('j'):
            c = re.match(r'(0x[0-9a-f]+)', ops)
            if c:
                t = int(c.group(1), 16)
                if t in plt: note = f'   <{plt[t]}>'
                elif t in syms: note = f'   <{syms[t]}>'
        lines.append((a, mn, ops, note))
        if filt is None or filt(mn, ops, note):
            print(f'  {a:x}: {mn:<7} {ops[:52]}{note}')
    return lines


if __name__ == '__main__':
    dis(sys.argv[1], int(sys.argv[2], 16), int(sys.argv[3], 16))
