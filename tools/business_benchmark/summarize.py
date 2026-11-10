"""Aggregate every frozen business trial; retain ranges and generate a standalone report."""
import argparse,csv,hashlib,itertools,json,statistics
from pathlib import Path
ENGINES=['fqdb','sqlite-rows','sqlite-json','sqlite-jsonb','sqlite-pb','mysql','redis','hybrid']
LABELS={'fqdb':'FengquDB','sqlite-rows':'SQLite 普通表','sqlite-json':'SQLite JSON','sqlite-jsonb':'SQLite JSONB','sqlite-pb':'SQLite PB 控制','mysql':'MySQL','redis':'Redis Hash','hybrid':'MySQL+Redis'}
WORKLOADS={'C':'完整档案读取','projection':'读取两个字段','B':'95% 读/5% 更新','A':'50% 读/50% 更新','F':'50% 读/50% 原子计数','batch32':'读取连续 32 个档案','transfer':'两账户原子转账','save':'完整档案编码并保存'}

def main():
    p=argparse.ArgumentParser(__doc__);p.add_argument('--input',type=Path,required=True);p.add_argument('--codec',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--snapshot',type=Path);p.add_argument('--save-input',type=Path,required=True);args=p.parse_args()
    root=Path(__file__).resolve().parents[2];out=args.output.resolve();out.mkdir(parents=True,exist_ok=False)
    top=json.loads((args.input/'report.json').read_text());assert top['ok'] and all(x['removed'] for x in top['resources'])
    source_root=args.snapshot.resolve() if args.snapshot else root
    assert all(hashlib.sha256((source_root/name).read_bytes()).hexdigest()==h for name,h in top['source_manifest'].items())
    save_top=json.loads((args.save_input/'report.json').read_text());assert save_top['ok'] and all(x['removed'] for x in save_top['resources'])
    assert top['core_sha256']==save_top['core_sha256']
    assert all(hashlib.sha256((root/name).read_bytes()).hexdigest()==h for name,h in save_top['source_manifest'].items())
    trials=[];coverage=[];groups={}
    for suite,base,metadata in [('primary',args.input,top),('save',args.save_input,save_top)]:
        count=0
        for item in metadata['jobs']:
            path=base/item['path'];job=json.loads((path/'report.json').read_text());assert job['ok']
            for i,t in enumerate(job['trials']):
                assert t['ok'] and t['independent_verify']['ok'];m=t['measurement'];cfg=t['trial'];count+=1
                assert m['calls']==m['reads']+m['writes'] and m['sample_count']>0
                key=tuple(cfg[k] for k in ['engine','mode','workload','threads','body','records']);groups.setdefault(key,[]).append((cfg,t))
                row=dict(cfg,**{k:v for k,v in m.items() if (k not in cfg or k=='seconds') and k not in ['engine_info','observed']});row['requested_seconds']=cfg['seconds'];row['source_suite']=suite
                row.update(cpu_cores=m['observed']['cpu_cores_used'],memory_mib=m['observed']['peak_bytes']/2**20,disk_mib=t['private_disk_bytes']/2**20,report=str(path/('t%04d'%i)/'report.json'))
                row['cache_hit_fraction']=m['cache_hits']/max(1,m['cache_hits']+m['cache_misses']);row['write_fraction']=m['writes']/m['calls'];trials.append(row)
                coverage.append(dict(path=row['report'],sha256=hashlib.sha256(Path(row['report']).read_bytes()).hexdigest(),oracle=t['oracle_sha256'],service_restart=t['server_restart'],limits=job['limits'],source_suite=suite))
        assert count==(792 if suite=='primary' else 144),(suite,count)
    assert len(trials)==936 and len(groups)==312,(len(trials),len(groups))
    summary=[]
    for key,samples in sorted(groups.items()):
        assert len(samples)==3 and {x[0]['round'] for x in samples}=={0,1,2},key
        row=dict(zip(['engine','mode','workload','threads','body','records'],key));metrics=[t['measurement'] for _,t in samples];ops=[m['business_ops_per_second'] for m in metrics]
        row.update(ops=statistics.median(ops),min_ops=min(ops),max_ops=max(ops),spread=(max(ops)-min(ops))/statistics.median(ops),rows_per_second=statistics.median(m['rows_per_second'] for m in metrics),p50_us=statistics.median(m['p50_us'] for m in metrics),p95_us=statistics.median(m['p95_us'] for m in metrics),p99_us=statistics.median(m['p99_us'] for m in metrics),cpu_cores=statistics.median(m['observed']['cpu_cores_used'] for m in metrics),memory_mib=max(m['observed']['peak_bytes'] for m in metrics)/2**20,retries=sum(m['retries'] for m in metrics),cache_hit_fraction=statistics.median(m['cache_hits']/max(1,m['cache_hits']+m['cache_misses']) for m in metrics),write_fraction=statistics.median(m['writes']/m['calls'] for m in metrics),min_samples=min(m['sample_count'] for m in metrics),protobuf_bytes=metrics[0]['protobuf_bytes'],json_bytes=metrics[0]['json_bytes'])
        summary.append(row)
    for name,rows in [('trials.csv',trials),('summary.csv',summary)]:
        with (out/name).open('w',newline='',encoding='utf-8-sig') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
    index={(r['engine'],r['mode'],r['workload'],r['threads'],r['body']):r for r in summary}
    ratios=[]
    for r in summary:
        if r['engine']!='fqdb':continue
        others={e:index[e,r['mode'],r['workload'],r['threads'],r['body']]['ops'] for e in ENGINES if e!='fqdb'}
        ratios.append(dict(mode=r['mode'],workload=r['workload'],threads=r['threads'],body=r['body'],fq_ops=r['ops'],vs={e:r['ops']/v for e,v in others.items()},vs_best_common_sqlite=r['ops']/max(others[e] for e in ['sqlite-rows','sqlite-json','sqlite-jsonb'])))
    codec_top=json.loads((args.codec/'report.json').read_text());assert codec_top['ok'];codec_job=json.loads((args.codec/'j000/report.json').read_text());codecs=[]
    for body,codec,operation in itertools.product([128,4096],['protobuf_wire','yyjson'],['encode','decode']):
        selected=[row for trial in codec_job['codecs'] if trial['body']==body for row in trial['samples'] if row['codec']==codec and row['operation']==operation];assert len(selected)==3
        values=[r['ops_per_second'] for r in selected];codecs.append(dict(body=body,codec=codec,operation=operation,ops=statistics.median(values),min_ops=min(values),max_ops=max(values),record_bytes=selected[0]['record_bytes']))
    result=dict(ok=True,trials=len(trials),groups=len(summary),summary=summary,ratios=ratios,codecs=codecs,source_snapshot=str(source_root),primary_executable_sha256=hashlib.sha256((source_root/'business_bench').read_bytes()).hexdigest(),source_manifest=top['source_manifest'],save_source_manifest=save_top['source_manifest'],save_executable_sha256=save_top['executable_sha256'],core_sha256=top['core_sha256'],analysis_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest())
    (out/'summary.json').write_text(json.dumps(result,indent=2,ensure_ascii=False),encoding='utf-8');(out/'coverage.json').write_text(json.dumps(coverage,indent=2),encoding='utf-8')
    md=['# 同业务数据库比较（2026-11-08）','','已完成 **936 个正式样本、312 个场景中位数**。每个场景三轮，保留最小/最大吞吐、抽样延迟、CPU、内存、缓存命中率、重试和独立校验。经典来源和建模见 [基准说明](../tools/business_benchmark/README.md)。','','此结果比较各引擎常用业务实现，不能把所有差异归因于 Protobuf。本轮工作集是 4096 个用户档案，所有方案处理相同十字段并生成相同 C++ DTO；简介 128/4096 字节，编号 7 的初始化 Protobuf 样本为 205/4173 字节，JSON 为 286/4254 字节；编号或变更戳变化后长度可能变化。没有把简单字节读取当作完整业务对象读取。','','环境：Linux Docker，客户端及所需服务合计 **2 个指定 vCPU、4 GiB、无额外 swap**，同容器 TCP loopback，单线程或 8 并发。热工作集，Zipf(0.99) 访问。MySQL 8.4.11、Redis 8.10.3、SQLite 3.54.1、yyjson 0.12.0；本库为最近共享读取审查的同一生产库。','','## 业务吞吐','','以下单位为 **业务操作/秒**。save 使用调用方预建 DTO，完整编码后保存修订戳和简介末字节，不使用八字节 patch；SQL 更新常规列、Redis 写完整 Hash。修订戳包含写入者与序号，每轮从零修订重新装载，首次更新的记录长度增长也在测量内。一次 batch32 操作返回 32 个拥有字符串的完整档案，要看按档案计的吞吐可乘 32；转账算一笔业务事务。A/B/F 的每次读或写各算一个操作。这里是给定并发点的中位数，不是各产品全局峰值。']
    def value(e,w,mode='buffered',threads=8):return index[e,mode,w,threads,4096]['ops']
    read=value('fqdb','C');save=value('fqdb','save');single=value('fqdb','save',threads=1);safe=value('fqdb','save','sync')
    lead=['结论：本轮完整读取、读多写少及多数可靠提交场景表现较强；大档案并发整存仍有明显短板，结果不支持“全面胜出”。','',
        f"- 4 KiB 简介、8 并发完整读取 {read:,.0f} 次/秒，为 SQLite 普通表的 {read/value('sqlite-rows','C'):.2f} 倍、SQLite JSON 的 {read/value('sqlite-json','C'):.2f} 倍。",
        f"- 同条件非逐次刷盘整存仅 {save:,.0f} 次/秒，为 SQLite 普通表的 {save/value('sqlite-rows','save'):.2f} 倍、Redis 的 {save/value('redis','save'):.2f} 倍；本库单线程 {single:,.0f}，8 并发下降 {(1-save/single)*100:.1f}%，应优先分析并发整存路径。",
        f"- 逐次可靠提交整存 {safe:,.0f} 次/秒，高于 SQLite 普通表和 Redis，但低于 MySQL 的 {value('mysql','save','sync'):,.0f} 次/秒。",
        '- 字段投影接近 SQLite 普通表，部分原子计数和大记录转账由 SQLite 领先；完整原始结果均保留。','']
    md[4:4]=lead
    for body,threads,mode in [(128,8,'buffered'),(4096,8,'buffered'),(4096,1,'buffered'),(128,8,'sync'),(4096,8,'sync'),(4096,1,'sync')]:
        names=list(WORKLOADS) if mode=='buffered' else ['B','A','F','transfer','save']
        md+=['',f'### 简介 {body} B / {threads} 并发 / {mode.upper()}','','| 业务 | '+' | '.join(LABELS[e] for e in ENGINES)+' |','| --- | '+' | '.join(['---:']*len(ENGINES))+' |']
        for w in names:md.append('| '+WORKLOADS[w]+' | '+' | '.join(f"{index[e,mode,w,threads,body]['ops']:,.0f}" for e in ENGINES)+' |')
    md+=['','## 延迟及资源实例','','下表为 4 KiB 简介、8 并发，CPU 和内存包含客户端及服务；内存列是计时期 cgroup 峰值，含文件缓存和业务输入，不能当作核心独占 RSS。']
    for workload,mode in [('C','buffered'),('save','sync')]:
        md+=['',f'### {WORKLOADS[workload]} / {mode.upper()}','','| 方案 | 次/秒 | 抽样 P50 微秒 | 抽样 P99 微秒 | 观测 CPU 核数 | 峰值 MiB |','| --- | ---: | ---: | ---: | ---: | ---: |']
        for e in ENGINES:
            r=index[e,mode,workload,8,4096];md.append(f"| {LABELS[e]} | {r['ops']:,.0f} | {r['p50_us']:.2f} | {r['p99_us']:.2f} | {r['cpu_cores']:.2f} | {r['memory_mib']:.1f} |")
    md+=['','### 组合缓存实测命中率','','| 4 KiB / 8 并发 / BUFFERED | 读取命中率 |','| --- | ---: |']
    for w in ['C','B','A','F','batch32']:
        r=index['hybrid','buffered',w,8,4096];md.append(f"| {WORKLOADS[w]} | {r['cache_hit_fraction']*100:.2f}% |")
    md+=['','纯写入场景没有缓存读取，命中率无意义。数据库页已热时，加一层缓存未必提高混合负载吞吐，需把失效和回填成本一起算入。']
    md+=['','## 格式控制与归因','','SQLite Protobuf 为控制组，使用相同 wire 编解码及可写 BLOB 固定字段接口；主比较仍是 SQLite 普通表、JSON/JSONB，MySQL 类型列及 Redis Hash。它可以说明同一引擎内换格式后的影响，但布局、提取方式和字段更新方式也会随之变化。','','| 简介 | 编解码 | Protobuf wire 次/秒 | yyjson 次/秒 | 比值 |','| --- | --- | ---: | ---: | ---: |']
    for body,op in itertools.product([128,4096],['encode','decode']):
        a=next(x for x in codecs if x['body']==body and x['codec']=='protobuf_wire' and x['operation']==op);b=next(x for x in codecs if x['body']==body and x['codec']=='yyjson' and x['operation']==op)
        md.append(f"| {body} B | {op} | {a['ops']:,.0f} | {b['ops']:,.0f} | {a['ops']/b['ops']:.2f}× |")
    md+=['','这是使用本库 wire 工具编写的**定型 schema、可信内部数据**编解码，未采用 Google 生成代码，也不做额外的 UTF-8 验证；JSON 仍需处理转义与语法。结果支持此模型/实现的格式成本差异，不能推广为所有 Protobuf/JSON 运行时的固定倍数。业务读取中没有使用固定单键的长期结果缓存；本库使用一次性共享 get，再物化 DTO。revision/credit 增量更新使用本例显式编码器保持的固定头偏移（10/19），这属于本库的增量接口用法；不能将这些偏移照搬到省略默认字段或结构变化的任意生成 Protobuf。补充的 save 则接收调用方完整 DTO，完整编码并覆盖保存，不依赖这些固定偏移；计时包含编码、驱动和提交，不包含从库中先读回对象。','','端到端还包含索引查找、缓存、驱动、IPC、分配及持久化。MySQL/Redis 的单条同步调用受往返延迟影响；Redis 同时测了流水线批量读取。即使本库远高于网络数据库，也不能将这个倍数全部称为 Protobuf 优势。','','## 持久性与校验','','SYNC 对齐为本库 SYNC、SQLite WAL/FULL、MySQL flush_log_at_trx_commit=1、Redis AOF/always。BUFFERED 分别为本库 BUFFERED、SQLite WAL/NORMAL、MySQL flush=2、Redis AOF/everysec；其断电丢失窗口并不等价。组合方案持久性取决于 MySQL，Redis 缓存不持久化。','','所有装载和完整预热在计时外；BUFFERED 最终同步也在计时外。每次运行后按独立修订号及余额增量模型核对全部字段，再由独立进程重开核对。每容器最后一个场景另杀服务后重启核对；不代表每个场景都经历杀服务，更不是真实断电认证。原子转账核对每个账户余额及总额，F 原子计数核对每键增量。组合缓存以进程内锁防止旧值回填，仅对应同一业务进程。','','## 延迟、波动与资源','','每阶段工作线程先建立连接；吞吐的阶段时间含末尾在途操作完成和连接释放，延迟样本仅围绕业务调用及公共消费/记账。SQLite 最后连接释放可能触发正常 WAL 检查点，故它也不是纯引擎调用理论上限。P50/P95/P99 是每轮抽样延迟的分位数，再取三轮中位数；不是合并全部请求的精确 P99，也未模拟开环到达率。BUFFERED 每 64 次采一条、SYNC 每 8 次，单线程最多 100000 条。少样本尾部只能供观察，不能当作生产 SLA。CPU 包含客户端和服务；内存是计时期间 cgroup memory.current 的采样峰值，包含文件缓存。','', '各轮吞吐区间、P99、CPU、内存、命中率与重试均在 `summary.csv`；`trials.csv` 保留逐轮数据。`coverage.json` 给出全部原始报告路径、SHA256、校验摘要、重启种类及资源限制。']
    regressions=[x for x in ratios if x['vs_best_common_sqlite']<.95]
    md+=['','### 本库落后于常规 SQLite 的场景','','以下列出相差超过 5% 的场景，仅是描述阈值，不是统计显著性检验。比较对象为普通表、JSON、JSONB 三种常规实现中该场景吞吐最高者，Protobuf 控制组另列原始表。','','| 模式 | 简介 | 并发 | 业务 | 本库/最佳常规 SQLite |','| --- | ---: | ---: | --- | ---: |']
    for x in regressions:md.append(f"| {x['mode']} | {x['body']} | {x['threads']} | {WORKLOADS[x['workload']]} | {x['vs_best_common_sqlite']:.2f}× |")
    if not regressions:md.append('| 本轮无超过阈值的项目 | | | | |')
    md+=['','## 使用范围与证据','','本轮覆盖用户档案、完整编码保存、字段更新/投影、热点计数、批量读取及两账户事务；批量读取在只读数据上进行，不要求跨键一致快照，Redis pipeline 不能替代跨键事务。未覆盖关联查询、范围/聚合、超内存、长期后台整理、真实断电、开放环路过载、网络远端或 C#/Java/Python 业务客户端。它们不能由本轮吞吐推断。经典工作负载仅借鉴 YCSB A/B/C/F，F 允许原生原子加法，并非官方 YCSB 合规跑分；转账也不是 TPC-C。','','完整输出：`build/business-comparison-20261108/formal/`、`formal-save/`、`codec/` 和 `summary/`。原始失败试验也保留：首次编译的十六进制字面量分词错误、首次 MySQL 夹具父目录未创建，以及修正前的短样本，不混入正式汇总。正式编译前还移除了成功路径的错误字符串分配，并让 SQLite 控制组直接借用列字节解析。','','核心 SHA256：`'+top['core_sha256']+'`。主矩阵冻结源码及程序在 `frozen-primary/`，完整保存使用另一个已记录身份的扩展驱动，核心库相同；每个业务场景内所有数据库使用相同驱动版本。此次只增加独立工具与文档，没有修改数据库主引擎或发布包。']
    target=root/'docs/BUSINESS_DATABASE_COMPARISON_20261108.md';target.write_text('\n'.join(md)+'\n',encoding='utf-8')
    # Exportable static chart, generated only after all timed runs have ended.
    import matplotlib;matplotlib.use('Agg');import matplotlib.pyplot as plt
    names=['C','projection','B','A','F','batch32','transfer','save'];fig,axes=plt.subplots(2,4,figsize=(16,8));colors=['#145E75','#8DADBA','#B3C8D0','#D1DFE4','#658FA1','#BD9973','#99858D','#5F8575']
    for ax,w in zip(axes.flat,names):
        values=[index[e,'buffered',w,8,4096]['ops'] for e in ENGINES];ax.bar(range(len(ENGINES)),values,color=colors);ax.set_yscale('log');ax.set_xticks(range(len(ENGINES)),['FQ','SQL rows','SQL JSON','SQL JSONB','SQL PB','MySQL','Redis','My+Redis'],rotation=55,ha='right',fontsize=8);ax.set_title({'C':'Full profile read','projection':'Two fields','B':'95% read / 5% field update','A':'50% read / 50% field update','F':'50% read / 50% increment','batch32':'Read 32 profiles','transfer':'Atomic transfer','save':'Full profile encode + save'}[w],fontsize=10);ax.set_ylabel('business ops / sec (log)');ax.grid(axis='y',alpha=.15)
    fig.suptitle('2 vCPU / 4 GiB total - 4096 B bio - 8 clients - median of 3 - hot data - buffered policies differ',fontsize=12)
    fig.tight_layout(rect=(0,0,1,.95));fig.savefig(out/'throughput.png',dpi=160);plt.close(fig)
    print(json.dumps(dict(ok=True,trials=len(trials),groups=len(summary),sqlite_regressions=len(regressions))))
if __name__=='__main__':main()
