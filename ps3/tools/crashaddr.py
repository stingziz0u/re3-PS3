#!/usr/bin/env python3
# crashaddr.py ELF LOG -- the [crash] lines of a re3-PS3 / reVC-PS3 log with the
# function each address belongs to, and the [ftrace] lines (function
# entry/exit trace) as a call tree plus the calls still open at the crash.
# PPU64 ELFs name functions by their .opd descriptors (nm shows those), so
# addr2line can't map code addresses: here each descriptor's code address
# is read from .opd and paired with its symbol name.
import struct, subprocess, sys, re, bisect, os, shutil

elf, log = sys.argv[1], sys.argv[2]
data = open(elf, 'rb').read()
# section headers (ELF64 big-endian)
shoff, = struct.unpack('>Q', data[0x28:0x30])
shentsize, shnum, shstrndx = struct.unpack('>HHH', data[0x3a:0x40])
secs = []
for i in range(shnum):
    h = data[shoff + i*shentsize: shoff + (i+1)*shentsize]
    name, typ, flags, addr, off, size = struct.unpack('>IIQQQQ', h[:40])
    secs.append((name, addr, off, size))
strtab = secs[shstrndx]
def secname(n):
    s = data[strtab[2] + n:]
    return s[:s.index(b'\0')].decode()
opd = next(s for s in secs if secname(s[0]) == '.opd')

def opd_code(a):
    if not (opd[1] <= a < opd[1] + opd[3]):
        return None
    return struct.unpack('>Q', data[opd[2] + a - opd[1]: opd[2] + a - opd[1] + 8])[0]

funcs = []
desc = {}	# .opd descriptor -> name (what the [ftrace] lines carry)
nmtool = shutil.which('ppu-nm') or os.path.join(os.environ.get('PS3DEV', '/usr/local/ps3dev'), 'ppu/bin/ppu-nm')
nm = subprocess.run([nmtool, '-C', elf], capture_output=True, text=True).stdout
for line in nm.splitlines():
    m = re.match(r'([0-9a-f]+) ([TtWwDd]) (.*)', line)
    if m:
        p = m.groups()
        code = opd_code(int(p[0], 16))
        if code:
            funcs.append((code, p[2]))
            desc.setdefault(int(p[0], 16), p[2])
funcs.sort()
starts = [f[0] for f in funcs]

def short(n):
    # drop the parameter list: "CPed::Fight()" instead of the whole signature
    i = n.find('(')
    return n[:i] + '()' + (n[n.rfind(')') + 1:] if i >= 0 else '') if i > 0 else n

def name_of(a):
    if a in desc:
        return short(desc[a])
    i = bisect.bisect_right(starts, a) - 1
    return '%s+0x%x' % (funcs[i][1], a - funcs[i][0]) if i >= 0 else '0x%08x' % a

events = []
for line in open(log, errors='replace'):
    if line.startswith('[ftrace]') and 'events' not in line:
        events += [int(w, 16) for w in line.split()[1:]]
if events:
    # depth relative to the oldest event; the tree is printed from the
    # shallowest point of the last TAIL events
    TAIL = 400
    depth, depths = 0, []
    for e in events:
        if e & 1:
            depth -= 1
            depths.append(depth)
        else:
            depths.append(depth)
            depth += 1
    tail = range(max(0, len(events) - TAIL), len(events))
    base = min(depths[i] for i in tail)
    print('[ftrace] last %d of %d calls (> entry, < exit):' % (len(tail), len(events)))
    for i in tail:
        e = events[i]
        print('[ftrace] %s%s %s' % ('  ' * (depths[i] - base), '<' if e & 1 else '>', name_of(e & ~1)))
    # calls entered and not left: the stack at the crash (innermost last)
    stack = []
    for e in events:
        if e & 1:
            if stack and stack[-1] == e & ~1:
                stack.pop()
            elif (e & ~1) in stack:
                while stack and stack.pop() != e & ~1:
                    pass
        else:
            stack.append(e)
    print('[ftrace] still open at the crash (outermost first):')
    for e in stack[-40:]:
        print('[ftrace]   ' + name_of(e))
    print()

for line in open(log, errors='replace'):
    if not line.startswith('[crash]'):
        continue
    line = line.rstrip('\n')
    m = re.search(r'@([0-9a-f]{8})', line)
    if m:
        a = int(m.group(1), 16)
        i = bisect.bisect_right(starts, a) - 1
        where = '%s+0x%x' % (funcs[i][1], a - funcs[i][0]) if i >= 0 else '?'
        line += '   <- ' + where
    print(line)
