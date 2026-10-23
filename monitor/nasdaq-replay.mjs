/**
 * Nasdaq TotalView-ITCH 5.0 order-book replay visualiser
 * Port 6000  —  exchange-side only, no firm / order-entry
 *
 * Reads any ITCH 5.0 BinaryFILE (the same format as the real Nasdaq sample
 * files at https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/).
 * Supports both uncompressed and .gz files.
 *
 * Usage:
 *   node nasdaq-replay.mjs [FILE]
 *   FILE defaults to ~/data/itch/synthetic.bin
 *   Real data: node nasdaq-replay.mjs ~/data/itch/01302019.NASDAQ_ITCH50.gz
 */

import http    from "http";
import fs      from "fs";
import zlib    from "zlib";
import path    from "path";
import os      from "os";

const PORT      = 6000;
const FILE      = process.argv[2] ?? path.join(os.homedir(), "data/itch/synthetic.bin");
const BATCH     = 800;   // messages processed per tick
const TICK_MS   = 25;    // ms between batches → full day loops in ~30–60 s
const PUSH_MS   = 150;   // ms between SSE pushes
const DEPTH     = 8;     // book levels to show

// ── ITCH 5.0 BinaryFILE reader ────────────────────────────────────────────────
// Format: [2-byte big-endian length][message bytes] …  (optionally gzipped)

async function loadMessages(filePath) {
  const isGz = filePath.endsWith(".gz");
  const raw  = fs.createReadStream(filePath);
  const src  = isGz ? raw.pipe(zlib.createGunzip()) : raw;

  const msgs = [];
  let buf = Buffer.alloc(0);

  for await (const chunk of src) {
    buf = Buffer.concat([buf, chunk]);
    let off = 0;
    while (off + 2 <= buf.length) {
      const len = buf.readUInt16BE(off);
      if (buf.length < off + 2 + len) break;
      msgs.push(Buffer.from(buf.subarray(off + 2, off + 2 + len)));
      off += 2 + len;
    }
    buf = buf.subarray(off);
  }
  return msgs;
}

// 6-byte big-endian timestamp (ns since midnight)
function readUint48BE(buf, off) {
  return buf.readUInt32BE(off) * 65536 + buf.readUInt16BE(off + 4);
}

function parseMsg(buf) {
  const type = String.fromCharCode(buf[0]);
  const loc  = buf.readUInt16BE(1);
  const ts   = readUint48BE(buf, 5);

  switch (type) {
    case "R": return { type, loc, ts, sym: buf.subarray(11,19).toString("ascii").trimEnd() };
    case "A":
    case "F": return { type:"A", loc, ts,
        ref:    buf.readBigUInt64BE(11),
        side:   String.fromCharCode(buf[19]),
        shares: buf.readUInt32BE(20),
        price:  buf.readUInt32BE(32) };
    case "E": return { type, loc, ts,
        ref:    buf.readBigUInt64BE(11),
        exec:   buf.readUInt32BE(19) };
    case "C": return { type, loc, ts,
        ref:    buf.readBigUInt64BE(11),
        exec:   buf.readUInt32BE(19),
        price:  buf.readUInt32BE(32) };
    case "X": return { type, loc, ts,
        ref:    buf.readBigUInt64BE(11),
        cancel: buf.readUInt32BE(19) };
    case "D": return { type, loc, ts, ref: buf.readBigUInt64BE(11) };
    case "U": return { type, loc, ts,
        origRef: buf.readBigUInt64BE(11),
        newRef:  buf.readBigUInt64BE(19),
        shares:  buf.readUInt32BE(27),
        price:   buf.readUInt32BE(31) };
    case "P": return { type, loc, ts,
        shares: buf.readUInt32BE(20),
        price:  buf.readUInt32BE(32) };
    default:  return { type, loc, ts };
  }
}

// ── order-book state ──────────────────────────────────────────────────────────

const symByLoc = new Map();    // loc  → symbol string
const locBySym = new Map();    // sym  → loc
const orders   = new Map();    // BigInt ref → {loc, side, shares, price}
const bookMap  = new Map();    // loc  → {bids: Map<price,size>, asks: Map<price,size>}
const stats    = new Map();    // loc  → {vol,turnover,hi,lo,lastPx,lastTs,addCt,exeCt,canCt}

function getBook(loc) {
  if (!bookMap.has(loc)) bookMap.set(loc, { bids: new Map(), asks: new Map() });
  return bookMap.get(loc);
}
function getStat(loc) {
  if (!stats.has(loc)) stats.set(loc, { vol:0, turnover:0, hi:0, lo:Infinity, lastPx:0, lastTs:0, addCt:0, exeCt:0, canCt:0 });
  return stats.get(loc);
}

// Accumulated events for the event-stream panel (last N per symbol)
const eventBuf = new Map(); // loc → [{tag,side,price,shares,ts}]
function pushEvt(loc, evt) {
  if (!eventBuf.has(loc)) eventBuf.set(loc, []);
  const q = eventBuf.get(loc);
  q.push(evt);
  if (q.length > 60) q.shift();
}

function processMsg(m) {
  switch (m.type) {
    case "R":
      symByLoc.set(m.loc, m.sym);
      locBySym.set(m.sym, m.loc);
      getStat(m.loc);
      break;

    case "A": {
      orders.set(m.ref, { loc: m.loc, side: m.side, shares: m.shares, price: m.price });
      const b = getBook(m.loc);
      const map = m.side === "B" ? b.bids : b.asks;
      map.set(m.price, (map.get(m.price) || 0) + m.shares);
      getStat(m.loc).addCt++;
      pushEvt(m.loc, { tag:"ADD", side: m.side==="B"?"BUY":"SELL", price: m.price, shares: m.shares, ts: m.ts });
      break;
    }

    case "E":
    case "C": {
      const ord = orders.get(m.ref);
      if (!ord) break;
      const execPx = m.type === "C" ? m.price : ord.price;
      const b = getBook(ord.loc);
      const map = ord.side === "B" ? b.bids : b.asks;
      const lvl = map.get(ord.price);
      if (lvl !== undefined) {
        const nxt = lvl - m.exec;
        if (nxt <= 0) map.delete(ord.price); else map.set(ord.price, nxt);
      }
      ord.shares -= m.exec;
      if (ord.shares <= 0) orders.delete(m.ref);
      const st = getStat(ord.loc);
      st.vol      += m.exec;
      st.turnover += m.exec * execPx;
      st.lastPx    = execPx;
      st.lastTs    = m.ts;
      st.exeCt++;
      if (execPx > st.hi) st.hi = execPx;
      if (execPx < st.lo) st.lo = execPx;
      pushEvt(ord.loc, { tag:"EXEC", side: ord.side==="B"?"BUY":"SELL", price: execPx, shares: m.exec, ts: m.ts });
      break;
    }

    case "X": {
      const ord = orders.get(m.ref);
      if (!ord) break;
      const b = getBook(ord.loc);
      const map = ord.side === "B" ? b.bids : b.asks;
      const lvl = map.get(ord.price);
      if (lvl !== undefined) {
        const nxt = lvl - m.cancel;
        if (nxt <= 0) map.delete(ord.price); else map.set(ord.price, nxt);
      }
      ord.shares -= m.cancel;
      if (ord.shares <= 0) orders.delete(m.ref);
      getStat(ord.loc).canCt++;
      pushEvt(ord.loc, { tag:"CXCL", side: ord.side==="B"?"BUY":"SELL", price: ord.price, shares: m.cancel, ts: m.ts });
      break;
    }

    case "D": {
      const ord = orders.get(m.ref);
      if (!ord) break;
      const b = getBook(ord.loc);
      const map = ord.side === "B" ? b.bids : b.asks;
      const lvl = map.get(ord.price);
      if (lvl !== undefined) {
        const nxt = lvl - ord.shares;
        if (nxt <= 0) map.delete(ord.price); else map.set(ord.price, nxt);
      }
      orders.delete(m.ref);
      getStat(ord.loc).canCt++;
      break;
    }

    case "U": {
      const orig = orders.get(m.origRef);
      if (!orig) break;
      // remove from old level
      const b = getBook(orig.loc);
      const map = orig.side === "B" ? b.bids : b.asks;
      const lvl = map.get(orig.price);
      if (lvl !== undefined) {
        const nxt = lvl - orig.shares;
        if (nxt <= 0) map.delete(orig.price); else map.set(orig.price, nxt);
      }
      orders.delete(m.origRef);
      // add at new level
      orders.set(m.newRef, { loc: orig.loc, side: orig.side, shares: m.shares, price: m.price });
      map.set(m.price, (map.get(m.price) || 0) + m.shares);
      break;
    }
  }
}

function resetState() {
  orders.clear();
  bookMap.clear();
  stats.clear();
  eventBuf.clear();
  // keep symByLoc / locBySym (populated by R messages at file start)
  symByLoc.clear();
  locBySym.clear();
}

// ── snapshot helpers ──────────────────────────────────────────────────────────

function fmtPx(cents) { return (cents / 10000).toFixed(4); }
function fmtTime(ns) {
  const s = Math.floor(ns / 1e9);
  const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60), sec = s % 60;
  return `${String(h).padStart(2,"0")}:${String(m).padStart(2,"0")}:${String(sec).padStart(2,"0")}`;
}

function bookSnap(loc) {
  const b = bookMap.get(loc);
  if (!b) return { bids:[], asks:[], bb:0, ba:0 };
  const bids = [...b.bids.entries()].sort((a,b)=>b[0]-a[0]).slice(0, DEPTH).map(([p,s])=>({p,s}));
  const asks = [...b.asks.entries()].sort((a,b)=>a[0]-b[0]).slice(0, DEPTH).map(([p,s])=>({p,s}));
  const bb = bids[0]?.p ?? 0, ba = asks[0]?.p ?? 0;
  return { bids, asks, bb, ba, spread: ba && bb ? ba - bb : 0 };
}

function statSnap(loc) {
  const st = stats.get(loc) ?? {};
  return {
    vol:   st.vol ?? 0,
    vwap:  st.vol ? fmtPx(st.turnover / st.vol) : "—",
    hi:    st.hi  ? fmtPx(st.hi)  : "—",
    lo:    st.lo !== Infinity ? fmtPx(st.lo) : "—",
    last:  st.lastPx ? fmtPx(st.lastPx) : "—",
    lastTs: st.lastTs ? fmtTime(st.lastTs) : "—",
    addCt: st.addCt ?? 0,
    exeCt: st.exeCt ?? 0,
    canCt: st.canCt ?? 0,
  };
}

function allSymbols() {
  // return symbols sorted by execution count desc
  return [...symByLoc.entries()]
    .map(([loc, sym]) => ({ sym, loc, exeCt: stats.get(loc)?.exeCt ?? 0 }))
    .sort((a,b) => b.exeCt - a.exeCt)
    .map(x => x.sym);
}

// ── SSE clients + push ────────────────────────────────────────────────────────

const clients = new Set();
let activeSym = null;

function push(event, data) {
  const msg = `event:${event}\ndata:${JSON.stringify(data)}\n\n`;
  for (const c of clients) c.write(msg);
}

let lastEvtIdx = new Map(); // loc -> last pushed index

function doPush() {
  if (!activeSym) {
    // pick most active
    const syms = allSymbols();
    if (syms.length) activeSym = syms[0];
    else return;
  }

  const loc = locBySym.get(activeSym);
  if (loc === undefined) return;

  const snap = bookSnap(loc);
  const st   = statSnap(loc);
  const evts = eventBuf.get(loc) ?? [];

  // only send new events since last push
  const lastIdx = lastEvtIdx.get(loc) ?? 0;
  const newEvts = evts.slice(lastIdx).map(e => ({
    ...e, price: fmtPx(e.price), ts: fmtTime(e.ts)
  }));
  lastEvtIdx.set(loc, evts.length);

  push("frame", {
    sym:    activeSym,
    syms:   allSymbols().slice(0, 20),
    book:   snap,
    stat:   st,
    events: newEvts,
  });
}

// ── replay loop ───────────────────────────────────────────────────────────────

let messages = [];
let msgIdx   = 0;

async function init() {
  console.log(`Loading ${FILE}...`);
  messages = await loadMessages(FILE);
  console.log(`Loaded ${messages.length.toLocaleString()} messages. Starting replay.`);
  scheduleNext();
}

function scheduleNext() {
  setTimeout(tick, TICK_MS);
}

function tick() {
  const end = Math.min(msgIdx + BATCH, messages.length);
  for (; msgIdx < end; msgIdx++) {
    try { processMsg(parseMsg(messages[msgIdx])); } catch { /* skip bad msg */ }
  }
  if (msgIdx >= messages.length) {
    console.log("Loop: resetting state for next pass");
    resetState();
    msgIdx = 0;
    lastEvtIdx.clear();
    activeSym = null;
  }
  scheduleNext();
}

setInterval(doPush, PUSH_MS);

// ── HTTP server ───────────────────────────────────────────────────────────────

const HTML = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Nasdaq ITCH Replay</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#07090d;--panel:#0b0f15;--border:#1a2030;--muted:#3d5068;
  --bid:#22d45e;--ask:#ff4d4d;--exec:#f59e0b;--blue:#4da6ff;--text:#dce8f0;
  --cancel:#6b7a8d;
}
body{background:var(--bg);color:var(--text);font-family:'Cascadia Code','Fira Code','Consolas',monospace;
  height:100vh;display:flex;flex-direction:column;overflow:hidden;font-size:12px}

/* header */
header{background:#050709;border-bottom:1px solid var(--border);padding:8px 18px;
  display:flex;align-items:center;gap:14px;flex-shrink:0;flex-wrap:wrap}
.logo{font-size:.95rem;font-weight:700;color:var(--blue);letter-spacing:.06em;white-space:nowrap}
select#sym-sel{background:#0f1620;color:var(--text);border:1px solid var(--border);
  border-radius:4px;padding:4px 8px;font-size:.8rem;font-family:inherit;cursor:pointer}
select#sym-sel:focus{outline:none;border-color:var(--blue)}
.stat-pill{background:#0f1620;border:1px solid var(--border);border-radius:4px;
  padding:3px 10px;display:flex;flex-direction:column;align-items:center;gap:0}
.stat-pill .lbl{font-size:.58rem;color:var(--muted);text-transform:uppercase;letter-spacing:.07em}
.stat-pill .val{font-size:.8rem;font-weight:600}
.val.bid{color:var(--bid)}.val.ask{color:var(--ask)}.val.exec{color:var(--exec)}.val.blue{color:var(--blue)}
.live-dot{width:6px;height:6px;border-radius:50%;background:var(--bid);
  animation:pulse 1.4s infinite;margin-left:auto;flex-shrink:0}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.3}}

/* counters bar */
#ctr-bar{background:#080b11;border-bottom:1px solid var(--border);padding:5px 18px;
  display:flex;gap:18px;font-size:.7rem;color:var(--muted);flex-shrink:0}
#ctr-bar span{color:var(--text)}

/* main grid */
main{flex:1;display:grid;grid-template-columns:260px 1fr 260px;gap:1px;
  background:var(--border);overflow:hidden;min-height:0}
.pane{background:var(--panel);display:flex;flex-direction:column;overflow:hidden;min-height:0}
.pane-hdr{background:#050709;border-bottom:1px solid var(--border);padding:6px 12px;
  font-size:.65rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted);flex-shrink:0}

/* event stream (left) */
#evt-list{flex:1;overflow-y:auto;padding:6px;display:flex;flex-direction:column;gap:3px;min-height:0}
#evt-list::-webkit-scrollbar{width:3px}#evt-list::-webkit-scrollbar-thumb{background:var(--border)}
.evt{border-radius:3px;padding:4px 8px;font-size:.72rem;animation:fadeIn .2s ease;
  display:grid;grid-template-columns:32px 38px 80px 60px 1fr;gap:4px;align-items:center}
@keyframes fadeIn{from{opacity:0;transform:translateX(-8px)}to{opacity:1;transform:none}}
.evt.ADD.BUY {background:rgba(34,212,94,.06);border-left:2px solid var(--bid)}
.evt.ADD.SELL{background:rgba(255,77,77,.06);border-left:2px solid var(--ask)}
.evt.EXEC.BUY {background:rgba(245,158,11,.12);border-left:2px solid var(--exec)}
.evt.EXEC.SELL{background:rgba(245,158,11,.12);border-left:2px solid var(--exec)}
.evt.CXCL    {background:rgba(107,122,141,.06);border-left:2px solid var(--cancel)}
.evt .tag{font-size:.62rem;font-weight:700;letter-spacing:.04em}
.evt.ADD.BUY  .tag{color:var(--bid)}
.evt.ADD.SELL .tag{color:var(--ask)}
.evt.EXEC     .tag{color:var(--exec)}
.evt.CXCL     .tag{color:var(--cancel)}
.evt .side{font-size:.65rem}
.evt.ADD.BUY  .side{color:var(--bid)}
.evt.ADD.SELL .side{color:var(--ask)}
.evt .price{font-variant-numeric:tabular-nums;color:var(--text)}
.evt .shares{color:var(--muted);text-align:right;font-variant-numeric:tabular-nums}
.evt .time{color:var(--muted);font-size:.6rem;text-align:right}

/* order book (center) */
#book-wrap{flex:1;display:flex;flex-direction:column;justify-content:center;
  padding:6px 0;overflow:hidden;min-height:0}
.book-asks,.book-bids{display:flex;flex-direction:column;gap:1px;padding:0 8px}
.brow{display:grid;grid-template-columns:80px 70px 1fr 70px;gap:4px;
  align-items:center;padding:3px 8px;border-radius:3px}
.brow .px{font-variant-numeric:tabular-nums;font-weight:600;font-size:.82rem}
.brow.ask .px{color:var(--ask)}.brow.bid .px{color:var(--bid)}
.brow .sz{font-variant-numeric:tabular-nums;font-size:.76rem;text-align:right}
.brow.ask .sz{color:rgba(255,77,77,.7)}.brow.bid .sz{color:rgba(34,212,94,.7)}
.brow .cum{font-size:.65rem;color:var(--muted);text-align:right;font-variant-numeric:tabular-nums}
.bar-cell{display:flex;align-items:center}
.bar{height:8px;border-radius:1px;min-width:1px;transition:width .2s ease}
.brow.ask .bar{background:rgba(255,77,77,.3)}.brow.bid .bar{background:rgba(34,212,94,.3)}
.brow.flash-ask{animation:flashAsk .4s ease}
.brow.flash-bid{animation:flashBid .4s ease}
@keyframes flashAsk{0%{background:rgba(255,77,77,.35)}100%{background:transparent}}
@keyframes flashBid{0%{background:rgba(34,212,94,.35)}100%{background:transparent}}
.spread-mid{display:flex;justify-content:center;align-items:center;gap:16px;
  padding:8px;margin:4px 8px;
  border-top:1px dashed var(--border);border-bottom:1px dashed var(--border)}
.spread-mid .mid-px{color:var(--exec);font-weight:700;font-size:1rem;font-variant-numeric:tabular-nums}
.spread-mid .sp-val{color:var(--muted);font-size:.72rem}

/* trade tape (right) */
#tape-list{flex:1;overflow-y:auto;padding:6px;display:flex;flex-direction:column;gap:3px;min-height:0}
#tape-list::-webkit-scrollbar{width:3px}#tape-list::-webkit-scrollbar-thumb{background:var(--border)}
.trade{display:grid;grid-template-columns:60px 42px 82px 60px;gap:4px;
  padding:5px 8px;border-radius:3px;animation:fadeRight .25s ease;
  font-size:.73rem;font-variant-numeric:tabular-nums}
@keyframes fadeRight{from{opacity:0;transform:translateX(8px)}to{opacity:1;transform:none}}
.trade.BUY {background:rgba(34,212,94,.07);border-left:2px solid var(--bid)}
.trade.SELL{background:rgba(255,77,77,.07);border-left:2px solid var(--ask)}
.trade .tr-ts{color:var(--muted);font-size:.62rem}
.trade .tr-side{font-weight:700}
.trade.BUY  .tr-side{color:var(--bid)}
.trade.SELL .tr-side{color:var(--ask)}
.trade .tr-px{color:var(--text)}
.trade .tr-sz{color:var(--muted);text-align:right}
</style>
</head>
<body>
<header>
  <span class="logo">NASDAQ ITCH REPLAY</span>
  <select id="sym-sel"><option>loading…</option></select>
  <div class="stat-pill"><span class="lbl">Mid</span><span class="val blue" id="h-mid">—</span></div>
  <div class="stat-pill"><span class="lbl">Last</span><span class="val exec" id="h-last">—</span></div>
  <div class="stat-pill"><span class="lbl">Spread</span><span class="val" id="h-spread">—</span></div>
  <div class="stat-pill"><span class="lbl">VWAP</span><span class="val blue" id="h-vwap">—</span></div>
  <div class="stat-pill"><span class="lbl">Hi</span><span class="val bid" id="h-hi">—</span></div>
  <div class="stat-pill"><span class="lbl">Lo</span><span class="val ask" id="h-lo">—</span></div>
  <div class="stat-pill"><span class="lbl">Volume</span><span class="val blue" id="h-vol">—</span></div>
  <div class="live-dot"></div>
</header>
<div id="ctr-bar">
  Adds: <span id="c-add">0</span> &nbsp;
  Execs: <span id="c-exe">0</span> &nbsp;
  Cancels: <span id="c-can">0</span> &nbsp;
  <span id="c-time" style="margin-left:auto"></span>
</div>
<main>
  <div class="pane">
    <div class="pane-hdr">Event Stream</div>
    <div id="evt-list"></div>
  </div>
  <div class="pane">
    <div class="pane-hdr" style="text-align:center">Order Book</div>
    <div id="book-wrap">
      <div class="book-asks" id="asks"></div>
      <div class="spread-mid">
        <span class="sp-val" id="sp-label">spread</span>
        <span class="mid-px" id="sp-mid">—</span>
        <span class="sp-val" id="sp-ticks">—</span>
      </div>
      <div class="book-bids" id="bids"></div>
    </div>
  </div>
  <div class="pane">
    <div class="pane-hdr">Trade Tape</div>
    <div id="tape-list"></div>
  </div>
</main>

<script>
const MAX_EVT   = 80;
const MAX_TRADE = 80;
let prevBids = {}, prevAsks = {}, selectedSym = null;

// symbol selector
const sel = document.getElementById('sym-sel');
sel.addEventListener('change', () => {
  selectedSym = sel.value;
  fetch('/select/' + encodeURIComponent(selectedSym));
  prevBids = {}; prevAsks = {};
  document.getElementById('evt-list').innerHTML = '';
  document.getElementById('tape-list').innerHTML = '';
});

function prepend(container, el, max) {
  container.prepend(el);
  while (container.children.length > max) container.removeChild(container.lastChild);
}

function onFrame(d) {
  // update symbol selector
  if (d.syms.length) {
    const cur = sel.value;
    const opts = d.syms.map(s => \`<option value="\${s}" \${s===d.sym?'selected':''}>\${s}</option>\`).join('');
    sel.innerHTML = opts;
    if (!selectedSym) selectedSym = d.sym;
  }

  // header stats
  const {bb, ba, spread} = d.book;
  const mid = bb && ba ? ((bb+ba)/2/10000).toFixed(4) : '—';
  document.getElementById('h-mid').textContent    = bb && ba ? '$'+mid : '—';
  document.getElementById('h-last').textContent   = d.stat.last !== '—' ? '$'+d.stat.last : '—';
  document.getElementById('h-spread').textContent = spread ? (spread/100).toFixed(2)+'¢' : '—';
  document.getElementById('h-vwap').textContent   = d.stat.vwap !== '—' ? '$'+d.stat.vwap : '—';
  document.getElementById('h-hi').textContent     = d.stat.hi   !== '—' ? '$'+d.stat.hi   : '—';
  document.getElementById('h-lo').textContent     = d.stat.lo   !== '—' ? '$'+d.stat.lo   : '—';
  document.getElementById('h-vol').textContent    = d.stat.vol  ? d.stat.vol.toLocaleString()+' sh' : '—';
  document.getElementById('c-add').textContent    = d.stat.addCt.toLocaleString();
  document.getElementById('c-exe').textContent    = d.stat.exeCt.toLocaleString();
  document.getElementById('c-can').textContent    = d.stat.canCt.toLocaleString();
  document.getElementById('c-time').textContent   = d.stat.lastTs !== '—' ? 'Last trade: '+d.stat.lastTs : '';

  // spread row
  document.getElementById('sp-mid').textContent   = bb && ba ? '$'+mid : '—';
  document.getElementById('sp-ticks').textContent = spread ? spread+' ticks' : '';

  // order book
  const allSz = [...d.book.bids.map(x=>x.s), ...d.book.asks.map(x=>x.s)];
  const maxSz = Math.max(...allSz, 1);

  // cumulative sizes for depth display
  let cumBid = 0, cumAsk = 0;

  const asksEl = document.getElementById('asks');
  asksEl.innerHTML = '';
  const asksSorted = [...d.book.asks].sort((a,b)=>b.p-a.p); // high→low so best ask at bottom
  for (const {p,s} of asksSorted) {
    cumAsk += s;
    const changed = prevAsks[p] !== undefined && prevAsks[p] !== s;
    const row = document.createElement('div');
    row.className = 'brow ask' + (changed ? ' flash-ask' : '');
    const barW = Math.round(s / maxSz * 140);
    row.innerHTML =
      \`<span class="px">\${(p/10000).toFixed(4)}</span>\` +
      \`<span class="sz">\${s.toLocaleString()}</span>\` +
      \`<div class="bar-cell"><div class="bar" style="width:\${barW}px"></div></div>\` +
      \`<span class="cum">\${cumAsk.toLocaleString()}</span>\`;
    asksEl.appendChild(row);
  }

  const bidsEl = document.getElementById('bids');
  bidsEl.innerHTML = '';
  for (const {p,s} of d.book.bids) {
    cumBid += s;
    const changed = prevBids[p] !== undefined && prevBids[p] !== s;
    const row = document.createElement('div');
    row.className = 'brow bid' + (changed ? ' flash-bid' : '');
    const barW = Math.round(s / maxSz * 140);
    row.innerHTML =
      \`<span class="px">\${(p/10000).toFixed(4)}</span>\` +
      \`<span class="sz">\${s.toLocaleString()}</span>\` +
      \`<div class="bar-cell"><div class="bar" style="width:\${barW}px"></div></div>\` +
      \`<span class="cum">\${cumBid.toLocaleString()}</span>\`;
    bidsEl.appendChild(row);
  }

  prevBids = {}; d.book.bids.forEach(({p,s}) => prevBids[p]=s);
  prevAsks = {}; d.book.asks.forEach(({p,s}) => prevAsks[p]=s);

  // events
  const evtEl = document.getElementById('evt-list');
  for (const e of d.events) {
    const el = document.createElement('div');
    el.className = \`evt \${e.tag} \${e.side||''}\`;
    el.innerHTML =
      \`<span class="tag">\${e.tag}</span>\` +
      \`<span class="side">\${e.side||''}</span>\` +
      \`<span class="price">\$\${e.price}</span>\` +
      \`<span class="shares">\${e.shares.toLocaleString()}</span>\` +
      \`<span class="time">\${e.ts}</span>\`;
    prepend(evtEl, el, MAX_EVT);
    // executions also go to tape
    if (e.tag === 'EXEC') {
      const te = document.createElement('div');
      te.className = \`trade \${e.side}\`;
      te.innerHTML =
        \`<span class="tr-ts">\${e.ts}</span>\` +
        \`<span class="tr-side">\${e.side}</span>\` +
        \`<span class="tr-px">\$\${e.price}</span>\` +
        \`<span class="tr-sz">\${e.shares.toLocaleString()} sh</span>\`;
      prepend(document.getElementById('tape-list'), te, MAX_TRADE);
    }
  }
}

const es = new EventSource('/stream');
es.addEventListener('frame', e => onFrame(JSON.parse(e.data)));
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  if (req.url === "/stream") {
    res.writeHead(200, { "Content-Type":"text/event-stream", "Cache-Control":"no-cache",
      "Access-Control-Allow-Origin":"*", Connection:"keep-alive" });
    res.write("retry:1000\n\n");
    clients.add(res);
    req.on("close", () => clients.delete(res));
    return;
  }
  if (req.url?.startsWith("/select/")) {
    activeSym = decodeURIComponent(req.url.slice(8));
    lastEvtIdx.clear();
    res.writeHead(200); res.end();
    return;
  }
  res.writeHead(200, { "Content-Type":"text/html" });
  res.end(HTML);
});

server.listen(PORT, "0.0.0.0", () => {
  console.log(`Nasdaq replay at http://localhost:${PORT}`);
  console.log(`File: ${FILE}`);
  init();
});
