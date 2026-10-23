import http from "http";
import fs from "fs";
import { execSync } from "child_process";

const PORT = 4000;
const LOGS = { simex: "/tmp/simex.out", "itch-fh": "/tmp/itch-fh.out", trader: "/tmp/trader.out" };

function getProcs() {
  try {
    return execSync('pgrep -a -f "simex|itch-fh|trader" 2>/dev/null || true')
      .toString().split("\n")
      .filter(l => l && !l.includes("bash") && !l.includes("grep") && !l.includes("monitor") && !l.includes("run-continuous"))
      .map(l => { const [pid, ...rest] = l.trim().split(" "); const cmd = rest.join(" ");
        const name = Object.keys(LOGS).find(k => cmd.includes(k)) || "unknown";
        return { pid, name }; });
  } catch { return []; }
}

// Extract only lines after the last SESSION marker
function currentSession(file) {
  try {
    const text = fs.readFileSync(file, "utf8");
    const parts = text.split(/={5,} SESSION \d+[^\n]*\n/);
    return parts[parts.length - 1] || "(waiting for session...)";
  } catch { return "(no output yet)"; }
}

function sessionNumber(file) {
  try {
    const text = fs.readFileSync(file, "utf8");
    const matches = text.match(/SESSION (\d+)/g);
    return matches ? parseInt(matches[matches.length - 1].replace("SESSION ", "")) : 0;
  } catch { return 0; }
}

// Pull key stats from current simex session
function parseStats(simexText, traderText) {
  const s = simexText;
  const t = traderText || "";
  const get = (re, src) => { const m = (src||s).match(re); return m ? m[1] : null; };
  return {
    session:    sessionNumber(LOGS.simex),
    messages:   get(/(\d[\d,]+) messages,/),
    accepted:   get(/(\d[\d,]+) accepted,/),
    executions: get(/(\d+) executions \([\d,]+ shares\)/),
    integrity:  s.includes("0 sides differ") ? "OK" : s.includes("differ") ? "FAIL" : "—",
    digest:     get(/Book digest\s+([0-9a-f]+)/),
    fills:      get(/(\d+) executions \|/, t),
    pnl:        get(/Total\s+[\d\s]+(-?[\d,]+\.\d{2})\s*\(marked/, t),
    symbols:    get(/(\d+) symbols/, t),
  };
}

const HTML = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Trading Lab Monitor</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d1117;color:#c9d1d9;font-family:'Segoe UI',monospace;display:flex;flex-direction:column;height:100vh;overflow:hidden}
header{background:#161b22;border-bottom:1px solid #30363d;padding:10px 20px;display:flex;align-items:center;gap:14px;flex-shrink:0}
header h1{font-size:1rem;color:#58a6ff;white-space:nowrap}
#procs{display:flex;gap:8px;flex-wrap:wrap}
.proc{background:#21262d;border:1px solid #30363d;border-radius:6px;padding:5px 10px;font-size:0.75rem;display:flex;align-items:center;gap:6px}
.proc.alive .dot{background:#3fb950}.proc.dead .dot{background:#f85149}
.dot{width:7px;height:7px;border-radius:50%;flex-shrink:0}
.proc .pid{color:#8b949e}
#stats-bar{background:#161b22;border-bottom:1px solid #30363d;padding:7px 20px;display:flex;gap:24px;flex-shrink:0;flex-wrap:wrap;align-items:center}
.stat{display:flex;flex-direction:column;gap:1px}
.stat-label{font-size:0.65rem;color:#8b949e;text-transform:uppercase;letter-spacing:.04em}
.stat-value{font-size:0.9rem;font-weight:600;color:#79c0ff}
.stat-value.ok{color:#3fb950}.stat-value.fail{color:#f85149}
#ts{margin-left:auto;color:#8b949e;font-size:0.72rem;white-space:nowrap}
main{display:grid;grid-template-columns:repeat(3,1fr);gap:1px;background:#30363d;flex:1;overflow:hidden;min-height:0}
.pane{background:#0d1117;display:flex;flex-direction:column;overflow:hidden;min-height:0}
.pane-header{background:#161b22;padding:7px 14px;font-size:0.8rem;color:#58a6ff;border-bottom:1px solid #30363d;display:flex;justify-content:space-between;align-items:center;flex-shrink:0}
.pane-header span{color:#8b949e;font-size:0.7rem}
pre{flex:1;overflow-y:auto;padding:8px 12px;font-size:0.72rem;line-height:1.5;white-space:pre-wrap;word-break:break-word;min-height:0}
pre::-webkit-scrollbar{width:5px}pre::-webkit-scrollbar-track{background:#161b22}pre::-webkit-scrollbar-thumb{background:#30363d;border-radius:3px}
.ok{color:#3fb950}.fail{color:#f85149}.warn{color:#d29922}.info{color:#58a6ff}.num{color:#e3b341}
</style>
</head>
<body>
<header>
  <h1>Trading Lab</h1>
  <div id="procs"></div>
  <span id="ts"></span>
</header>
<div id="stats-bar">
  <div class="stat"><div class="stat-label">Session</div><div class="stat-value" id="st-session">—</div></div>
  <div class="stat"><div class="stat-label">Messages</div><div class="stat-value" id="st-messages">—</div></div>
  <div class="stat"><div class="stat-label">Accepted</div><div class="stat-value" id="st-accepted">—</div></div>
  <div class="stat"><div class="stat-label">Executions</div><div class="stat-value" id="st-executions">—</div></div>
  <div class="stat"><div class="stat-label">Trader Fills</div><div class="stat-value" id="st-fills">—</div></div>
  <div class="stat"><div class="stat-label">PnL</div><div class="stat-value" id="st-pnl">—</div></div>
  <div class="stat"><div class="stat-label">Integrity</div><div class="stat-value" id="st-integrity">—</div></div>
  <div class="stat"><div class="stat-label">Book Digest</div><div class="stat-value" id="st-digest" style="font-size:0.72rem">—</div></div>
</div>
<main>
  <div class="pane"><div class="pane-header">simex <span id="simex-ct"></span></div><pre id="simex-log"></pre></div>
  <div class="pane"><div class="pane-header">itch-fh <span id="itch-fh-ct"></span></div><pre id="itch-fh-log"></pre></div>
  <div class="pane"><div class="pane-header">trader <span id="trader-ct"></span></div><pre id="trader-log"></pre></div>
</main>
<script>
function colorize(t){
  return t.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;')
    .replace(/\\b(OK|PASS|clean)\\b/g,'<span class="ok">$1</span>')
    .replace(/\\b(FAIL|error|Error)\\b/g,'<span class="fail">$1</span>')
    .replace(/\\b(WARN|WARNING)\\b/g,'<span class="warn">$1</span>')
    .replace(/(\\d[\\.\\d]*\\s*(?:ns|µs|ms|M msgs\\/s|shares))/g,'<span class="num">$1</span>');
}
function set(id,val,cls){const el=document.getElementById(id);if(!el)return;el.textContent=val;if(cls){el.className='stat-value '+cls}}
function refresh(){
  fetch('/api/state').then(r=>r.json()).then(d=>{
    document.getElementById('ts').textContent=new Date().toLocaleTimeString();
    document.getElementById('procs').innerHTML=
      ['simex','itch-fh','trader'].map(n=>{
        const p=d.procs.find(x=>x.name===n);
        return \`<div class="proc \${p?'alive':'dead'}"><div class="dot"></div><b>\${n}</b>\${p?'<span class="pid">'+p.pid+'</span>':''}</div>\`;
      }).join('');
    const s=d.stats;
    set('st-session', s.session||'—');
    set('st-messages', s.messages||'—');
    set('st-accepted', s.accepted||'—');
    set('st-executions', s.executions||'—');
    set('st-fills', s.fills||'—');
    set('st-pnl', s.pnl ? '$'+s.pnl : '—', s.pnl ? (s.pnl.startsWith('-')?'fail':'ok') : '');
    set('st-integrity', s.integrity||'—', s.integrity==='OK'?'ok':s.integrity==='FAIL'?'fail':'');
    set('st-digest', s.digest||'—');
    for(const [name,log] of Object.entries(d.logs)){
      const el=document.getElementById(name+'-log');
      const ct=document.getElementById(name+'-ct');
      if(!el)continue;
      const lines=log.split('\\n');
      if(ct)ct.textContent=lines.length+' lines';
      const atBottom=el.scrollHeight-el.scrollTop-el.clientHeight<80;
      el.innerHTML=colorize(log);
      if(atBottom)el.scrollTop=el.scrollHeight;
    }
  }).catch(()=>{});
}
refresh();
setInterval(refresh,2000);
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  if (req.url === "/api/state") {
    res.writeHead(200, { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" });
    const logs = {};
    for (const [name, file] of Object.entries(LOGS)) logs[name] = currentSession(file);
    const stats = parseStats(logs.simex || "", logs.trader || "");
    res.end(JSON.stringify({ procs: getProcs(), logs, stats }));
    return;
  }
  res.writeHead(200, { "Content-Type": "text/html" });
  res.end(HTML);
});

server.listen(PORT, "0.0.0.0", () => console.log(`Monitor at http://localhost:${PORT}`));
