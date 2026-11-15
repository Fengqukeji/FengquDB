"""Export only a complete, verified five-engine business run to a static report."""
import argparse
import csv
import hashlib
import html
import io
import itertools
import json
from pathlib import Path
import statistics
import zipfile

ENGINES = ['mysql', 'redis', 'hybrid', 'sqlite-rows', 'fqdb']
SCENARIOS = [
    ('C', '按键读取', '按 ID 读取一份完整档案，转换为业务对象。'),
    ('save', '完整保存', '提交整份档案对象，更新修订号及简介中的一个字符。允许数据库内部识别未变化的部分。'),
    ('insert', '单条新增', '每次插入一个全新 ID，共 65,536 份档案；线程之间不重复。'),
    ('delete', '单条删除', '每次删除一个存在的 ID，共 65,536 次；最终核对空库。'),
    ('patch', '小字段修改', '按 ID 修改 8 字节修订号，其他字段保持原值。'),
    ('B', '95% 读 · 5% 字段更新', '大部分调用读取完整档案，少量调用更新修订号。'),
    ('A', '50% 读 · 50% 字段更新', '一半调用读取完整档案，一半调用更新修订号。'),
    ('batch32', '一次读 32 份档案', '一次业务调用取回 32 份档案并转换为对象；吞吐按业务调用次数计算。'),
    ('projection', '只读两个字段', '只需要修订号和余额；SQL 选择列，Redis HMGET，FengquDB 从共享二进制解析所需字段。'),
    ('F', '热点计数', '一半调用在 128 个热点档案上原子加一，一半读取完整档案。'),
    ('transfer', '双键转账事务', '在 128 个热点账户之间原子转移 1 单位余额，核对每个账户和总余额。'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=Path('docs/benchmarks'))
    args = parser.parse_args()
    root, output = args.input.resolve(), args.output.resolve()
    manifest = json.loads((root/'report.json').read_text())
    assert manifest['ok'] and not manifest['configuration']['pilot']
    config = manifest['configuration']
    assert config['engines'].split(',') == ENGINES and not config['reliable']
    assert set(config['workloads'].split(',')) == {s[0] for s in SCENARIOS}
    rounds = config['runs']
    assert rounds >= 3
    expected = set(itertools.product(ENGINES, config['workloads'].split(','),
                     map(int, config['bodies'].split(',')), map(int, config['threads'].split(',')),
                     range(rounds)))
    samples, seen = [], set()
    raw_files = [root/'report.json', root/'image-build.log']
    codecs = json.loads((root/'codec.json').read_text())
    assert {run['body'] for run in codecs} == {128,4096}
    assert all(run['ok'] and len(run['samples']) == 4 for run in codecs)
    raw_files.append(root/'codec.json')
    for job in manifest['jobs']:
        job_root = root/job['path']
        report = json.loads((job_root/'report.json').read_text())
        assert report['ok']
        assert all(resource['removed'] for resource in manifest['resources'])
        for item in report['trials']:
            assert item['ok'] and item['prepare']['ok'] and item['verify']['ok']
            row = item['run']
            key = row['engine'], row['workload'], row['body'], row['threads'], item['trial']['round']
            assert row['ok'] and key in expected and key not in seen
            assert row['calls'] > 0 and row['visible_seconds'] > 0
            assert row['p50_us'] <= row['p95_us'] <= row['p99_us']
            assert row['sample_count'] > 0
            assert (row['mode'] == 'performance') == (row['engine'] == 'fqdb')
            seen.add(key)
            samples.append(dict(row, round=item['trial']['round'],
                                oracle_sha256=item['oracle_sha256'], settings=item['settings']))
        raw_files.extend(p for p in job_root.rglob('*') if p.is_file())
    assert seen == expected, f'missing samples: {expected-seen}'
    samples.sort(key=lambda r: (config['workloads'].split(',').index(r['workload']),r['body'],r['threads'],ENGINES.index(r['engine']),r['round']))
    result = {'format': 'fengqudb-public-business-report-1', 'date': '2026-11-15',
        'runs': rounds, 'engines': ENGINES,
        'scenarios': [dict(id=id, name=name, description=description) for id,name,description in SCENARIOS],
        'environment': 'Windows 主机：AMD Ryzen 9 9950X3D；Linux 测试环境：Docker / WSL2。客户端与服务合计限制 2 逻辑 CPU（0、2，按虚拟机拓扑属于不同核心）／4 GiB／无 swap；未独占宿主物理核。',
        'core_sha256': manifest['core_sha256'], 'base_image_id': manifest['base_image_id'],
        'image_id': manifest['image_id'], 'versions': manifest['versions'],
        'configuration': {k:v for k,v in config.items() if k not in ('output','core','core_root')},
        'source_manifest': manifest['source_manifest'], 'samples': samples, 'codecs': codecs}
    output.mkdir(parents=True, exist_ok=True)
    content = json.dumps(result, ensure_ascii=False, indent=2)+'\n'
    (output/'results.json').write_text(content, encoding='utf-8')
    (output/'data.js').write_text('window.FQDB_BENCHMARK = '+content.rstrip()+';\n', encoding='utf-8')
    fields = ['engine','mode','workload','body','threads','round','records','calls','visible_seconds',
              'seconds','visible_ops_per_second','business_ops_per_second','rows_per_second',
              'sample_count','p50_us','p95_us','p99_us','flush_tail_ms','retries','cache_hits','cache_misses']
    buffer = io.StringIO(newline='')
    writer = csv.DictWriter(buffer, fieldnames=fields+['peak_bytes','cpu_usage_usec'], extrasaction='ignore')
    writer.writeheader()
    for row in samples:
        writer.writerow(dict(row, **{k:row['observed'][k] for k in ('peak_bytes','cpu_usage_usec')}))
    (output/'results.csv').write_text(buffer.getvalue(), encoding='utf-8', newline='')
    repository = Path(__file__).resolve().parents[1]
    legal_sections = []
    for title,path in [('FengquDB · Apache-2.0',repository/'LICENSE'),
                       ('NOTICE',repository/'NOTICE'),
                       ('Protocol Buffers · BSD-3-Clause',repository/'licenses/Protobuf-BSD-3-Clause.txt'),
                       ('JSON for Modern C++ · MIT',repository/'licenses/nlohmann-json.MIT')]:
        legal_sections.append('<h2>'+html.escape(title)+'</h2><pre style="font:inherit;white-space:pre-wrap">'+html.escape(path.read_text(encoding='utf-8'))+'</pre>')
    (output/'licenses.html').write_text('<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>FengquDB · 许可与来源</title><link rel="stylesheet" href="report.css"><main class="wrap" style="padding-block:36px;max-width:900px"><a href="index.html">返回性能报告</a><h1 style="margin-block:30px">许可与来源</h1><p>本页与测量数据采用 Apache-2.0。冻结复现 ZIP 内保留完整第三方说明及适用的工具链许可证；MySQL、Redis、SQLite 与基准依赖仍采用各自许可证。</p>'+''.join(legal_sections)+'</main></html>',encoding='utf-8')
    with zipfile.ZipFile(output/'raw-evidence.zip','w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for path in sorted(set(raw_files)):
            archive.write(path, path.relative_to(root).as_posix())
    with zipfile.ZipFile(output/'reproduction-inputs.zip','w',zipfile.ZIP_DEFLATED,compresslevel=9) as archive:
        for directory in ('image-context','core-source'):
            for path in sorted((root/directory).rglob('*')):
                if path.is_file(): archive.write(path,path.relative_to(root).as_posix())
        for name in ('LICENSE','NOTICE','THIRD_PARTY_NOTICES.md','scripts/run-public-benchmark.py',
                     'scripts/measure-benchmark-codec.py','scripts/publish-benchmark-report.py',
                     'scripts/fetch-benchmark-references.py','tools/final_benchmark/Dockerfile.base'):
            path = Path(__file__).resolve().parents[1]/name
            archive.write(path,name)
        for path in (Path(__file__).resolve().parents[1]/'licenses').iterdir():
            if path.is_file():archive.write(path,'licenses/'+path.name)
    victories = 0
    ratios = []
    for workload,body,threads in itertools.product(config['workloads'].split(','),map(int,config['bodies'].split(',')),map(int,config['threads'].split(','))):
        scores={e:statistics.median(r['visible_ops_per_second'] for r in samples if (r['engine'],r['workload'],r['body'],r['threads'])==(e,workload,body,threads)) for e in ENGINES}
        ratio=scores['fqdb']/max(v for k,v in scores.items() if k!='fqdb')
        ratios.append(dict(workload=workload,body=body,threads=threads,ratio=ratio,scores=scores))
        victories += ratio>1
    summary={'samples':len(samples),'cases':len(ratios),'fqdb_leads':victories,'comparisons':ratios,
             'files_sha256':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in output.iterdir() if p.is_file() and p.name!='summary.json' and p.suffix in ('.zip','.csv','.json')}}
    (output/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
    print(json.dumps({k:v for k,v in summary.items() if k not in ('comparisons','files_sha256')}))


if __name__ == '__main__':
    main()
