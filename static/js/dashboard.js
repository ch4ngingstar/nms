/* NMS console runtime.
   LIVE  — EventSource('/api/stream') + REST seed when served same-origin.
   DEMO  — a simulator emitting frames shaped like the real SSE so the console
           is populated on a projector with nothing else running. */

const state = { nodes:{}, jobs:{}, aps:{}, ble:{}, alerts:[], frames:0 };
let paused = false, live = false; // activity feed state

const $ = s => document.querySelector(s);
const el = (t,c)=>{const e=document.createElement(t);if(c)e.className=c;return e;};
const esc = s => String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));
const clk = () => new Date().toTimeString().slice(0,8);
const ago = ts => { const s=Math.max(0,(Date.now()-ts)/1000);
  if(s<60)return Math.floor(s)+'s ago'; if(s<3600)return Math.floor(s/60)+'m ago'; return Math.floor(s/3600)+'h ago'; };
const rssiPct = r => Math.max(4,Math.min(100,Math.round((r+95)/60*100)));
const fmtUp = s => { const h=s/3600|0,m=(s%3600)/60|0; return h?`${h}h ${m}m`:`${m}m`; };

const JOB_STATE = {pending:'mute',accepted:'info',done:'ok',incomplete:'warn',error:'crit',timed_out:'crit'};
const EVDOT = {announce:'info',job:'iris',telemetry:'mute',alert:'crit',monitor:'warn'};

/* live feed */
const feed = $('#feed');
function pushFeed(kind, node, html, alert){
  if(paused) return;
  const row = el('div','ev'+(alert?' alert':''));
  row.innerHTML =
    `<span class="edot ${EVDOT[kind]||'mute'}"></span>`+
    `<span class="et">${clk()}</span>`+
    `<span class="em"><span class="en">${esc(node)}</span> ${html}</span>`;
  feed.appendChild(row);
  while(feed.childElementCount>90) feed.removeChild(feed.firstChild);
  feed.scrollTop = feed.scrollHeight;
}

/* SSE handlers (real payload shapes) */
function onNodeStatus(d){
  const n = state.nodes[d.node] || (state.nodes[d.node]={node_id:d.node});
  if(d.state) n.state=d.state;
  if(d.label!==undefined) n.label=d.label;
  if(d.capabilities) n.capabilities=d.capabilities;
  n.last_seen=Date.now();
  if(d.state==='online') pushFeed('announce',d.node,`announced <b>online</b> · ${(d.capabilities||[]).length} capabilities`);
  else if(d.state) pushFeed('announce',d.node,`status <b>${esc(d.state)}</b>`);
  renderNodes(); renderStats();
}
function onJobEvent(d){
  const j = state.jobs[d.job_id] || (state.jobs[d.job_id]={job_id:d.job_id,node_id:d.node,created:Date.now()});
  j.state=d.state; j.node_id=d.node; j.event=d.event; j.updated=Date.now();
  if(d.cmd) j.cmd=d.cmd;
  if(d.chunks!=null) j.chunks=d.chunks;
  if(d.gaps) j.gaps=d.gaps;
  if(d.duration_ms!=null) j.duration_ms=d.duration_ms;
  const bad = ['error','timed_out','incomplete'].includes(d.state);
  pushFeed('job',d.node,`${esc(j.cmd||'job')} <b>${esc(d.state)}</b>`,bad);
  renderJobs(); renderStats();
}
function onTelemetry(d){
  const n = state.nodes[d.node] || (state.nodes[d.node]={node_id:d.node});
  n.telemetry={free_heap:d.free_heap,uptime_s:d.uptime_s,rssi:d.rssi,channel:d.channel};
  n.last_seen=Date.now();
  pushFeed('telemetry',d.node,`telemetry · heap ${(d.free_heap/1024|0)} KB · rssi ${d.rssi??'—'} dBm`);
  renderNodes();
}
function onMonitor(d){
  const up=(d.results||[]).filter(r=>r.status==='up').length, tot=(d.results||[]).length;
  pushFeed('monitor',d.node,`monitor cycle · <b>${up}/${tot}</b> reachable`);
}
function onAlert(a){
  a.detected_at = a.detected_at || Date.now();
  state.alerts.unshift(a);
  if(state.alerts.length>60) state.alerts.pop();
  const label = ({deauth_flood:'deauth flood',rogue_ap:'rogue AP',evil_twin:'evil twin'})[a.alert_type]||a.alert_type;
  pushFeed('alert',a.node_id,`<b>${esc(label)}</b> detected${a.channel?(' · ch '+a.channel):''}`,true);
  renderAlerts(); renderStats();
}

/* renderers */
function renderStats(){
  const nodes=Object.values(state.nodes);
  const online=nodes.filter(n=>n.state==='online').length;
  const active=Object.values(state.jobs).filter(j=>['pending','accepted'].includes(j.state)).length;
  $('#s-nodes').textContent=online;
  $('#s-jobs').textContent=active;
  $('#s-alerts').textContent=state.alerts.length;
  $('#s-frames').textContent=state.frames.toLocaleString();
  $('#b-fleet').textContent=nodes.length;
  $('#b-jobs').textContent=Object.keys(state.jobs).length;
  $('#b-rf').textContent=Object.keys(state.aps).length+Object.keys(state.ble).length;
  $('#b-sec').textContent=state.alerts.length;
}
function renderNodes(){
  const wrap=$('#nodes'); const nodes=Object.values(state.nodes);
  if(!nodes.length){wrap.innerHTML='<div class="empty">No probes enrolled.</div>';return;}
  wrap.innerHTML='';
  nodes.forEach(n=>{
    const t=n.telemetry||{};
    const heapPct=t.free_heap?Math.min(100,Math.round(t.free_heap/320000*100)):0;
    const rssiP=t.rssi?rssiPct(t.rssi):0;
    const kind=n.node_id==='probe-server'?'virtual peer':'esp32 · wroom-32';
    const dotc=n.state==='online'?'ok':(n.state==='offline'?'off':'warn');
    const caps=(n.capabilities||[]).map(c=>`<span class="chip">${esc(c)}</span>`).join('');
    const card=el('div','card');
    card.innerHTML=
      `<div class="top"><span class="sd ${dotc}"></span><span class="id">${esc(n.node_id)}</span><span class="kind">${kind}</span></div>`+
      `<div class="meta">${esc(n.label||'—')} · last seen ${n.last_seen?ago(n.last_seen):'—'}</div>`+
      `<div class="chips">${caps||'<span class="faint">no capabilities advertised</span>'}</div>`+
      `<div class="meters">`+
        meter('Free heap',t.free_heap?`${(t.free_heap/1024|0)} KB`:'—',heapPct,heapPct<20?'crit':'ok')+
        meter('Uptime',t.uptime_s?fmtUp(t.uptime_s):'—',t.uptime_s?100:0,'')+
        meter('Signal',t.rssi?`${t.rssi} dBm`:'—',rssiP,rssiP<35?'warn':'ok')+
        meter('Channel',t.channel?String(t.channel):'—',t.channel?100:0,'')+
      `</div>`;
    wrap.appendChild(card);
  });
}
function meter(lab,val,pct,cls){
  return `<div class="meter"><div class="lab"><span>${lab}</span><b>${val}</b></div>`+
         `<div class="track"><div class="fill ${cls}" style="width:${pct}%"></div></div></div>`;
}
function renderJobs(){
  const tb=$('#jobs'); const jobs=Object.values(state.jobs).sort((a,b)=>b.updated-a.updated);
  if(!jobs.length){tb.innerHTML='<tr><td class="empty" colspan="7">No jobs issued.</td></tr>';return;}
  tb.innerHTML=jobs.map(j=>{
    const p=JOB_STATE[j.state]||'mute';
    return `<tr><td class="m">${esc((j.job_id||'').slice(0,8))}</td>`+
      `<td class="m" style="color:var(--iris-2)">${esc(j.node_id||'')}</td>`+
      `<td class="m">${esc(j.cmd||'—')}</td>`+
      `<td><span class="pill ${p}"><span class="sd"></span>${esc(j.state||'')}</span></td>`+
      `<td class="m">${j.chunks??'—'}</td>`+
      `<td class="m">${(j.gaps&&j.gaps.length)?('<span style="color:var(--warn)">'+j.gaps.length+'</span>'):'0'}</td>`+
      `<td class="m dim">${j.duration_ms?j.duration_ms+' ms':'—'}</td></tr>`;
  }).join('');
}
function renderRF(){
  const wrap=$('#aps'); const aps=Object.values(state.aps);
  wrap.innerHTML = aps.length? aps.map(ap=>{
    const vans=ap.observations.map(o=>
      `<div class="van"><span class="n">${esc(o.node)}</span>`+
      `<span class="track"><span class="fill" style="width:${rssiPct(o.rssi)}%"></span></span>`+
      `<span class="v">${o.rssi} dBm</span></div>`).join('');
    return `<div class="ap"><div class="h"><span class="ssid">${esc(ap.ssid||'Hidden network')}</span>`+
      `<span class="bssid">${esc(ap.bssid)}</span>`+
      `<span class="ch">Channel ${ap.observations[0]?.channel??'—'}</span></div>${vans}</div>`;
  }).join(''):'<div class="empty">No access points observed.</div>';

  const tb=$('#ble'); const bles=Object.values(state.ble);
  tb.innerHTML = bles.length? bles.map(b=>{
    const best=Math.max(...b.observations.map(o=>o.rssi));
    return `<tr><td>${esc(b.name||'Unnamed device')}</td><td class="m dim">${esc(b.mac)}</td>`+
      `<td class="dim">${esc(b.manufacturer||'—')}</td>`+
      `<td class="m">${b.observations.length}</td><td class="m">${best} dBm</td>`+
      `<td><span class="pill ${b.connectable?'info':'mute'}"><span class="sd"></span>${b.connectable?'yes':'no'}</span></td></tr>`;
  }).join(''):'<tr><td class="empty" colspan="6">No BLE devices observed.</td></tr>';
}
function renderAlerts(){
  const wrap=$('#alerts');
  if(!state.alerts.length){wrap.innerHTML='<div class="empty">No intrusion alerts — fleet nominal.</div>';return;}
  const meta={deauth_flood:['Deauth','crit'],rogue_ap:['Rogue AP','warn'],evil_twin:['Evil twin','warn'],ble_spam_flood:['BLE spam flood','warn']};
  wrap.innerHTML=state.alerts.map(a=>{
    const [sev,cls]=meta[a.alert_type]||[a.alert_type,'warn'];
    let desc;
    if(a.alert_type==='deauth_flood')
      desc=`<b>Deauthentication flood</b> from <span class="mono">${esc(a.source_mac)}</span> — ${a.count} frames/s on channel ${a.channel}`;
    else if(a.alert_type==='evil_twin')
      desc=`<b>Evil-twin access point</b> — SSID “${esc(a.ssid||'—')}” on unexpected BSSID <span class="mono">${esc(a.source_mac)}</span>`;
    else if(a.alert_type==='ble_spam_flood')
      desc=`<b>BLE advertisement flood</b> — ${a.rate} pkts/s spoofing company id <span class="mono">${esc(a.company_id||'—')}</span>`;
    else
      desc=`<b>Rogue access point</b> <span class="mono">${esc(a.source_mac)}</span> on channel ${a.channel}`;
    return `<div class="arow ${cls}"><span class="sev">${sev}</span>`+
      `<div class="desc">${desc}<div class="sub">Detected by ${esc(a.node_id)}</div></div>`+
      `<span class="when">${ago(a.detected_at)}</span></div>`;
  }).join('');
}
function renderAll(){renderStats();renderNodes();renderJobs();renderRF();renderAlerts();}

/* nav */
document.querySelectorAll('.nav').forEach(t=>t.addEventListener('click',()=>{
  document.querySelectorAll('.nav').forEach(x=>x.classList.remove('sel'));
  t.classList.add('sel');
  $('#crumb').textContent=t.dataset.title;
  const v=t.dataset.tab;
  document.querySelectorAll('.view').forEach(x=>x.classList.toggle('hidden',x.dataset.view!==v));
}));
$('#pausebtn').addEventListener('click',function(){
  paused=!paused; this.textContent=paused?'Resume':'Pause';
});
setInterval(()=>{ if(!$('[data-view="rf"]').classList.contains('hidden')) renderRF();
  if(!$('[data-view="security"]').classList.contains('hidden')) renderAlerts(); },5000);

function setConn(isLive){
  live=isLive;
  const c=$('#conn'); c.classList.toggle('on',isLive);
  $('#conn-t').textContent=isLive?'Live':'Demo data';
  $('#conn .sd').className='sd '+(isLive?'ok':'off');
  $('#feed-dot').className='sd '+(isLive?'ok':'iris');
}

/* LIVE */
function goLive(){
  setConn(true);
  const j=(p)=>fetch(p,{credentials:'same-origin'}).then(r=>r.ok?r.json():[]).catch(()=>[]);
  j('/api/nodes').then(ns=>ns.forEach(n=>{state.nodes[n.node_id]=Object.assign(state.nodes[n.node_id]||{},n,{last_seen:Date.parse(n.last_seen)||Date.now()});renderNodes();renderStats();}));
  j('/api/rf/aps').then(a=>{a.forEach(x=>state.aps[x.bssid]=x);renderRF();renderStats();});
  j('/api/rf/ble').then(b=>{b.forEach(x=>state.ble[x.mac]=x);renderRF();renderStats();});
  j('/api/security/alerts').then(al=>{state.alerts=al.map(x=>Object.assign(x,{detected_at:Date.parse(x.detected_at)||Date.now()}));renderAlerts();renderStats();});
  const es=new EventSource('/api/stream');
  es.addEventListener('node_status',e=>{state.frames++;onNodeStatus(JSON.parse(e.data));});
  es.addEventListener('job_event',e=>{state.frames++;onJobEvent(JSON.parse(e.data));});
  es.addEventListener('telemetry',e=>{state.frames++;onTelemetry(JSON.parse(e.data));});
  es.addEventListener('monitor_cycle',e=>{state.frames++;onMonitor(JSON.parse(e.data));});
  setInterval(renderStats,1000);
}

/* DEMO */
function goDemo(){
  setConn(false);
  onNodeStatus({node:'probe-a4c1f8',state:'online',label:'lab-bench',capabilities:['ble_scan','wifi_ids','wifi_survey','port_scan','dns']});
  onNodeStatus({node:'probe-server',state:'online',label:'virtual peer',capabilities:['port_scan','banner','dns','discover','wifi_survey']});
  onNodeStatus({node:'probe-7f21c3',state:'online',label:'roof-north',capabilities:['ble_scan','wifi_ids','wifi_survey']});
  state.aps={
    'a4:c1:38:2b:70:0f':{bssid:'a4:c1:38:2b:70:0f',ssid:'CANDELA-5G',observations:[
      {node:'probe-a4c1f8',rssi:-38,channel:36},{node:'probe-7f21c3',rssi:-61,channel:36}]},
    'e8:94:f6:11:44:71':{bssid:'e8:94:f6:11:44:71',ssid:'TP-LINK_4471',observations:[
      {node:'probe-a4c1f8',rssi:-54,channel:6},{node:'probe-7f21c3',rssi:-49,channel:6}]},
    '3c:84:6a:d2:9a:aa':{bssid:'3c:84:6a:d2:9a:aa',ssid:'',observations:[
      {node:'probe-7f21c3',rssi:-77,channel:11}]}};
  state.ble={
    'ff:22:19:8a:0c:d1':{mac:'ff:22:19:8a:0c:d1',name:'Mi Band 7',manufacturer:'Xiaomi',connectable:true,
      observations:[{node:'probe-a4c1f8',rssi:-52},{node:'probe-7f21c3',rssi:-70}]},
    'd0:03:4b:77:2e:90':{mac:'d0:03:4b:77:2e:90',name:'',manufacturer:'Apple, Inc.',connectable:false,
      observations:[{node:'probe-a4c1f8',rssi:-63}]}};
  renderAll();

  let lastJob=null;
  const baseOJE=onJobEvent;
  onJobEvent=function(d){ if(d.event==='created') lastJob={id:d.job_id,node:d.node,cmd:d.cmd}; return baseOJE(d); };
  const rid=()=>Math.random().toString(16).slice(2,10);
  const bump=(st,ex)=>{ if(lastJob) onJobEvent(Object.assign({job_id:lastJob.id,node:lastJob.node,cmd:lastJob.cmd,event:st,state:st},ex||{})); };

  const script=[
    ()=>onJobEvent({job_id:rid(),node:'probe-server',cmd:'port_scan',event:'created',state:'pending'}),
    ()=>onTelemetry({node:'probe-a4c1f8',free_heap:198000,uptime_s:15547,rssi:-41,channel:36}),
    ()=>bump('accepted'),
    ()=>onJobEvent({job_id:rid(),node:'probe-a4c1f8',cmd:'wifi_survey',event:'created',state:'pending'}),
    ()=>onMonitor({node:'probe-server',results:[{status:'up'},{status:'up'},{status:'down'}]}),
    ()=>bump('done',{chunks:3,duration_ms:412}),
    ()=>onTelemetry({node:'probe-7f21c3',free_heap:71000,uptime_s:6231,rssi:-58,channel:6}),
    ()=>onAlert({node_id:'probe-a4c1f8',alert_type:'deauth_flood',source_mac:'de:ad:be:ef:00:12',channel:6,count:240}),
    ()=>onJobEvent({job_id:rid(),node:'probe-a4c1f8',cmd:'ble_scan',event:'created',state:'pending'}),
    ()=>bump('incomplete',{chunks:2,gaps:[3]}),
    ()=>onAlert({node_id:'probe-7f21c3',alert_type:'evil_twin',source_mac:'02:11:6a:d2:9a:77',ssid:'CANDELA-5G',channel:36}),
    ()=>onTelemetry({node:'probe-a4c1f8',free_heap:191000,uptime_s:15602,rssi:-43,channel:36}),
    ()=>onJobEvent({job_id:rid(),node:'probe-server',cmd:'dns',event:'created',state:'pending'}),
    ()=>bump('done',{chunks:1,duration_ms:88}),
    ()=>onAlert({node_id:'probe-7f21c3',alert_type:'rogue_ap',source_mac:'6a:1f:00:88:31:0c',channel:11}),
  ];
  let i=0;
  (function tick(){ script[i%script.length](); state.frames++; renderStats(); i++;
    setTimeout(tick, 1500+Math.random()*900); })();
  setInterval(()=>{ const n=state.nodes['probe-a4c1f8'];
    if(n&&n.telemetry){n.telemetry.rssi=-38-(Math.random()*8|0);renderNodes();} },4000);
}

/* boot */
(function boot(){
  renderAll();
  if(location.protocol!=='http:'&&location.protocol!=='https:'){ goDemo(); return; }
  // Live only when the API answers with real data; an unauthenticated 401 (or
  // no server) falls back to the self-contained demo so the page is never empty.
  fetch('/api/nodes',{credentials:'same-origin'})
    .then(r=>{ r.ok ? goLive() : goDemo(); })
    .catch(()=>goDemo());
})();
