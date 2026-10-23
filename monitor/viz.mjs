import http from "http";

const PORT = 5000;
const TICK_MS = 600;   // one order event every 600ms
const LEVELS  = 8;     // book depth to show
const SYMBOL  = "SYM0001";

// ─── Order-book simulation ────────────────────────────────────────────────────

let mid = 10004;   // $100.04 in cents
let orderId = 1;
let tradeId = 1;
let sessionFills = 0;
let sessionPnl   = 0;  // cents
let firmPos      = 0;
let lastTradePrice = mid;

const bids = new Map(); // cents -> size
const asks = new Map();

function fmt(cents) { return "$" + (cents / 100).toFixed(2); }
function rndSize() { return (Math.floor(Math.random() * 9) + 1) * 100; }

function initBook() {
  bids.clear(); asks.clear();
  for (let i = 1; i <= 12; i++) {
    bids.set(mid - i, rndSize());
    asks.set(mid + i, rndSize());
  }
}

function bestBid() { return Math.max(...bids.keys()); }
function bestAsk() { return Math.min(...asks.keys()); }

function replenish() {
  // keep at least 10 levels each side
  while (bids.size < 10) {
    const bot = Math.min(...bids.keys()) - 1;
    bids.set(bot, rndSize());
  }
  while (asks.size < 10) {
    const top = Math.max(...asks.keys()) + 1;
    asks.set(top, rndSize());
  }
  // trim deep levels
  [...bids.keys()].sort((a,b)=>a-b).slice(0, Math.max(0, bids.size-14)).forEach(p=>bids.delete(p));
  [...asks.keys()].sort((a,b)=>b-a).slice(0, Math.max(0, asks.size-14)).forEach(p=>asks.delete(p));
}

function bookSnapshot() {
  const bb = bestBid(), ba = bestAsk();
  const bidArr = [...bids.entries()].sort((a,b)=>b[0]-a[0]).slice(0,LEVELS).map(([p,s])=>({p,s}));
  const askArr = [...asks.entries()].sort((a,b)=>a[0]-b[0]).slice(0,LEVELS).map(([p,s])=>({p,s}));
  return { bids: bidArr, asks: askArr, bb, ba, spread: ba-bb, mid: Math.round((bb+ba)/2) };
}

// ─── Event queue for SSE clients ─────────────────────────────────────────────

const clients = new Set();
function push(event, data) {
  const msg = `event:${event}\ndata:${JSON.stringify(data)}\n\n`;
  for (const c of clients) c.write(msg);
}

// ─── Simulation tick ──────────────────────────────────────────────────────────

function tick() {
  const bb = bestBid(), ba = bestAsk();
  const side = Math.random() > 0.5 ? "buy" : "sell";
  const aggressive = Math.random() < 0.55;
  const qty = rndSize();

  let price, orderType;
  if (aggressive) {
    price = side === "buy" ? ba : bb;   // cross spread
    orderType = "MKT";
  } else {
    // passive: join best or one tick inside
    const inside = Math.random() < 0.4;
    price = side === "buy"
      ? (inside ? bb + 1 : bb)
      : (inside ? ba - 1 : ba);
    orderType = "LMT";
  }

  const oid = orderId++;

  // ① Firm submits order
  push("order", {
    oid, side, qty, price: fmt(price), type: orderType,
    aggr: aggressive, ts: new Date().toLocaleTimeString("en-GB", {hour12:false})
  });

  // ② Exchange processes
  let fills = [];
  if (aggressive) {
    const oppMap = side === "buy" ? asks : bids;
    const prices = side === "buy"
      ? [...oppMap.keys()].sort((a,b)=>a-b)
      : [...oppMap.keys()].sort((a,b)=>b-a);

    let rem = qty;
    for (const p of prices) {
      if (rem <= 0) break;
      if (side === "buy"  && p > price) break;
      if (side === "sell" && p < price) break;
      const avail = oppMap.get(p);
      const f = Math.min(rem, avail);
      fills.push({ price: p, qty: f });
      rem -= f;
      if (avail - f <= 0) oppMap.delete(p);
      else oppMap.set(p, avail - f);
    }

    if (fills.length) {
      const avgPx = fills.reduce((s,f)=>s+f.price*f.qty,0) / fills.reduce((s,f)=>s+f.qty,0);
      lastTradePrice = Math.round(avgPx);
      sessionFills += fills.reduce((s,f)=>s+f.qty,0);
      // PnL: sell fills are revenue, buy fills are cost; very rough mark-to-mid
      if (side === "buy")  { firmPos += fills.reduce((s,f)=>s+f.qty,0); sessionPnl -= fills.reduce((s,f)=>s+f.price*f.qty,0); }
      if (side === "sell") { firmPos -= fills.reduce((s,f)=>s+f.qty,0); sessionPnl += fills.reduce((s,f)=>s+f.price*f.qty,0); }
      mid = lastTradePrice;
    }
  } else {
    // Passive: add to book
    const map = side === "buy" ? bids : asks;
    map.set(price, (map.get(price) || 0) + qty);
  }

  replenish();

  // ③ Push fills + book
  if (fills.length) {
    for (const f of fills) {
      push("fill", {
        tid: tradeId++, oid, side, qty: f.qty,
        price: fmt(f.price),
        ts: new Date().toLocaleTimeString("en-GB", {hour12:false})
      });
    }
  } else if (!aggressive) {
    push("rest", { oid, side, qty, price: fmt(price) });
  }

  // Approximate PnL mark-to-mid
  const markPnl = (sessionPnl + firmPos * mid) / 100;

  push("book", {
    ...bookSnapshot(),
    lastTrade: fmt(lastTradePrice),
    sessionFills,
    pnl: markPnl.toFixed(2),
    firmPos
  });
}

initBook();
setInterval(tick, TICK_MS);

// ─── HTTP server ──────────────────────────────────────────────────────────────

const HTML = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Trading Lab — Live Simulation</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#0a0e14;--panel:#0d1219;--border:#1e2733;--muted:#4a5568;
  --bid:#22c55e;--ask:#ef4444;--fill:#f59e0b;--blue:#60a5fa;--text:#e2e8f0;
  --flash-bid:rgba(34,197,94,.25);--flash-ask:rgba(239,68,68,.25);
}
body{background:var(--bg);color:var(--text);font-family:'Cascadia Code','Fira Code',monospace;
  height:100vh;display:flex;flex-direction:column;overflow:hidden;font-size:13px}

/* ── header ── */
header{background:#070b10;border-bottom:1px solid var(--border);padding:10px 20px;
  display:flex;align-items:center;gap:20px;flex-shrink:0}
header .sym{font-size:1.1rem;font-weight:700;color:var(--blue);letter-spacing:.05em}
header .mid{font-size:1.3rem;font-weight:700;color:var(--text)}
.badge{background:var(--panel);border:1px solid var(--border);border-radius:4px;
  padding:3px 10px;display:flex;flex-direction:column;align-items:center;gap:1px}
.badge .lbl{font-size:.6rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}
.badge .val{font-size:.85rem;font-weight:600}
.badge .val.pos{color:var(--bid)}.badge .val.neg{color:var(--ask)}.badge .val.neu{color:var(--blue)}
.dot{width:7px;height:7px;border-radius:50%;background:var(--bid);
  animation:pulse 1.5s infinite;margin-left:auto}
@keyframes pulse{0%,100%{opacity:1;transform:scale(1)}50%{opacity:.5;transform:scale(.8)}}

/* ── main grid ── */
main{flex:1;display:grid;grid-template-columns:1fr 340px 1fr;gap:1px;
  background:var(--border);overflow:hidden;min-height:0}
.panel{background:var(--panel);display:flex;flex-direction:column;overflow:hidden;min-height:0}
.panel-hdr{background:#070b10;border-bottom:1px solid var(--border);padding:7px 14px;
  font-size:.7rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);flex-shrink:0;
  display:flex;align-items:center;gap:8px}
.panel-hdr .icon{font-size:.85rem}

/* ── order flow (left) ── */
#flow-list{flex:1;overflow-y:auto;padding:8px;display:flex;flex-direction:column;gap:4px;min-height:0}
#flow-list::-webkit-scrollbar{width:4px}#flow-list::-webkit-scrollbar-thumb{background:var(--border)}
.flow-item{border-radius:5px;padding:6px 10px;border-left:3px solid;
  animation:slideIn .3s ease-out;font-size:.78rem;line-height:1.6}
@keyframes slideIn{from{opacity:0;transform:translateX(-16px)}to{opacity:1;transform:none}}
.flow-item.buy{border-color:var(--bid);background:rgba(34,197,94,.07)}
.flow-item.sell{border-color:var(--ask);background:rgba(239,68,68,.07)}
.flow-item .row1{display:flex;justify-content:space-between;align-items:center}
.flow-item .side-lbl{font-weight:700;font-size:.8rem}
.flow-item.buy .side-lbl{color:var(--bid)}.flow-item.sell .side-lbl{color:var(--ask)}
.flow-item .arrow{color:var(--muted);font-size:.7rem}
.flow-item .ts{color:var(--muted);font-size:.65rem}
.flow-item .row2{color:var(--muted);font-size:.72rem;margin-top:1px}
.fill-badge{display:inline-block;background:rgba(245,158,11,.15);color:var(--fill);
  border:1px solid rgba(245,158,11,.4);border-radius:3px;
  padding:1px 6px;font-size:.67rem;margin-top:3px;animation:flashFill .4s ease}
.rest-badge{display:inline-block;background:rgba(96,165,250,.08);color:var(--blue);
  border:1px solid rgba(96,165,250,.3);border-radius:3px;padding:1px 6px;font-size:.67rem;margin-top:3px}
@keyframes flashFill{0%{background:rgba(245,158,11,.5)}100%{background:rgba(245,158,11,.15)}}

/* ── order book (center) ── */
#book{flex:1;display:flex;flex-direction:column;justify-content:center;padding:4px 0;min-height:0}
.book-section{display:flex;flex-direction:column;padding:0 10px;gap:2px}
.book-row{display:grid;grid-template-columns:68px 60px 1fr;gap:6px;align-items:center;
  padding:3px 6px;border-radius:3px;transition:background .2s}
.book-row.ask{direction:ltr}
.book-row.bid{direction:ltr}
.book-row .price{font-weight:600;font-size:.82rem;font-variant-numeric:tabular-nums}
.book-row.ask .price{color:var(--ask)}.book-row.bid .price{color:var(--bid)}
.book-row .size{text-align:right;color:var(--text);font-size:.78rem;font-variant-numeric:tabular-nums}
.bar-wrap{height:14px;display:flex;align-items:center}
.bar{height:10px;border-radius:2px;min-width:2px;transition:width .25s ease}
.book-row.ask .bar{background:rgba(239,68,68,.35);float:left}
.book-row.bid .bar{background:rgba(34,197,94,.35);float:left}
.book-row.flash-ask{background:var(--flash-ask);animation:rowFlash .5s ease}
.book-row.flash-bid{background:var(--flash-bid);animation:rowFlash .5s ease}
@keyframes rowFlash{0%{background:rgba(239,68,68,.45)}100%{background:transparent}}

.spread-row{display:flex;align-items:center;justify-content:center;gap:10px;
  padding:6px 10px;margin:4px 10px;border-top:1px dashed var(--border);
  border-bottom:1px dashed var(--border);color:var(--muted);font-size:.72rem}
.spread-row .last{color:var(--fill);font-weight:600;font-size:.85rem}
.spread-row .sp{color:var(--muted)}

/* ── trade tape (right) ── */
#tape-list{flex:1;overflow-y:auto;padding:8px;display:flex;flex-direction:column;gap:4px;min-height:0}
#tape-list::-webkit-scrollbar{width:4px}#tape-list::-webkit-scrollbar-thumb{background:var(--border)}
.tape-item{display:grid;grid-template-columns:60px 1fr 70px 60px;
  gap:4px;align-items:center;padding:5px 8px;border-radius:4px;
  animation:fadeIn .3s ease-out;font-size:.76rem;font-variant-numeric:tabular-nums}
@keyframes fadeIn{from{opacity:0;transform:translateY(8px)}to{opacity:1;transform:none}}
.tape-item.buy{background:rgba(34,197,94,.07);border-left:2px solid var(--bid)}
.tape-item.sell{background:rgba(239,68,68,.07);border-left:2px solid var(--ask)}
.tape-item .t-ts{color:var(--muted);font-size:.65rem}
.tape-item .t-side{font-weight:700}
.tape-item.buy .t-side{color:var(--bid)}.tape-item.sell .t-side{color:var(--ask)}
.tape-item .t-price{color:var(--text);font-weight:600}
.tape-item .t-qty{color:var(--muted);text-align:right}
.pos-box{margin:8px;padding:10px;background:rgba(255,255,255,.03);border:1px solid var(--border);
  border-radius:6px;flex-shrink:0}
.pos-box .row{display:flex;justify-content:space-between;padding:2px 0;font-size:.78rem}
.pos-box .row .k{color:var(--muted)}.pos-box .row .v{font-weight:600;font-variant-numeric:tabular-nums}
.pos-box .row .v.pos{color:var(--bid)}.pos-box .row .v.neg{color:var(--ask)}.pos-box .row .v.neu{color:var(--blue)}
</style>
</head>
<body>
<header>
  <span class="sym">${SYMBOL}</span>
  <span class="mid" id="h-mid">$100.04</span>
  <div class="badge"><span class="lbl">Spread</span><span class="val neu" id="h-spread">2¢</span></div>
  <div class="badge"><span class="lbl">Last Trade</span><span class="val" id="h-last">—</span></div>
  <div class="badge"><span class="lbl">Session Fills</span><span class="val neu" id="h-fills">0 sh</span></div>
  <div class="badge"><span class="lbl">Firm PnL</span><span class="val" id="h-pnl">$0.00</span></div>
  <div class="badge"><span class="lbl">Position</span><span class="val" id="h-pos">0</span></div>
  <div class="dot" title="live"></div>
</header>
<main>
  <!-- LEFT: order flow -->
  <div class="panel">
    <div class="panel-hdr"><span class="icon">⟶</span> FIRM01 → EXCHANGE</div>
    <div id="flow-list"></div>
  </div>

  <!-- CENTER: order book -->
  <div class="panel">
    <div class="panel-hdr" style="justify-content:center">ORDER BOOK</div>
    <div id="book">
      <div class="book-section" id="asks-section"></div>
      <div class="spread-row">
        <span class="sp">spread</span>
        <span class="last" id="spread-last">—</span>
        <span class="sp" id="spread-val">—</span>
      </div>
      <div class="book-section" id="bids-section"></div>
    </div>
  </div>

  <!-- RIGHT: tape + position -->
  <div class="panel">
    <div class="panel-hdr"><span class="icon">⟵</span> EXCHANGE → FIRM01</div>
    <div id="tape-list"></div>
    <div class="pos-box">
      <div class="row"><span class="k">Position</span><span class="val neu" id="p-pos">0 sh</span></div>
      <div class="row"><span class="k">Last Trade</span><span class="val" id="p-last">—</span></div>
      <div class="row"><span class="k">Session Fills</span><span class="val neu" id="p-fills">0 sh</span></div>
      <div class="row"><span class="k">Mark PnL</span><span class="val" id="p-pnl">$0.00</span></div>
    </div>
  </div>
</main>

<script>
const MAX_FLOW = 40, MAX_TAPE = 60;
const flowPending = new Map(); // oid -> element

function prependCapped(container, el, max) {
  container.prepend(el);
  while (container.children.length > max) container.removeChild(container.lastChild);
}

function pnlClass(v) { return parseFloat(v) >= 0 ? 'pos' : 'neg'; }
function posClass(v) { return v > 0 ? 'pos' : v < 0 ? 'neg' : 'neu'; }

// ── order event ──
function onOrder(d) {
  const el = document.createElement('div');
  el.className = 'flow-item ' + d.side;
  el.dataset.oid = d.oid;
  el.innerHTML =
    '<div class="row1">' +
      '<span class="side-lbl">' + d.side.toUpperCase() + '</span>' +
      '<span>' + d.qty.toLocaleString() + ' @ ' + d.price + '</span>' +
      '<span class="arrow">→ EXCH</span>' +
    '</div>' +
    '<div class="row2">' + d.type + ' &nbsp;·&nbsp; <span class="ts">' + d.ts + '</span></div>';
  flowPending.set(d.oid, el);
  prependCapped(document.getElementById('flow-list'), el, MAX_FLOW);
}

// ── fill event ──
function onFill(d) {
  // Update flow item
  const fl = flowPending.get(d.oid);
  if (fl) {
    const badge = document.createElement('div');
    badge.className = 'fill-badge';
    badge.textContent = '← FILLED ' + d.qty.toLocaleString() + ' @ ' + d.price;
    fl.appendChild(badge);
    fl.querySelector('.arrow').textContent = '✓ FILLED';
  }
  // Add to tape
  const te = document.createElement('div');
  te.className = 'tape-item ' + d.side;
  te.innerHTML =
    '<span class="t-ts">' + d.ts + '</span>' +
    '<span class="t-side">' + d.side.toUpperCase() + '</span>' +
    '<span class="t-price">' + d.price + '</span>' +
    '<span class="t-qty">' + d.qty.toLocaleString() + ' sh</span>';
  prependCapped(document.getElementById('tape-list'), te, MAX_TAPE);
}

// ── rest event ──
function onRest(d) {
  const fl = flowPending.get(d.oid);
  if (fl) {
    const badge = document.createElement('div');
    badge.className = 'rest-badge';
    badge.textContent = '⋯ RESTING on book';
    fl.appendChild(badge);
    fl.querySelector('.arrow').textContent = '⋯ RESTING';
  }
}

// ── book event ──
let prevBids = {}, prevAsks = {};
function onBook(d) {
  // Header
  document.getElementById('h-mid').textContent = '$' + (d.mid/100).toFixed(2);
  document.getElementById('h-spread').textContent = d.spread + '¢';
  document.getElementById('h-last').textContent = d.lastTrade;
  document.getElementById('h-fills').textContent = d.sessionFills.toLocaleString() + ' sh';
  const pnlEl = document.getElementById('h-pnl');
  pnlEl.textContent = '$' + d.pnl;
  pnlEl.className = 'val ' + pnlClass(d.pnl);
  const posEl = document.getElementById('h-pos');
  posEl.textContent = (d.firmPos > 0 ? '+' : '') + d.firmPos;
  posEl.className = 'val ' + posClass(d.firmPos);
  // Spread row
  document.getElementById('spread-last').textContent = d.lastTrade;
  document.getElementById('spread-val').textContent = 'spread ' + d.spread + '¢';
  // Position box
  document.getElementById('p-pos').textContent = (d.firmPos > 0?'+':'') + d.firmPos + ' sh';
  document.getElementById('p-pos').className = 'val ' + posClass(d.firmPos);
  document.getElementById('p-last').textContent = d.lastTrade;
  document.getElementById('p-fills').textContent = d.sessionFills.toLocaleString() + ' sh';
  const ppnl = document.getElementById('p-pnl');
  ppnl.textContent = '$' + d.pnl;
  ppnl.className = 'val ' + pnlClass(d.pnl);

  // Max size for bar scaling
  const allSizes = [...d.bids.map(x=>x.s), ...d.asks.map(x=>x.s)];
  const maxSize = Math.max(...allSizes, 1);

  // Render asks (reversed so best ask nearest spread)
  const asksEl = document.getElementById('asks-section');
  asksEl.innerHTML = '';
  const asksSorted = [...d.asks].sort((a,b) => b.p - a.p); // highest first
  for (const {p, s} of asksSorted) {
    const changed = prevAsks[p] !== undefined && prevAsks[p] !== s;
    const row = document.createElement('div');
    row.className = 'book-row ask' + (changed ? ' flash-ask' : '');
    const barW = Math.round((s / maxSize) * 120);
    row.innerHTML =
      '<span class="price">' + (p/100).toFixed(2) + '</span>' +
      '<span class="size">' + s.toLocaleString() + '</span>' +
      '<div class="bar-wrap"><div class="bar" style="width:' + barW + 'px"></div></div>';
    asksEl.appendChild(row);
  }

  // Render bids
  const bidsEl = document.getElementById('bids-section');
  bidsEl.innerHTML = '';
  for (const {p, s} of d.bids) {
    const changed = prevBids[p] !== undefined && prevBids[p] !== s;
    const row = document.createElement('div');
    row.className = 'book-row bid' + (changed ? ' flash-bid' : '');
    const barW = Math.round((s / maxSize) * 120);
    row.innerHTML =
      '<span class="price">' + (p/100).toFixed(2) + '</span>' +
      '<span class="size">' + s.toLocaleString() + '</span>' +
      '<div class="bar-wrap"><div class="bar" style="width:' + barW + 'px"></div></div>';
    bidsEl.appendChild(row);
  }

  // Save for next diff
  prevBids = {}; d.bids.forEach(({p,s}) => prevBids[p] = s);
  prevAsks = {}; d.asks.forEach(({p,s}) => prevAsks[p] = s);
}

// ── SSE ──
const es = new EventSource('/stream');
es.addEventListener('order', e => onOrder(JSON.parse(e.data)));
es.addEventListener('fill',  e => onFill(JSON.parse(e.data)));
es.addEventListener('rest',  e => onRest(JSON.parse(e.data)));
es.addEventListener('book',  e => onBook(JSON.parse(e.data)));
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  if (req.url === "/stream") {
    res.writeHead(200, {
      "Content-Type": "text/event-stream",
      "Cache-Control": "no-cache",
      "Access-Control-Allow-Origin": "*",
      Connection: "keep-alive",
    });
    res.write("retry:1000\n\n");
    clients.add(res);
    req.on("close", () => clients.delete(res));
    return;
  }
  res.writeHead(200, { "Content-Type": "text/html" });
  res.end(HTML);
});

server.listen(PORT, "0.0.0.0", () =>
  console.log(`Live simulation at http://localhost:${PORT}`)
);
