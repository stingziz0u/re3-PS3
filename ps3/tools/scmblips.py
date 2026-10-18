#!/usr/bin/env python3
# Makes src/core/ps3_scmblips.h (unique jumps, Vice City's store robberies
# on the map) from an original main.scm:
#   python3 ps3/tools/scmblips.py <main.scm> iii|vc
# prints the tables that go after the two structs of src/core/ps3_scmblips.h.
# It disassembles main.scm with the command table of src/control (argument
# counts fixed where the table is wrong), then finds in the USJ script each
# jump's take-off and "done" global, and the ADD_STORES_KNOCKED_OFF blocks.
import re, struct, sys, os

def load_table(src_dir):
    enum = open(f'{src_dir}/src/control/ScriptCommands.h').read()
    names = {}
    n = 0
    for m in re.finditer(r'^\s*(COMMAND_\w+)\s*(?:=\s*(\d+))?\s*,', enum, re.M):
        if m.group(2):
            n = int(m.group(2))
        names[m.group(1)] = n
        n += 1
    tab = {}
    src = open(f'{src_dir}/src/control/Script.cpp').read()
    for m in re.finditer(r'REGISTER_COMMAND\((COMMAND_\w+),\s*INPUT_ARGUMENTS\(([^)]*)\),\s*OUTPUT_ARGUMENTS\(([^)]*)\)', src):
        name = m.group(1)
        ins = [a.strip() for a in m.group(2).split(',') if a.strip()]
        outs = [a.strip() for a in m.group(3).split(',') if a.strip()]
        if name in names:
            tab[names[name]] = (name[8:], ins + outs)
    return tab

class Disasm:
    def __init__(self, data, tab, vc):
        self.d = data; self.tab = tab; self.vc = vc

    def arg(self, ip):
        t = self.d[ip]; ip += 1
        if t == 0: return ip, ('end', None)
        if t == 1: return ip+4, ('i', struct.unpack_from('<i', self.d, ip)[0])
        if t == 2: return ip+2, ('g', struct.unpack_from('<H', self.d, ip)[0])
        if t == 3: return ip+2, ('l', struct.unpack_from('<H', self.d, ip)[0])
        if t == 4: return ip+1, ('i', struct.unpack_from('<b', self.d, ip)[0])
        if t == 5: return ip+2, ('i', struct.unpack_from('<h', self.d, ip)[0])
        if t == 6:
            if self.vc: return ip+4, ('f', struct.unpack_from('<f', self.d, ip)[0])
            return ip+2, ('f', struct.unpack_from('<h', self.d, ip)[0] / 16.0)
        raise ValueError(f'bad arg type {t} at {ip-1}')

    def insn(self, ip):
        start = ip
        raw = struct.unpack_from('<H', self.d, ip)[0]; ip += 2
        op = raw & 0x7fff; neg = bool(raw & 0x8000)
        if op not in self.tab:
            raise ValueError(f'unknown opcode {op:04x} at {start}')
        name, types = self.tab[op]
        args = []
        for t in types:
            if t == 'ARGTYPE_STRING':
                s = self.d[ip:ip+8].split(b'\0')[0].decode('latin1'); ip += 8
                args.append(('s', s))
            else:
                ip, a = self.arg(ip)
                args.append(a)
        if name == 'START_NEW_SCRIPT':
            while True:
                ip, a = self.arg(ip)
                if a[0] == 'end': break
                args.append(a)
        return ip, (start, op, neg, name, args)

def fmt(a):
    k, v = a
    if k == 'g': return f'$g{v}'
    if k == 'l': return f'{v}@'
    if k == 'f': return f'{v:.3f}'
    if k == 's': return f"'{v}'"
    return str(v)

def disasm_all(path, src_dir, vc):
    data = open(path, 'rb').read()
    tab = load_table(src_dir)
    d = Disasm(data, tab, vc)
    out = []
    # segment jumps: GOTO over the global space, the models, the missions
    ip = 0
    segs = []
    for _ in range(3):
        nip, ins = d.insn(ip)
        tgt = ins[4][0][1]
        segs.append((ip, tgt))
        ip = tgt
    # mission table: segment 3 data
    seg3 = segs[2][0] + 8   # GOTO (7 bytes) + segment id byte
    main_size, largest, nmiss = struct.unpack_from('<IIH', data, seg3)
    pad = 2
    offs = list(struct.unpack_from(f'<{nmiss}I', data, seg3 + 10 + pad - 2 + 0)) if False else None
    return d, data, segs, (main_size, largest, nmiss, seg3)


def disassemble(path, src, vc):
    d, data, segs, hdr = disasm_all(path, src, vc)
    
    fixed = {}
    def with_count(op, k):
        name, types = d.tab[op]
        strs = [t for t in types if t == 'ARGTYPE_STRING']
        typed = [t for t in types if t != 'ARGTYPE_STRING']
        if k <= len(typed):
            # drop typed args from the end
            nt = []; left = k
            for t in types:
                if t == 'ARGTYPE_STRING': nt.append(t)
                elif left > 0: nt.append(t); left -= 1
            return nt
        return types + ['X'] * (k - len(typed))
    orig = dict(d.tab)
    def try_decode(ip, depth):
        for _ in range(depth):
            if ip >= len(data): return True
            ip, _ = d.insn(ip)
        return True
    def decode(ip):
        raw = struct.unpack_from('<H', data, ip)[0]; op = raw & 0x7fff
        if op in fixed or op not in orig:
            return d.insn(ip)
        name, types = orig[op]
        n = len([t for t in types if t != 'ARGTYPE_STRING'])
        for k in [n, n+1, n-1, n+2, n+3, n-2, n+4, n+5, n+6]:
            if k < 0: continue
            d.tab[op] = (name, with_count(op, k))
            try:
                nip, ins = d.insn(ip)
                try_decode(nip, 12)
                fixed[op] = k
                if k != n: print(f'fix {name} {n} -> {k}', file=sys.stderr)
                return nip, ins
            except Exception:
                pass
        d.tab[op] = orig[op]
        return d.insn(ip)
    ip = segs[2][1]
    lines = []
    while ip < len(data):
        nip, (st, op, neg, name, args) = decode(ip)
        lines.append(f"{st}: {'NOT ' if neg else ''}{name} {' '.join(fmt(a) for a in args)}")
        ip = nip
    return data, lines
def load(lines):
    ins = []
    for line in lines:
        m = re.match(r'(\d+): (NOT )?(\w+) ?(.*)', line.rstrip('\n'))
        if not m: continue
        ins.append((int(m.group(1)), m.group(3), m.group(4).split() if m.group(4) else []))
    return ins
def g(a):
    return int(a[2:]) if a.startswith('$g') else None
def num(a):
    return float(a)
def extract(path, game):
    ins = load(path)
    idx = {a: i for i, (a, _, _) in enumerate(ins)}
    floats = {}
    for a, n, args in ins:
        if n == 'SET_VAR_FLOAT' and args and args[0].startswith('$g'):
            floats.setdefault(g(args[0]), []).append(float(args[1]))
    def val(x):
        if x.startswith('$g'):
            v = floats.get(g(x), [])
            assert len(v) == 1, (x, v)
            return v[0]
        return float(x)
    # --- unique jumps
    usj = [i for i, (a, n, args) in enumerate(ins) if n == 'SCRIPT_NAME' and args[0] == "'USJ'"][0]
    end = [i for i in range(usj, len(ins)) if ins[i][1] == 'REGISTER_UNIQUE_JUMP_FOUND'][0]
    flags = {}
    idvar = None
    for i in range(usj, end):
        a, n, args = ins[i]
        if (n == 'IS_INT_VAR_EQUAL_TO_NUMBER' and ins[i+1][1] == 'IS_INT_VAR_EQUAL_TO_NUMBER'
            and ins[i+1][2][1] == '0' and ins[i+2][1] == 'GOTO_IF_FALSE' and ins[i+3][1] == 'SET_VAR_INT'
            and ins[i+3][2] == [ins[i+1][2][0], '1']):
            idvar = args[0]
            flags[int(args[1])] = (g(ins[i+1][2][0]), ins[i+3][0])
    takeoffs = []
    for i in range(usj, end):
        a, n, args = ins[i]
        if n in ('LOCATE_PLAYER_IN_CAR_2D', 'LOCATE_PLAYER_IN_CAR_3D') and ins[i+1][1] == 'GOTO_IF_FALSE' \
           and ins[i+2][1] == 'SET_VAR_INT' and ins[i+2][2][0] == idvar:
            x, y = val(args[1]), val(args[2])
            z = val(args[3]) if n.endswith('3D') else None
            takeoffs.append((int(ins[i+2][2][1]), x, y, z))
    # --- store robberies (VC)
    stores = []
    for i, (a, n, args) in enumerate(ins):
        if n != 'ADD_STORES_KNOCKED_OFF': continue
        # back: GOTO_IF_FALSE, IS_INT_VAR_EQUAL_TO_NUMBER $gF 0, LOCATE/AREA
        assert ins[i-1][1] == 'GOTO_IF_FALSE' and ins[i-2][1] == 'IS_INT_VAR_EQUAL_TO_NUMBER'
        flag = g(ins[i-2][2][0])
        loc = ins[i-3]
        if loc[1] == 'LOCATE_PLAYER_ANY_MEANS_3D':
            x, y, z = val(loc[2][1]), val(loc[2][2]), val(loc[2][3])
        elif loc[1] == 'IS_PLAYER_IN_AREA_3D':
            x1, y1, z1, x2, y2, z2 = [val(v) for v in loc[2][1:7]]
            x, y, z = (x1+x2)/2, (y1+y2)/2, min(z1, z2)
        else:
            raise SystemExit(f'store at {a}: {loc}')
        setf = [j for j in range(i, i+4) if ins[j][1] == 'SET_VAR_INT' and ins[j][2] == [f'$g{flag}', '1']][0]
        stores.append((x, y, z, flag, ins[setf][0]))
    return takeoffs, flags, stores

path, game = sys.argv[1], sys.argv[2]
src = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
data, lines = disassemble(path, src, game == 'vc')
takeoffs, flags, stores = extract(lines, game)

print('jumps', len(flags), 'takeoffs', len(takeoffs), 'stores', len(stores), file=sys.stderr)
ids = sorted(flags)
assert ids == list(range(1, len(ids)+1)), ids
for t in takeoffs: assert t[0] in flags, t
missing = [i for i in ids if i not in {t[0] for t in takeoffs}]
print('jump ids without a takeoff:', missing, file=sys.stderr)

def chk(off, addr):
    exp = bytes([4, 0, 2, off & 255, off >> 8, 4, 1])
    assert data[addr:addr+7] == exp, (addr, data[addr:addr+7].hex(), exp.hex())
seen = set(); rows = []
for t in sorted(takeoffs):
    if t in seen: continue
    seen.add(t); rows.append(t)
for i in flags: chk(*flags[i])
for s in stores: chk(s[3], s[4])
pre = game
out = []
out.append(f'// generated from the original {"GTA III" if pre=="iii" else "Vice City"} main.scm (ps3/tools/scmblips.py)')
out.append(f'static const PS3ScmFlag ps3UsjFlags[] = {{\t// jump n: the global set to 1 when done, where main.scm sets it')
for i in sorted(flags):
    out.append(f'\t{{ {flags[i][0]}, {flags[i][1]} }},\t// {i}')
out.append('};')
out.append('static const PS3ScmPoint ps3UsjPoints[] = {\t// take-offs: jump number, x, y, z (-1000: no height in the script)')
for t in rows:
    z = t[3] if t[3] is not None else -1000.0
    out.append(f'\t{{ {t[0]}, {t[1]:.3f}f, {t[2]:.3f}f, {z:.3f}f }},')
out.append('};')
if stores:
    out.append('static const PS3ScmFlag ps3StoreFlags[] = {\t// store robberies: the global set to 1 when knocked off')
    for s in stores: out.append(f'\t{{ {s[3]}, {s[4]} }},')
    out.append('};')
    out.append('static const PS3ScmPoint ps3StorePoints[] = {\t// store number (1 based), x, y, z')
    for k, s in enumerate(stores): out.append(f'\t{{ {k+1}, {s[0]:.1f}f, {s[1]:.1f}f, {s[2]:.1f}f }},')
    out.append('};')
sys.stdout.write('\n'.join(out) + '\n')
