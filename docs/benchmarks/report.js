"use strict";
(() => {
  const data = window.FQDB_BENCHMARK;
  const root = document.querySelector("#charts");
  if (!data || !data.samples || !data.scenarios) {
    root.replaceChildren(Object.assign(document.createElement("p"), {className:"error",textContent:"测量数据缺失。请将 data.js 与网页放在同一目录，或下载 CSV 查看。"}));
    return;
  }
  const engines = data.engines;
  const order=["C","insert","delete","patch","save","B","A","batch32","projection","F","transfer"];
  const scenesInOrder=[...data.scenarios].sort((a,b)=>order.indexOf(a.id)-order.indexOf(b.id));
  const names = {mysql:"MySQL",redis:"Redis",hybrid:"MySQL+Redis","sqlite-rows":"SQLite",fqdb:"FengquDB"};
  const colors = ["#799198","#6e8594","#788b7c","#647e80","#126247"];
  const controls = ["body","threads","metric","workload"].map(id => document.getElementById(id));
  const format = value => Number(value).toLocaleString("en-US",{maximumFractionDigits:value<100?2:0});
  const median = values => {const v=[...values].sort((a,b)=>a-b); return v.length%2?v[(v.length-1)/2]:(v[v.length/2-1]+v[v.length/2])/2;};
  const el = (tag, text, cls) => {const node=document.createElement(tag);if(text!==undefined)node.textContent=text;if(cls)node.className=cls;return node;};
  const svg = (tag, attrs, text) => {const node=document.createElementNS("http://www.w3.org/2000/svg",tag);for(const [k,v] of Object.entries(attrs))node.setAttribute(k,String(v));if(text!==undefined)node.textContent=text;return node;};
  for(const scene of scenesInOrder) controls[3].append(new Option(scene.name,scene.id));
  document.getElementById("run-description").textContent=`${data.date} 实测 · ${data.samples.length} 个样本 · 每场景 ${data.runs} 轮 · 全字段与重开校验通过`;
  document.getElementById("environment").textContent=data.environment;
  if(data.codecs?.length){
    const table=el("table"),head=el("thead"),tr=el("tr");
    ["简介长度","编码方式","操作","次/秒","记录字节数"].forEach(t=>tr.append(el("th",t)));head.append(tr);table.append(head);
    const tbody=el("tbody");for(const run of data.codecs)for(const sample of run.samples){const row=el("tr");[run.body===128?"128 B":"4 KiB",sample.codec==="yyjson"?"JSON / yyjson":"Protobuf wire",sample.operation==="encode"?"编码":"解码",format(sample.ops_per_second),format(sample.record_bytes)].forEach((v,i)=>row.append(el(i?"td":"th",v)));tbody.append(row);}table.append(tbody);document.getElementById("codecs").append(table);document.getElementById("codec-section").hidden=false;
  }
  function draw(rows, metric, caption) {
    const summaries=engines.map(engine=>{const values=rows.filter(row=>row.engine===engine).map(row=>row[metric]).filter(Number.isFinite);return values.length?{value:median(values),min:Math.min(...values),max:Math.max(...values)}:null;});
    const maximum=Math.max(0,...summaries.filter(Boolean).map(v=>v.max));
    const rough=(maximum||1)/4, magnitude=10**Math.floor(Math.log10(rough));
    const step=[1,2,2.5,5,10].map(x=>x*magnitude).find(x=>x>=rough)||10*magnitude;
    const ceiling=Math.ceil(maximum/step)*step||1, ticks=Math.round(ceiling/step);
    const width=800, height=324, left=78, right=20, top=35, bottom=264, pitch=(width-left-right)/5;
    const chart=svg("svg",{viewBox:`0 0 ${width} ${height}`,class:"plot",role:"img","aria-label":caption});
    chart.append(svg("title",{},caption));
    const y=value=>bottom-value/ceiling*(bottom-top);
    for(let tick=0;tick<=ticks;tick++){const value=tick*step,at=y(value);chart.append(svg("line",{x1:left,y1:at,x2:width-right,y2:at,class:tick?"grid":"axis"}),svg("text",{x:left-12,y:at+4,"text-anchor":"end"},format(value)));}
    summaries.forEach((item,i)=>{const x=left+pitch*(i+.5), barWidth=66;if(item){const bar=svg("rect",{x:x-barWidth/2,y:y(item.value),width:barWidth,height:bottom-y(item.value),fill:colors[i]});bar.append(svg("title",{},`${names[engines[i]]}: ${format(item.value)}，范围 ${format(item.min)}–${format(item.max)}`));chart.append(bar,svg("line",{x1:x,y1:y(item.min),x2:x,y2:y(item.max),class:"range"}),svg("line",{x1:x-5,y1:y(item.max),x2:x+5,y2:y(item.max),class:"range"}),svg("line",{x1:x-5,y1:y(item.min),x2:x+5,y2:y(item.min),class:"range"}),svg("text",{x,y:Math.max(16,y(item.max)-10),"text-anchor":"middle",class:"value"},format(item.value)));}else chart.append(svg("text",{x,y:bottom-10,"text-anchor":"middle"},"缺少数据"));chart.append(svg("text",{x,y:bottom+28,"text-anchor":"middle",class:i===4?"fq-label":"engine-label"},names[engines[i]]));});
    return {chart,summaries};
  }
  function render(){
    const body=+controls[0].value, threads=+controls[1].value, metric=controls[2].value, workload=controls[3].value;
    const latency=metric==="p99_us", unit=latency?"μs":"次/秒";
    const scenes=scenesInOrder.filter(s=>workload==="all"||s.id===workload);
    document.getElementById("selection-note").textContent=`${body===128?"128 B":"4 KiB"} 简介 · ${threads} 线程 · ${scenes.length} 项业务`;
    const fragment=document.createDocumentFragment();
    scenes.forEach(scene=>{
      const rows=data.samples.filter(s=>s.body===body&&s.threads===threads&&s.workload===scene.id);
      const section=el("section",undefined,"scenario");section.id="scene-"+scene.id;
      const aside=el("aside");aside.append(el("h3",scene.name),el("p",scene.description));
      const plotArea=el("div"), scroll=el("div",undefined,"plot-wrap");scroll.tabIndex=0;scroll.setAttribute("aria-label","横向滚动查看五种数据库");
      const {chart,summaries}=draw(rows,metric,`${scene.name}，${controls[2].selectedOptions[0].text}，${body} B 简介，${threads} 线程`);scroll.append(chart);plotArea.append(scroll);
      const fq=summaries[4], others=summaries.slice(0,4).filter(Boolean);
      if(fq&&others.length===4){const best=(latency?Math.min:Math.max)(...others.map(s=>s.value));const ratio=latency?best/fq.value:fq.value/best;const note=el("p",undefined,"comparison");note.append(el("strong",`${format(ratio)}×`),document.createTextNode(latency?"其他最低 P99 ÷ FengquDB P99":"FengquDB ÷ 其他最高吞吐"));aside.append(note);}
      if(fq&&others.length===4){
        const comparisons=el("div",undefined,"engine-comparisons");
        comparisons.setAttribute("aria-label",latency?"各库 P99 与 FengquDB P99 的比值":"FengquDB 与各库的吞吐比值");
        const title=el("p",latency?"对方 P99 ÷ FengquDB P99，超过 1 表示本库延迟更低。":"FengquDB 确认吞吐 ÷ 对方确认吞吐：");
        if(!latency&&metric==="business_ops_per_second")title.textContent="FengquDB 含同步吞吐 ÷ 对方含同步吞吐：";
        comparisons.append(title);
        const list=el("dl");
        engines.slice(0,4).forEach((engine,i)=>{
          const item=el("div"),value=latency?others[i].value/fq.value:fq.value/others[i].value;
          item.append(el("dt",names[engine]),el("dd",Number.isFinite(value)?`${value.toLocaleString("en-US",{minimumFractionDigits:2,maximumFractionDigits:2})}×`:"不适用"));list.append(item);
        });
        comparisons.append(list);plotArea.append(comparisons);
      }
      const detail=el("details"), summary=el("summary",`查看逐轮数据 · ${rows.length} 个样本`), tableWrap=el("div",undefined,"table-wrap"),table=el("table");tableWrap.tabIndex=0;tableWrap.setAttribute("aria-label","逐轮测量表，可横向滚动");
      const caption=el("caption",`${scene.name} · 吞吐单位 次/秒，延迟单位 μs`);table.append(caption);
      const head=el("thead"), headRow=el("tr");["数据库 / 轮次","确认吞吐","含同步吞吐","P50","P95","P99","调用次数","同步尾部 ms","容器 MiB","CPU ms"].forEach(t=>headRow.append(el("th",t)));head.append(headRow);table.append(head);
      const tbody=el("tbody");
      for(const engine of engines){for(const row of rows.filter(r=>r.engine===engine).sort((a,b)=>a.round-b.round)){const tr=el("tr");const values=[`${names[engine]} / ${row.round+1}`,row.visible_ops_per_second,row.business_ops_per_second,row.p50_us,row.p95_us,row.p99_us,row.calls,row.flush_tail_ms,row.observed.peak_bytes/1048576,row.observed.cpu_usage_usec/1000];values.forEach((v,i)=>tr.append(el(i?"td":"th",i?format(v):v)));tbody.append(tr);}}
      table.append(tbody);tableWrap.append(table);detail.append(summary,tableWrap);plotArea.append(detail);
      section.append(aside,plotArea);fragment.append(section);
    });
    root.replaceChildren(fragment);
  }
  controls.forEach(control=>control.addEventListener("change",render));render();
})();
