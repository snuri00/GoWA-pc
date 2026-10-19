import argparse
import collections
import sys

ELF = 'bin/01.12/GOWA.patched.elf'
PMS = {0x436cf002: 'WWS', 0x4306dc02: 'taskset', 0x42377002: 'sys'}
PHASES = ['disp', 'kernel', 'pm', 'job', 'native', 'interp', 'host_k', 'host_pm', 'host_job', 'idle']
CHANNEL_COST = {
    ('w', 16): 18, ('w', 17): 18, ('w', 18): 18, ('w', 19): 18, ('w', 20): 18,
    ('w', 22): 110, ('w', 23): 110, ('r', 24): 120, ('r', 27): 110, ('r', 8): 200,
    ('r', 13): 110, ('r', 0): 250, ('w', 2): 250, ('w', 1): 250, ('w', 26): 150,
}
DEFAULT_CHANNEL_COST = 130
COUNT_COST = 110


def mfc_cost(cmd, n, nbytes):
    hi = cmd & 0xF0
    if cmd == 0xD0:
        return n * 450
    if cmd in (0xB4, 0xB0, 0xB8):
        return n * 550
    if (cmd & 0x04) and cmd < 0x50:
        return n * 350 + nbytes * (1.2 if hi == 0x40 else 1.9)
    if hi == 0x40:
        return n * 330 + nbytes * 0.65
    if hi in (0x20, 0x30, 0xA0):
        return n * 420 + nbytes * 1.72
    return n * 60


class Images:
    def __init__(self, path):
        self.elf = open(path, 'rb').read()
        self.names = {}

    def name(self, hexs):
        if hexs not in self.names:
            i = self.elf.find(bytes.fromhex(hexs))
            self.names[hexs] = '%x' % (i + 0x10000) if i >= 0 else 'unk:' + hexs[:8]
        return self.names[hexs]

    def key(self, img, pm, wid):
        p = PMS.get(pm, 'pm%08x' % pm)
        return (p, self.name(img) if p == 'WWS' else 'wid%d' % wid)


def parse_jobs(path, images):
    res = collections.defaultdict(collections.Counter)
    blocks = collections.defaultdict(collections.Counter)
    cur = None
    for line in open(path):
        t = line.split()
        if not t:
            continue
        if t[0] == 'job':
            cur = images.key(t[1], int(t[3], 16), int(t[5]))
            c = res[cur]
            c['entries'] += int(t[7])
            c['ndisp'] += int(t[9])
            c['hle_calls'] += int(t[13])
            c['hle_insns'] += int(t[14])
        elif t[0] in ('r0', 'r1', 'r2'):
            r = t[0]
            c = res[cur]
            c['blocks_' + r] += int(t[2])
            c['insns_' + r] += int(t[4]) + int(t[10])
            ci, mi = t.index('chan'), t.index('mfc')
            cost = 0.0
            for x in t[ci + 1:mi]:
                k, v = x.split('=')
                kind, ch, v = k[0], int(k[1:]), int(v)
                c['nch_' + r] += v
                c['ch_%s%d' % (kind, ch)] += v
                if kind == 'c':
                    cost += COUNT_COST * v
                elif not (kind == 'w' and ch == 21):
                    cost += CHANNEL_COST.get((kind, ch), DEFAULT_CHANNEL_COST) * v
            for x in t[mi + 1:]:
                k, v = x.split('=')
                n, b = v.split('/')
                cost += mfc_cost(int(k, 16), int(n), int(b))
                c['mfc_%s_%s' % (r, k)] += int(n)
                c['mfcb_%s_%s' % (r, k)] += int(b)
            c['chcost_' + r] += cost
        elif t[0] == 'block':
            key = images.key(t[1], int(t[2], 16), int(t[3]))
            lsa = int(t[4], 16)
            blocks[(key, lsa)]['calls'] += int(t[5])
            blocks[(key, lsa)]['insns'] += int(t[6])
            blocks[(key, lsa)]['cycles'] += int(t[7])
    return res, blocks


def parse_sample(path, images, total):
    km = {}
    hist = collections.defaultdict(collections.Counter)
    for line in open(path):
        t = line.split()
        if t[0] == 'key':
            km[int(t[1])] = images.key(t[2], int(t[3], 16), int(t[4]))
        elif t[0] == 'hist' and PHASES[int(t[2])] != 'idle':
            hist[km[int(t[1])]][PHASES[int(t[2])]] += int(t[3])
    n = sum(sum(c.values()) for c in hist.values())
    return {k: {p: v * total / n for p, v in c.items()} for k, c in hist.items()}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('jobs')
    ap.add_argument('sample')
    ap.add_argument('cycles_per_insn', type=float)
    ap.add_argument('insns', type=float)
    ap.add_argument('--plain', type=float, default=0.0)
    ap.add_argument('--elf', default=ELF)
    ap.add_argument('--min', type=float, default=0.008)
    a = ap.parse_args()
    images = Images(a.elf)
    counts, blocks = parse_jobs(a.jobs, images)
    total = a.cycles_per_insn * a.insns
    over = 0.0
    ndisp = sum(c['ndisp'] for c in counts.values())
    if a.plain:
        over = (a.cycles_per_insn - a.plain) * a.insns / max(1, ndisp)
    samp = parse_sample(a.sample, images, total)
    rows = []
    for k in set(counts) | set(samp):
        s = samp.get(k, {})
        c = counts.get(k, collections.Counter())
        bt = sum(c['blocks_r%d' % r] for r in range(3)) or 1
        disp = max(0.0, s.get('disp', 0.0) - over * c['ndisp'])
        part = {
            'native': s.get('native', 0.0),
            'job': s.get('job', 0.0) + s.get('interp', 0.0),
            'pm': s.get('pm', 0.0),
            'kernel': s.get('kernel', 0.0),
            'disp': disp,
            'chan': c['chcost_r0'] + c['chcost_r1'] + c['chcost_r2'],
        }
        for r, nm in enumerate(('kernel', 'pm', 'job')):
            part['disp_' + nm] = disp * c['blocks_r%d' % r] / bt
            part['chan_' + nm] = c['chcost_r%d' % r]
        part['model'] = sum(part[x] for x in ('native', 'job', 'pm', 'kernel', 'disp', 'chan'))
        part['replay_host'] = s.get('host_k', 0.0) + s.get('host_pm', 0.0) + s.get('host_job', 0.0)
        rows.append((k, part, c))
    rows.sort(key=lambda r: -r[1]['model'])
    T = sum(r[1]['model'] for r in rows)
    G = collections.Counter()
    for _, p, _ in rows:
        for x, v in p.items():
            G[x] += v
    pct = lambda v: 100.0 * v / T
    print('modeled runtime SPU cycles per pass %.0fM: replay %.0fM, replay channel host %.0fM replaced by modeled runtime channel cost %.0fM, dispatch sampling overhead %.1f cycles/dispatch removed' % (
        T / 1e6, total / 1e6, G['replay_host'] / 1e6, G['chan'] / 1e6, over))
    print()
    print('%-8s %-9s %6s | %6s %6s %6s %6s %6s %6s | %6s %8s %7s %8s %7s' % ('pm', 'image', 'tot%', 'native', 'jobAOT', 'PMAOT', 'kernel', 'disp', 'chan', 'jobs', 'kcyc/job', 'PMins/j', 'jobins/j', 'chops/j'))
    for k, p, c in rows:
        if p['model'] / T < a.min:
            continue
        e = c['entries']
        per = lambda v: ('%.0f' % (v / e)) if e else '-'
        nch = c['nch_r0'] + c['nch_r1'] + c['nch_r2']
        print('%-8s %-9s %6.2f | %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f | %6d %8s %7s %8s %7s' % (
            k[0], k[1], pct(p['model']), pct(p['native']), pct(p['job']), pct(p['pm']), pct(p['kernel']), pct(p['disp']), pct(p['chan']),
            e, per(p['model'] / 1e3), per(c['insns_r1']), per(c['insns_r2']), per(nch)))
    print('%-18s %6.2f | %6.2f %6.2f %6.2f %6.2f %6.2f %6.2f' % ('ALL', 100.0, pct(G['native']), pct(G['job']), pct(G['pm']), pct(G['kernel']), pct(G['disp']), pct(G['chan'])))
    print()
    print('per region (code + dispatch share by block count + modeled channel cost), %% of total')
    print('%-8s %-9s | %-30s | %-30s | %s' % ('pm', 'image', 'PM region code/disp/chan', 'job region code/disp/chan', 'kernel'))
    for k, p, c in rows:
        if p['model'] / T < a.min:
            continue
        pm = p['pm'] + p['disp_pm'] + p['chan_pm']
        jb = p['job'] + p['disp_job'] + p['chan_job']
        kr = p['kernel'] + p['disp_kernel'] + p['chan_kernel']
        print('%-8s %-9s | %6.2f = %5.2f %5.2f %5.2f   | %6.2f = %5.2f %5.2f %5.2f   | %5.2f' % (
            k[0], k[1], pct(pm), pct(p['pm']), pct(p['disp_pm']), pct(p['chan_pm']), pct(jb), pct(p['job']), pct(p['disp_job']), pct(p['chan_job']), pct(kr)))
    S = collections.Counter()
    for k, p, c in rows:
        S[k[0] + '_pm'] += p['pm'] + p['disp_pm'] + p['chan_pm']
        S[k[0] + '_job'] += p['job'] + p['disp_job'] + p['chan_job']
        S[k[0] + '_kernel'] += p['kernel'] + p['disp_kernel'] + p['chan_kernel']
        S[k[0] + '_native'] += p['native']
    for name in ('WWS', 'taskset', 'sys'):
        print('%-8s total: native %5.2f  PM region %5.2f  job region %5.2f  kernel %5.2f' % (name, pct(S[name + '_native']), pct(S[name + '_pm']), pct(S[name + '_job']), pct(S[name + '_kernel'])))
    print()
    ch = collections.Counter()
    for k, p, c in rows:
        for x, v in c.items():
            if x.startswith('ch_') or x.startswith('mfc_') or x.startswith('mfcb_'):
                ch[x] += v
    print('channel ops per pass:', ' '.join('%s=%d' % (x[3:], v) for x, v in ch.most_common() if x.startswith('ch_')))
    for r in ('r0', 'r1', 'r2'):
        items = sorted(((x.split('_')[2], v, ch['mfcb_%s_%s' % (r, x.split('_')[2])]) for x, v in ch.items() if x.startswith('mfc_' + r + '_')), key=lambda t: -t[2])
        print('mfc %s (%s):' % (r, ('kernel', 'PM', 'job')[int(r[1])]), ' '.join('%s=%d/%.1fMB' % (cmd, n, b / 1e6) for cmd, n, b in items))


if __name__ == '__main__':
    sys.exit(main())
