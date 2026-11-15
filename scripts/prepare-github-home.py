"""Export static README benchmark charts from the frozen business run."""
import hashlib
import json
from pathlib import Path
import statistics

ROOT = Path(__file__).resolve().parents[1]
ENGINES = ['mysql', 'redis', 'hybrid', 'sqlite-rows', 'fqdb']
NAMES = ['MySQL', 'Redis', 'MySQL+Redis', 'SQLite', 'FengquDB']
COLORS = ['#799198', '#6e8594', '#788b7c', '#647e80', '#126247']
SCENES = {
    'C': 'Key read', 'insert': 'Insert one record', 'delete': 'Delete one record',
    'patch': 'Update a small field', 'save': 'Save a complete profile',
    'B': '95% read / 5% field update', 'A': '50% read / 50% field update',
    'batch32': 'Read 32 profiles per call', 'projection': 'Read two fields',
    'F': 'Hot-key counter', 'transfer': 'Two-key transfer transaction',
}


def main():
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.ticker import FuncFormatter, MaxNLocator

    data_path = ROOT/'docs/benchmarks/results.json'
    data = json.loads(data_path.read_text(encoding='utf-8'))
    assert data['runs'] == 3 and len(data['samples']) == 660
    assets = ROOT/'docs/assets/benchmarks'
    assets.mkdir(parents=True, exist_ok=True)
    comparisons = []
    plt.rcParams.update({'font.family': 'DejaVu Sans', 'font.size': 12,
                         'svg.fonttype': 'none', 'axes.unicode_minus': False})
    for threads in (1, 8):
        for body in (128, 4096):
            body_label = '128 B' if body == 128 else '4 KiB'
            stem = ('single-thread' if threads == 1 else 'eight-threads') + ('-128b' if body == 128 else '-4kib')
            fig, axes = plt.subplots(len(SCENES), 1, figsize=(10, 40.5), dpi=120)
            fig.set_facecolor('#ffffff')
            fig.subplots_adjust(left=.105, right=.975, top=.953, bottom=.068, hspace=.94)
            fig.text(.105, .988, f'{threads} CALLING THREAD' + ('S' if threads != 1 else '') + f'  /  {body_label} PROFILE BODY',
                     fontsize=20, weight='bold', color='#152a2d', va='top')
            fig.text(.105, .978, 'FengquDB performance mode · median of 3 runs · operations/second',
                     fontsize=11, color='#506264', va='top')
            for number, ((scene, title), ax) in enumerate(zip(SCENES.items(), axes), 1):
                samples = [s for s in data['samples'] if (s['workload'], s['body'], s['threads']) == (scene, body, threads)]
                values = [[s['visible_ops_per_second'] for s in samples if s['engine'] == engine] for engine in ENGINES]
                assert all(len(v) == 3 and all(x > 0 for x in v) for v in values)
                medians = [statistics.median(v) for v in values]
                lower = [m-min(v) for m, v in zip(medians, values)]
                upper = [max(v)-m for m, v in zip(medians, values)]
                ax.bar(range(5), medians, color=COLORS, width=.53, zorder=3,
                       yerr=[lower, upper], error_kw={'ecolor': '#243f38', 'elinewidth': 1, 'capsize': 3})
                ymax = max(max(v) for v in values)
                ax.set_ylim(0, ymax*1.22)
                ax.set_xlim(-.65, 4.65)
                ax.yaxis.set_major_locator(MaxNLocator(nbins=4, min_n_ticks=3))
                ax.yaxis.set_major_formatter(FuncFormatter(lambda x, _: f'{x:,.0f}'))
                ax.set_xticks(range(5), NAMES, fontsize=11)
                ax.tick_params(axis='both', length=0, pad=8, labelcolor='#506264')
                ax.get_xticklabels()[-1].set_color('#126247')
                ax.get_xticklabels()[-1].set_weight('bold')
                ax.set_axisbelow(True)
                ax.grid(axis='y', color='#e0e5e0', linewidth=.7)
                for side in ('top', 'right', 'left'):
                    ax.spines[side].set_visible(False)
                ax.spines['bottom'].set_color('#8d9c94')
                ax.set_title(f'{number:02d}  {title}', loc='left', pad=21, fontsize=15, weight='bold', color='#152a2d')
                for i, (v, raw) in enumerate(zip(medians, values)):
                    ax.text(i, max(raw)+ymax*.045, f'{v:,.0f}', ha='center', va='bottom',
                            fontsize=11, weight='bold', color=COLORS[-1] if i == 4 else '#152a2d')
                ratios = {engine: medians[-1]/medians[i] for i, engine in enumerate(ENGINES[:-1])}
                for i, name in enumerate(NAMES[:-1]):
                    ax.text(i/4+.125, -.29, f'vs {name}', transform=ax.transAxes,
                            ha='center', va='top', fontsize=10, color='#506264')
                    ax.text(i/4+.125, -.42, f'{ratios[ENGINES[i]]:,.2f}×', transform=ax.transAxes,
                            ha='center', va='top', fontsize=14, weight='bold', color='#126247')
                comparisons.append({'workload': scene, 'body': body, 'threads': threads,
                                    'scores': dict(zip(ENGINES, medians)), 'ratios': ratios})
            fig.text(.105, .008, 'Ratios = FengquDB / comparator throughput. All axes start at zero; each chart has its own linear scale.\n'
                     'Different durability windows: FQDB 50 ms / 1 MiB, MySQL flush=2, Redis AOF everysec, SQLite WAL NORMAL.\n'
                     'Local embedded calls vs local TCP services; these are business access results, not equal-durability commit results.',
                     fontsize=9, color='#506264', va='bottom', linespacing=1.6)
            fig.savefig(assets/(stem+'.png'), dpi=120,
                        metadata={'Description': 'Measured business throughput, source docs/benchmarks/results.json; generated by scripts/prepare-github-home.py'})
            plt.close(fig)
    summary = {'source_sha256': hashlib.sha256(data_path.read_bytes()).hexdigest(),
               'metric': 'visible_ops_per_second', 'statistic': 'median of three runs',
               'ratio_definition': 'FengquDB median / comparator median', 'comparisons': comparisons}
    (assets/'comparisons.json').write_text(json.dumps(summary, indent=2)+'\n', encoding='utf-8')
    print(json.dumps({'charts': 4, 'cases': len(comparisons), 'assets': str(assets)}))


if __name__ == '__main__':
    main()
