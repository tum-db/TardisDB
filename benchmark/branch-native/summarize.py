"""Collects the runs into the tables the figures plot. Usage: python3 summarize.py <results directory>
scaling.csv (Fig. "scaling"): from scaling_{cb,depth}_t<threads>.csv and, if measured, scaling_depth_{lineage,clineage,vmap}_t<threads>.csv
and scaling_cb_clineage_t<threads>.csv,
times and speedups over one thread.
loops.csv (Fig. "loops"): from loops_{lineage,interval,lazy}_r<run>.csv (lazy if measured), one row per topology and build, medians of the runs in s."""
import csv
import glob
import os
import statistics
import sys

out = sys.argv[1]


def write(name, rows):
    with open(os.path.join(out, name), 'w', newline='') as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)


threads = os.environ.get('SCALING_THREADS', '1 2 4 8 16 18 32 36').split()
if all(os.path.exists(os.path.join(out, f'scaling_{e}_t{t}.csv')) for e in ['cb', 'depth'] for t in threads):
    rows = []
    for t in threads:
        cb = {r['k']: r for r in csv.DictReader(open(os.path.join(out, f'scaling_cb_t{t}.csv')))}
        d = {r['depth']: r for r in csv.DictReader(open(os.path.join(out, f'scaling_depth_t{t}.csv')))}
        rows.append(dict(threads=t, depth128_ms=d['128']['scan_leaf_ms'], main_ms=d['128']['scan_main_ms'],
                         shared256_ms=cb['256']['shared_ms'], perbranch256_ms=cb['256']['perbranch_ms'],
                         separate256_ms=cb['256']['separate_ms'], shared64_ms=cb['64']['shared_ms']))
    keys = ['depth128_ms', 'main_ms', 'shared256_ms', 'perbranch256_ms', 'separate256_ms', 'shared64_ms']
    for v in ['lineage', 'clineage', 'vmap']:  # the other variants of the chain at depth 128
        if all(os.path.exists(os.path.join(out, f'scaling_depth_{v}_t{t}.csv')) for t in threads):
            for t, r in zip(threads, rows):
                d = {x['depth']: x for x in csv.DictReader(open(os.path.join(out, f'scaling_depth_{v}_t{t}.csv')))}
                r[f'{v}128_ms'] = d['128']['scan_leaf_ms']
                if v == 'vmap':
                    r['vmapbuild128_ms'] = d['128']['version_map_ms']
            keys.append(f'{v}128_ms')
    if all(os.path.exists(os.path.join(out, f'scaling_cb_clineage_t{t}.csv')) for t in threads):  # the fan-out with compiled lineage
        for t, r in zip(threads, rows):
            cb = {x['k']: x for x in csv.DictReader(open(os.path.join(out, f'scaling_cb_clineage_t{t}.csv')))}
            r['clineage_shared256_ms'] = cb['256']['shared_ms']
        keys.append('clineage_shared256_ms')
    for r in rows:
        for key in keys:
            r[key.replace('_ms', '_speedup')] = round(float(rows[0][key]) / float(r[key]), 3)
    write('scaling.csv', rows)

runs = sorted(glob.glob(os.path.join(out, 'loops_lineage_r*.csv')))
if runs:
    rows = []
    loops = {b: [{r['topology']: r for r in csv.DictReader(open(f))} for f in sorted(glob.glob(os.path.join(out, f'loops_{b}_r*.csv')))]
             for b in ['lineage', 'interval', 'lazy']}
    builds = [b for b in ['lineage', 'interval', 'lazy'] if loops[b]]
    names = {'lineage': 'recursive', 'interval': 'labels', 'lazy': 'lazy'}
    for topology in ['mcts', 'beam', 'bisect']:
        for build in builds:
            r = loops[build][0][topology]
            median = lambda p: round(statistics.median(float(run[topology][f'{p}_ms']) for run in loops[build]) / 1000, 3)
            rows.append(dict(row=len(rows), topology=topology, build=build, branches=r['branches'], statements=r['statements'],
                             label=f"{topology}: {names[build]}", runs=len(loops[build]),
                             membership_mib=round(statistics.median(float(run[topology]['membership_mib']) for run in loops[build]), 3),
                             rss_mib=round(statistics.median(float(run[topology]['rss_mib']) for run in loops[build]), 1),
                             **{f'{p}_s': median(p) for p in ['fork', 'mutate', 'evaluate', 'resolve', 'alter', 'compile', 'total']}))
            rows[-1]['mutatealter_s'] = round(rows[-1]['mutate_s'] + rows[-1]['alter_s'], 3)
    write('loops.csv', rows)

# Listing "beam": an excerpt of trace_beam.sql, round 2 with two of its candidates, and the end
trace = os.path.join(out, 'trace_beam.sql')
if os.path.exists(trace):
    lines = [l.rstrip('\n') for l in open(trace)]
    start = next(i for i, l in enumerate(lines) if l.startswith('-- round 2 '))
    end = next(i for i, l in enumerate(lines) if l.startswith('-- round 3 '))
    round2 = lines[start:end]
    forks = [i for i, l in enumerate(round2) if l.startswith('CREATE BRANCH')]
    drops = [l for l in round2 if l.startswith('DROP BRANCH')]
    query = next(l for l in round2 if l.startswith('SELECT'))
    workspace = next((i for i, l in enumerate(lines) if l.startswith('-- workspace')), None)
    excerpt = lines[workspace:workspace + 3] if workspace is not None else []
    excerpt += [round2[0]] + round2[forks[0]:forks[0] + 2] + round2[forks[1]:forks[1] + 2]
    excerpt += [f'-- ... {len(forks) - 2} more candidates', query.replace(' VERSION', '\n  VERSION').replace(' GROUP BY', '\n  GROUP BY'), drops[0],
                f'-- ... {len(drops) - 1} more drops; rounds 3 to {sum(l.startswith("-- round ") for l in lines)}']
    resolve = next(i for i, l in enumerate(lines) if l.startswith('-- resolve'))
    excerpt += lines[resolve:resolve + 3]
    open(os.path.join(out, 'trace_beam_excerpt.sql'), 'w').write('\n'.join(excerpt) + '\n')

# compare_loops.csv (Fig. "comparison"): compare_loops_<topology>_<system>.csv of compare/run.sh, phases in s
if all(os.path.exists(os.path.join(out, f'compare_loops_{t}_{s}.csv')) for t in ['mcts', 'beam', 'bisect'] for s in ['tardisdb', 'dolt']):
    rows = []
    for topology in ['mcts', 'beam', 'bisect']:
        for system in ['tardisdb', 'dolt']:
            r = next(csv.DictReader(open(os.path.join(out, f'compare_loops_{topology}_{system}.csv'))))
            seconds = {p: round(float(r.get(f'{p}_ms', 0)) / 1000, 3) for p in ['fork', 'update', 'alter', 'commit', 'evaluate', 'drop', 'merge', 'total']}
            rows.append(dict(row=len(rows), label=f"{topology}: {'TardisDB' if system == 'tardisdb' else 'Dolt'}",
                             **{f'{p}_s': v for p, v in seconds.items()}, dropmerge_s=round(seconds['drop'] + seconds['merge'], 3),
                             updatealter_s=round(seconds['update'] + seconds['alter'], 3)))
    write('compare_loops.csv', rows)
