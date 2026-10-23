/**
 * Nasdaq TotalView-ITCH 5.0 — human-speed order-book replay
 * Port 7000  ·  top-3 symbols  ·  100 trades/session
 * Every 2 s: trade highlight + 2 s pause
 * Every 10th trade: 5 s detail overlay explaining why/how fill happened
 * Event log: ADD=green  DELETE=red  MODIFY=orange
 */
import http from "http";
import fs   from "fs";
import zlib from "zlib";
import path from "path";
import os   from "os";

const PORT           = 7000;
const FILE           = process.argv[2] ?? path.join(os.homedir(), "data/itch/synthetic.bin");
const TOP_N          = 3;
const SESSION_TRADES = 100;
const NORMAL_PAUSE   = 2000;   // ms pause after every trade
const DETAIL_PAUSE   = 5000;   // ms pause every 10th trade
const BOOK_INTERVAL  = 700;    // ms between book pushes
const PROC_BATCH     = 300;
const PROC_TICK      = 15;
const DEPTH          = 6;
const MAX_EVTLOG     = 8;      // recent add/del/mod per symbol

// ── ITCH loader ───────────────────────────────────────────────────────────────

async function loadMessages(filePath) {
  const isGz = filePath.endsWith(".gz");
  const src  = isGz
    ? fs.createReadStream(filePath).pipe(zlib.createGunzip())
    : fs.createReadStream(filePath);
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

function r48(b, o) { return b.readUInt32BE(o) * 65536 + b.readUInt16BE(o + 4); }

function parseMsg(buf) {
  const t = String.fromCharCode(buf[0]), loc = buf.readUInt16BE(1), ts = r48(buf, 5);
  switch (t) {
    case "R": return { t, loc, ts, sym: buf.subarray(11,19).toString("ascii").trimEnd() };
    case "A":
    case "F": return { t:"A", loc, ts, ref:buf.readBigUInt64BE(11),
        side:String.fromCharCode(buf[19]), shares:buf.readUInt32BE(20), price:buf.readUInt32BE(32) };
    case "E": return { t, loc, ts, ref:buf.readBigUInt64BE(11), exec:buf.readUInt32BE(19) };
    case "C": return { t, loc, ts, ref:buf.readBigUInt64BE(11),
        exec:buf.readUInt32BE(19), price:buf.readUInt32BE(32) };
    case "X": return { t, loc, ts, ref:buf.readBigUInt64BE(11), cancel:buf.readUInt32BE(19) };
    case "D": return { t, loc, ts, ref:buf.readBigUInt64BE(11) };
    case "U": return { t, loc, ts, origRef:buf.readBigUInt64BE(11),
        newRef:buf.readBigUInt64BE(19), shares:buf.readUInt32BE(27), price:buf.readUInt32BE(31) };
    default:  return { t, loc, ts };
  }
}

// ── order-book state ──────────────────────────────────────────────────────────

const symByLoc  = new Map();
const locBySym  = new Map();
const orders    = new Map();
const bookMap   = new Map();
const statMap   = new Map();
const evtLogMap = new Map(); // loc → [{kind,side,price,oldPrice,shares,ts}]

function getBook(loc) {
  if (!bookMap.has(loc)) bookMap.set(loc, { bids:new Map(), asks:new Map() });
  return bookMap.get(loc);
}
function getStat(loc) {
  if (!statMap.has(loc)) statMap.set(loc, { vol:0, turn:0, hi:0, lo:Infinity, lastPx:0, lastTs:0 });
  return statMap.get(loc);
}
function pushEvtLog(loc, evt) {
  if (!evtLogMap.has(loc)) evtLogMap.set(loc, []);
  const q = evtLogMap.get(loc);
  q.push(evt);
  if (q.length > MAX_EVTLOG) q.shift();
}

function applyMsg(m) {
  switch (m.t) {
    case "R":
      symByLoc.set(m.loc, m.sym); locBySym.set(m.sym, m.loc); break;

    case "A": {
      orders.set(m.ref, { loc:m.loc, side:m.side, shares:m.shares, price:m.price });
      const map = m.side === "B" ? getBook(m.loc).bids : getBook(m.loc).asks;
      map.set(m.price, (map.get(m.price)||0) + m.shares);
      if (top3Locs.has(m.loc))
        pushEvtLog(m.loc, { kind:"ADD", side:m.side==="B"?"BUY":"SELL",
          price:m.price, shares:m.shares, ts:m.ts });
      break;
    }
    case "E": case "C": {
      const o = orders.get(m.ref); if (!o) break;
      const px = m.t === "C" ? m.price : o.price;
      const bk = getBook(o.loc), map = o.side==="B" ? bk.bids : bk.asks;
      const lv = map.get(o.price);
      if (lv !== undefined) { const n = lv - m.exec; n <= 0 ? map.delete(o.price) : map.set(o.price, n); }
      o.shares -= m.exec; if (o.shares <= 0) orders.delete(m.ref);
      const st = getStat(o.loc);
      st.vol += m.exec; st.turn += m.exec * px; st.lastPx = px; st.lastTs = m.ts;
      if (px > st.hi) st.hi = px; if (px < st.lo) st.lo = px;
      return { execPx:px, execShares:m.exec, side:o.side, loc:o.loc, orderPrice:o.price };
    }
    case "X": {
      const o = orders.get(m.ref); if (!o) break;
      const map = o.side==="B" ? getBook(o.loc).bids : getBook(o.loc).asks;
      const lv = map.get(o.price);
      if (lv !== undefined) { const n = lv - m.cancel; n <= 0 ? map.delete(o.price) : map.set(o.price, n); }
      o.shares -= m.cancel; if (o.shares <= 0) orders.delete(m.ref);
      if (top3Locs.has(o.loc))
        pushEvtLog(o.loc, { kind:"DEL", side:o.side==="B"?"BUY":"SELL",
          price:o.price, shares:m.cancel, ts:m.ts });
      break;
    }
    case "D": {
      const o = orders.get(m.ref); if (!o) break;
      const map = o.side==="B" ? getBook(o.loc).bids : getBook(o.loc).asks;
      const lv = map.get(o.price);
      if (lv !== undefined) { const n = lv - o.shares; n <= 0 ? map.delete(o.price) : map.set(o.price, n); }
      if (top3Locs.has(o.loc))
        pushEvtLog(o.loc, { kind:"DEL", side:o.side==="B"?"BUY":"SELL",
          price:o.price, shares:o.shares, ts:m.ts });
      orders.delete(m.ref); break;
    }
    case "U": {
      const o = orders.get(m.origRef); if (!o) break;
      const map = o.side==="B" ? getBook(o.loc).bids : getBook(o.loc).asks;
      const lv = map.get(o.price);
      if (lv !== undefined) { const n = lv - o.shares; n <= 0 ? map.delete(o.price) : map.set(o.price, n); }
      if (top3Locs.has(o.loc))
        pushEvtLog(o.loc, { kind:"MOD", side:o.side==="B"?"BUY":"SELL",
          price:m.price, oldPrice:o.price, shares:m.shares, ts:m.ts });
      orders.delete(m.origRef);
      orders.set(m.newRef, { loc:o.loc, side:o.side, shares:m.shares, price:m.price });
      map.set(m.price, (map.get(m.price)||0) + m.shares);
      break;
    }
  }
  return null;
}

function resetBookState() {
  orders.clear(); bookMap.clear(); statMap.clear();
  evtLogMap.clear(); symByLoc.clear(); locBySym.clear();
}

// ── snapshot helpers ──────────────────────────────────────────────────────────

function fp(v)  { return (v / 10000).toFixed(2); }
function fts(ns) {
  const s=Math.floor(ns/1e9), h=Math.floor(s/3600), m=Math.floor((s%3600)/60), sc=s%60;
  return `${String(h).padStart(2,"0")}:${String(m).padStart(2,"0")}:${String(sc).padStart(2,"0")}`;
}

function bookSnap(loc) {
  const bk = bookMap.get(loc);
  if (!bk) return { bids:[], asks:[], bb:0, ba:0, spread:0 };
  const bids = [...bk.bids.entries()].sort((a,b)=>b[0]-a[0]).slice(0,DEPTH).map(([p,s])=>({p,s}));
  const asks = [...bk.asks.entries()].sort((a,b)=>a[0]-b[0]).slice(0,DEPTH).map(([p,s])=>({p,s}));
  const bb = bids[0]?.p||0, ba = asks[0]?.p||0;
  return { bids, asks, bb, ba, spread: ba&&bb ? ba-bb : 0 };
}

function statSnap(loc) {
  const st = statMap.get(loc)||{};
  return { last:st.lastPx?fp(st.lastPx):"—", ts:st.lastTs?fts(st.lastTs):"—",
    vol:st.vol||0, vwap:st.vol?fp(st.turn/st.vol):"—",
    hi:st.hi?fp(st.hi):"—", lo:st.lo!==Infinity?fp(st.lo):"—" };
}

function evtSnap(loc) {
  return (evtLogMap.get(loc)||[]).map(e => ({
    kind:e.kind, side:e.side, price:fp(e.price),
    oldPrice:e.oldPrice?fp(e.oldPrice):null,
    shares:e.shares, ts:fts(e.ts)
  }));
}

// Build fill detail explanation for every 10th trade
function buildDetail(exec, snap) {
  const { bb, ba, spread, bids, asks } = snap;
  const aggrSide = exec.side === "B" ? "SELL" : "BUY";
  const restSide = exec.side === "B" ? "BUY"  : "SELL";
  const hitSide  = exec.side === "B" ? "bid"  : "ask";

  // Find the level that was hit in the pre-fill book
  const matchedLevel = exec.side === "B"
    ? bids.find(b => b.p === exec.orderPrice) || bids[0]
    : asks.find(a => a.p === exec.orderPrice) || asks[0];

  return {
    tradeNum:  exec.session,
    sym:       exec.sym,
    restSide,  aggrSide,
    price:     fp(exec.execPx),
    shares:    exec.execShares,
    bb:        bb ? fp(bb) : "—",
    ba:        ba ? fp(ba) : "—",
    spread,
    hitSide,
    steps: [
      `An aggressive ${aggrSide} order entered the exchange`,
      `It priced at $${fp(exec.execPx)} — crossing into the ${hitSide} side`,
      `Found resting ${restSide} at $${fp(exec.orderPrice)} · ${exec.execShares.toLocaleString()} shares available`,
      `Price-time priority: best ${hitSide} matched first`,
      `Exchange confirmed fill: ${exec.execShares.toLocaleString()} sh @ $${fp(exec.execPx)}`,
    ],
    bookBefore: { bids: bids.slice(0,4), asks: asks.slice(0,4) },
  };
}

// ── SSE ───────────────────────────────────────────────────────────────────────

const clients = new Set();
function push(event, data) {
  const msg = `event:${event}\ndata:${JSON.stringify(data)}\n\n`;
  for (const c of clients) c.write(msg);
}

// ── replay engine ─────────────────────────────────────────────────────────────

let messages      = [];
let top3Locs      = new Set();
let top3Syms      = [];
let msgIdx        = 0;
let sessionTrades = 0;
let paused        = false;
let execQueue     = [];

async function init() {
  console.log(`Loading ${FILE}…`);
  messages = await loadMessages(FILE);
  console.log(`Loaded ${messages.length.toLocaleString()} messages — prescanning…`);

  const execCt = new Map();
  for (const buf of messages) {
    const m = parseMsg(buf);
    if (m.t === "R") { symByLoc.set(m.loc, m.sym); locBySym.set(m.sym, m.loc); }
    if (m.t === "E" || m.t === "C") execCt.set(m.loc, (execCt.get(m.loc)||0)+1);
  }
  const sorted = [...execCt.entries()].sort((a,b)=>b[1]-a[1]).slice(0, TOP_N);
  top3Locs = new Set(sorted.map(([l])=>l));
  top3Syms = sorted.map(([l]) => symByLoc.get(l) || `LOC${l}`);
  symByLoc.clear(); locBySym.clear();
  console.log(`Top ${TOP_N}: ${top3Syms.join(", ")}`);

  startBookPush();
  setTimeout(processTick, PROC_TICK);
  dequeueExec();
}

function processTick() {
  if (!paused) {
    const end = Math.min(msgIdx + PROC_BATCH, messages.length);
    for (; msgIdx < end; msgIdx++) {
      const m = parseMsg(messages[msgIdx]);
      const result = applyMsg(m);
      if (result && top3Locs.has(result.loc)) {
        const snap = bookSnap(result.loc);
        execQueue.push({
          sym:        symByLoc.get(result.loc) || "?",
          loc:        result.loc,
          side:       result.side,
          execPx:     result.execPx,
          execShares: result.execShares,
          orderPrice: result.orderPrice,
          ts:         fts(m.ts),
          snapBefore: JSON.parse(JSON.stringify(snap)),  // book state at fill
        });
      }
    }
    if (msgIdx >= messages.length) {
      msgIdx = 0; resetBookState();
    }
  }
  setTimeout(processTick, PROC_TICK);
}

function dequeueExec() {
  if (paused || execQueue.length === 0) {
    setTimeout(dequeueExec, 150);
    return;
  }

  const e = execQueue.shift();
  if (!top3Locs.has(e.loc)) { setTimeout(dequeueExec, 0); return; }

  sessionTrades++;
  const isDetail  = (sessionTrades % 10 === 0);
  const pauseMs   = isDetail ? DETAIL_PAUSE : NORMAL_PAUSE;
  const snap      = bookSnap(e.loc);
  const st        = statSnap(e.loc);
  const detail    = isDetail ? buildDetail({ ...e, session: sessionTrades }, e.snapBefore) : null;

  push("trade", {
    sym:     e.sym,
    side:    e.side === "B" ? "BUY" : "SELL",
    price:   fp(e.execPx),
    shares:  e.execShares,
    ts:      e.ts,
    session: sessionTrades,
    total:   SESSION_TRADES,
    isDetail,
    pauseMs,
    detail,
    book:    snap,
    stat:    st,
  });

  if (sessionTrades >= SESSION_TRADES) {
    sessionTrades = 0;
    paused = true;
    execQueue = [];
    push("session_end", {});
    setTimeout(() => { paused = false; push("session_start", { syms: top3Syms }); }, 5000);
    return;
  }

  setTimeout(dequeueExec, pauseMs);
}

function startBookPush() {
  setInterval(() => {
    if (paused) return;
    const books = {}, stats = {}, evts = {};
    for (const loc of top3Locs) {
      const sym = symByLoc.get(loc); if (!sym) continue;
      books[sym] = bookSnap(loc);
      stats[sym] = statSnap(loc);
      evts[sym]  = evtSnap(loc);
    }
    push("book", { syms: top3Syms, books, stats, evts });
  }, BOOK_INTERVAL);
}

// ── HTML ──────────────────────────────────────────────────────────────────────

const HTML = `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<title>Nasdaq Replay</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
:root{
  --bg:#07090d;--panel:#0b0f15;--border:#1a2030;--muted:#3d5068;
  --bid:#22d45e;--ask:#ff4444;--exec:#f59e0b;--blue:#4da6ff;--text:#dde8f0;
  --add:#22d45e;--del:#ff4444;--mod:#f97316;
}
body{background:var(--bg);color:var(--text);font-family:'Cascadia Code','Fira Code','Consolas',monospace;
  height:100vh;display:flex;flex-direction:column;overflow:hidden;font-size:12px}

/* header */
header{background:#050709;border-bottom:1px solid var(--border);padding:8px 18px;
  display:flex;align-items:center;gap:14px;flex-shrink:0}
.logo{font-size:.9rem;font-weight:700;color:var(--blue);letter-spacing:.07em}
.sess{background:#0f1620;border:1px solid var(--border);border-radius:20px;
  padding:4px 14px;font-size:.75rem;display:flex;align-items:center;gap:8px}
.sess .cnt{color:var(--exec);font-weight:700}
.prog-track{background:#1a2030;border-radius:4px;height:5px;width:110px;overflow:hidden}
.prog-fill{height:100%;background:linear-gradient(90deg,var(--blue),var(--exec));
  border-radius:4px;transition:width .5s ease}
.live{width:7px;height:7px;border-radius:50%;background:var(--bid);
  animation:pulse 1.4s infinite;margin-left:auto}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:.25}}

/* session-end overlay */
#sess-ov{position:fixed;inset:0;background:rgba(5,7,9,.88);z-index:200;
  display:none;align-items:center;justify-content:center;flex-direction:column;gap:14px}
#sess-ov.show{display:flex}
#sess-ov h2{font-size:1.5rem;color:var(--exec);letter-spacing:.06em}
#sess-ov .cd{font-size:2.8rem;color:var(--blue);font-weight:700}

/* fill-detail overlay */
#detail-ov{position:fixed;inset:0;background:rgba(5,7,9,.92);z-index:150;
  display:none;align-items:center;justify-content:center}
#detail-ov.show{display:flex}
.detail-box{background:#0d1520;border:1px solid var(--border);border-radius:10px;
  padding:28px 32px;max-width:560px;width:90%;display:flex;flex-direction:column;gap:18px}
.detail-box .title{font-size:.7rem;text-transform:uppercase;letter-spacing:.1em;color:var(--muted)}
.detail-box .headline{font-size:1.15rem;font-weight:700;color:var(--exec)}
.detail-box .steps{display:flex;flex-direction:column;gap:7px}
.detail-box .step{display:flex;gap:10px;font-size:.78rem;color:var(--text);line-height:1.5}
.detail-box .step .num{color:var(--blue);font-weight:700;min-width:16px}
.mini-book{display:grid;grid-template-columns:1fr 60px 1fr;gap:2px;font-size:.72rem}
.mini-book .hdr{text-align:center;color:var(--muted);font-size:.6rem;
  text-transform:uppercase;letter-spacing:.07em;padding-bottom:4px;border-bottom:1px solid var(--border)}
.mb-ask{color:var(--ask);text-align:center;padding:2px 4px;border-radius:2px}
.mb-bid{color:var(--bid);text-align:center;padding:2px 4px;border-radius:2px}
.mb-ask.hit,.mb-bid.hit{font-weight:700;animation:hitRow .6s ease infinite alternate}
@keyframes hitRow{from{background:rgba(245,158,11,.35)}to{background:rgba(245,158,11,.1)}}
.mb-spread{display:flex;flex-direction:column;align-items:center;justify-content:center;
  gap:4px;color:var(--muted);font-size:.65rem}
.mb-spread .sp-val{color:var(--exec);font-weight:700}
.detail-timer{height:4px;background:var(--border);border-radius:2px;overflow:hidden}
.detail-timer-fill{height:100%;background:var(--blue);border-radius:2px;
  transition:width linear}

/* main 3-column */
main{flex:1;display:grid;grid-template-columns:repeat(3,1fr);gap:1px;
  background:var(--border);overflow:hidden;min-height:0}
.col{background:var(--panel);display:flex;flex-direction:column;overflow:hidden;
  min-height:0;position:relative}
.col.flash-buy {animation:cfb .9s ease}
.col.flash-sell{animation:cfs .9s ease}
@keyframes cfb{0%{background:rgba(34,212,94,.18)}100%{background:var(--panel)}}
@keyframes cfs{0%{background:rgba(255,68,68,.18)}100%{background:var(--panel)}}

/* column header */
.col-hdr{padding:8px 12px;background:#050709;border-bottom:1px solid var(--border);flex-shrink:0}
.col-hdr .sym{font-size:.95rem;font-weight:700;color:var(--blue)}
.col-hdr .sts{display:flex;gap:10px;margin-top:4px;flex-wrap:wrap}
.st .k{font-size:.55rem;color:var(--muted);text-transform:uppercase;letter-spacing:.06em}
.st .v{font-size:.76rem;font-weight:600;font-variant-numeric:tabular-nums}
.v.g{color:var(--bid)}.v.r{color:var(--ask)}.v.o{color:var(--exec)}.v.b{color:var(--blue)}

/* fill banner */
.f-banner{position:absolute;top:50px;left:0;right:0;z-index:10;
  padding:8px 12px;display:none;align-items:center;justify-content:center;
  gap:8px;font-size:.82rem;font-weight:700;letter-spacing:.03em}
.f-banner.show{display:flex;animation:bIn .25s ease}
@keyframes bIn{from{opacity:0;transform:translateY(-6px)}to{opacity:1;transform:none}}
.f-banner.BUY {background:rgba(34,212,94,.22);border-bottom:1px solid rgba(34,212,94,.45);color:var(--bid)}
.f-banner.SELL{background:rgba(255,68,68,.22);border-bottom:1px solid rgba(255,68,68,.45);color:var(--ask)}

/* book */
.book{flex:1;display:flex;flex-direction:column;justify-content:center;
  padding:2px 0;overflow:hidden;min-height:0}
.asks,.bids{display:flex;flex-direction:column;gap:1px;padding:0 6px}
.brow{display:grid;grid-template-columns:72px 60px 1fr;gap:4px;
  align-items:center;padding:3px 8px;border-radius:3px}
.brow .px{font-variant-numeric:tabular-nums;font-weight:600;font-size:.8rem}
.brow.ask .px{color:var(--ask)}.brow.bid .px{color:var(--bid)}
.brow .sz{font-variant-numeric:tabular-nums;font-size:.72rem;text-align:right;color:var(--muted)}
.bar-w{overflow:hidden;height:10px;display:flex;align-items:center}
.bar{height:7px;border-radius:1px;min-width:2px;transition:width .3s ease}
.brow.ask .bar{background:rgba(255,68,68,.32)}.brow.bid .bar{background:rgba(34,212,94,.32)}
.brow.exec-flash-ask{animation:efA .9s ease}
.brow.exec-flash-bid{animation:efB .9s ease}
@keyframes efA{0%{background:rgba(255,68,68,.7);transform:scaleX(1.015)}70%{background:rgba(255,68,68,.2)}100%{background:transparent}}
@keyframes efB{0%{background:rgba(34,212,94,.7);transform:scaleX(1.015)}70%{background:rgba(34,212,94,.2)}100%{background:transparent}}
.spread-row{display:flex;justify-content:center;align-items:center;gap:8px;
  padding:5px 8px;border-top:1px dashed var(--border);border-bottom:1px dashed var(--border);
  margin:3px 6px;font-size:.68rem;color:var(--muted)}
.spread-row .mid{color:var(--exec);font-weight:700;font-size:.82rem}

/* event log */
.evtlog{border-top:1px solid var(--border);flex-shrink:0;padding:4px 6px;
  display:flex;flex-direction:column;gap:2px;max-height:96px;overflow:hidden}
.evtlog-hdr{font-size:.58rem;text-transform:uppercase;letter-spacing:.09em;
  color:var(--muted);padding:0 4px 2px;border-bottom:1px solid var(--border);margin-bottom:2px}
.erow{display:grid;grid-template-columns:28px 36px 68px 52px 1fr;gap:3px;
  align-items:center;padding:2px 6px;border-radius:2px;font-size:.68rem;
  font-variant-numeric:tabular-nums;animation:eIn .2s ease;border-left:2px solid}
@keyframes eIn{from{opacity:0;transform:translateX(-4px)}to{opacity:1;transform:none}}
.erow.ADD {background:rgba(34,212,94,.06); border-color:var(--add)}
.erow.DEL {background:rgba(255,68,68,.06); border-color:var(--del)}
.erow.MOD {background:rgba(249,115,22,.08);border-color:var(--mod)}
.erow .etag{font-weight:700;font-size:.62rem}
.erow.ADD .etag{color:var(--add)}.erow.DEL .etag{color:var(--del)}.erow.MOD .etag{color:var(--mod)}
.erow .eside.BUY{color:var(--bid)}.erow .eside.SELL{color:var(--ask)}
.erow .epx{color:var(--text)}.erow .esz{color:var(--muted);text-align:right}
.erow .ets{color:var(--muted);font-size:.6rem}

/* tape */
.tape{border-top:1px solid var(--border);flex-shrink:0;max-height:90px;
  overflow-y:auto;padding:3px}
.tape::-webkit-scrollbar{width:3px}.tape::-webkit-scrollbar-thumb{background:var(--border)}
.trow{display:grid;grid-template-columns:56px 36px 68px 52px;gap:3px;
  padding:3px 8px;border-radius:3px;font-size:.7rem;font-variant-numeric:tabular-nums;
  animation:tIn .4s ease}
@keyframes tIn{from{opacity:0;transform:translateY(5px)}to{opacity:1;transform:none}}
.trow.new-trade{animation:tNew .8s ease}
@keyframes tNew{0%{background:rgba(245,158,11,.4)}100%{background:transparent}}
.trow.BUY {background:rgba(34,212,94,.07);border-left:2px solid var(--bid)}
.trow.SELL{background:rgba(255,68,68,.07);border-left:2px solid var(--ask)}
.trow .t-ts{color:var(--muted);font-size:.6rem}
.trow .t-sd{font-weight:700}
.trow.BUY .t-sd{color:var(--bid)}.trow.SELL .t-sd{color:var(--ask)}
.trow .t-px{color:var(--text)}.trow .t-sz{color:var(--muted);text-align:right}
</style>
</head>
<body>

<!-- session end -->
<div id="sess-ov">
  <h2>SESSION COMPLETE — 100 TRADES</h2>
  <p style="color:var(--muted);font-size:.82rem">Next session starting in</p>
  <div class="cd" id="sess-cd">5</div>
</div>

<!-- fill detail (every 10th trade) -->
<div id="detail-ov">
  <div class="detail-box">
    <div>
      <div class="title" id="d-title">Trade detail</div>
      <div class="headline" id="d-headline"></div>
    </div>
    <div class="mini-book" id="d-book"></div>
    <div class="steps" id="d-steps"></div>
    <div>
      <div style="display:flex;justify-content:space-between;font-size:.68rem;color:var(--muted);margin-bottom:4px">
        <span>Best bid: <b id="d-bb" style="color:var(--bid)"></b></span>
        <span>Spread: <b id="d-sp" style="color:var(--exec)"></b></span>
        <span>Best ask: <b id="d-ba" style="color:var(--ask)"></b></span>
      </div>
      <div class="detail-timer"><div class="detail-timer-fill" id="d-timer" style="width:100%"></div></div>
    </div>
  </div>
</div>

<header>
  <span class="logo">NASDAQ ITCH REPLAY</span>
  <div class="sess">
    <span style="color:var(--muted)">Session</span>
    <span class="cnt" id="trade-cnt">0 / 100</span>
    <div class="prog-track"><div class="prog-fill" id="prog" style="width:0%"></div></div>
  </div>
  <div class="live"></div>
</header>

<main id="grid"></main>

<script>
const MAX_TAPE = 10, MAX_EVT = 6;
const cols = {}, prevBk = {};
let syms = [];

function buildCol(sym) {
  const el = document.createElement('div');
  el.className = 'col'; el.id = 'col-'+sym;
  el.innerHTML = \`
    <div class="col-hdr">
      <div class="sym">\${sym}</div>
      <div class="sts">
        <div class="st"><span class="k">Last</span><span class="v o" id="sl-\${sym}">—</span></div>
        <div class="st"><span class="k">Mid</span><span class="v b" id="sm-\${sym}">—</span></div>
        <div class="st"><span class="k">Spread</span><span class="v" id="ss-\${sym}">—</span></div>
        <div class="st"><span class="k">VWAP</span><span class="v b" id="sv-\${sym}">—</span></div>
        <div class="st"><span class="k">Hi</span><span class="v g" id="sh-\${sym}">—</span></div>
        <div class="st"><span class="k">Lo</span><span class="v r" id="so-\${sym}">—</span></div>
        <div class="st"><span class="k">Vol</span><span class="v b" id="svl-\${sym}">—</span></div>
      </div>
    </div>
    <div class="f-banner" id="bn-\${sym}"></div>
    <div class="book">
      <div class="asks" id="asks-\${sym}"></div>
      <div class="spread-row">
        <span>spread</span><span class="mid" id="sp-\${sym}">—</span>
        <span id="spt-\${sym}" style="color:var(--muted);font-size:.65rem"></span>
      </div>
      <div class="bids" id="bids-\${sym}"></div>
    </div>
    <div class="evtlog">
      <div class="evtlog-hdr">Order Events &nbsp;
        <span style="color:var(--add)">■ ADD</span> &nbsp;
        <span style="color:var(--del)">■ DEL</span> &nbsp;
        <span style="color:var(--mod)">■ MOD</span>
      </div>
      <div id="evtlog-\${sym}"></div>
    </div>
    <div class="tape" id="tape-\${sym}"></div>
  \`;
  document.getElementById('grid').appendChild(el);
  prevBk[sym] = { bids:{}, asks:{} };
  cols[sym] = { el, tape: null, banner: null };
  cols[sym].tape   = el.querySelector('#tape-'+sym);
  cols[sym].banner = el.querySelector('#bn-'+sym);
  cols[sym].evtlog = el.querySelector('#evtlog-'+sym);
}

function s(id, v) { const e=document.getElementById(id); if(e) e.textContent=v; }

function updateBook(sym, book, stat) {
  const {bb, ba, spread} = book;
  const mid = bb&&ba ? '$'+((bb+ba)/2/10000).toFixed(2) : '—';
  s('sl-'+sym, stat.last!=='—'?'$'+stat.last:'—');
  s('sm-'+sym, mid);
  s('ss-'+sym, spread?(spread/100).toFixed(0)+'¢':'—');
  s('sv-'+sym, stat.vwap!=='—'?'$'+stat.vwap:'—');
  s('sh-'+sym, stat.hi!=='—'?'$'+stat.hi:'—');
  s('so-'+sym, stat.lo!=='—'?'$'+stat.lo:'—');
  s('svl-'+sym, stat.vol?stat.vol.toLocaleString()+' sh':'—');
  s('sp-'+sym, mid);
  s('spt-'+sym, spread?spread+' ticks':'');

  const allSz = [...book.bids.map(x=>x.s),...book.asks.map(x=>x.s)];
  const maxSz = Math.max(...allSz, 1);
  const pb = prevBk[sym]||{bids:{},asks:{}};

  const asksEl = document.getElementById('asks-'+sym);
  if (asksEl) {
    asksEl.innerHTML='';
    [...book.asks].sort((a,b)=>b.p-a.p).forEach(({p,s:sz})=>{
      const chg = pb.asks[p]!==undefined && pb.asks[p]!==sz;
      const r=document.createElement('div');
      r.className='brow ask'+(chg?' exec-flash-ask':'');
      r.innerHTML=\`<span class="px">\${(p/10000).toFixed(2)}</span><span class="sz">\${sz.toLocaleString()}</span><div class="bar-w"><div class="bar" style="width:\${Math.round(sz/maxSz*110)}px"></div></div>\`;
      asksEl.appendChild(r);
    });
  }
  const bidsEl = document.getElementById('bids-'+sym);
  if (bidsEl) {
    bidsEl.innerHTML='';
    book.bids.forEach(({p,s:sz})=>{
      const chg = pb.bids[p]!==undefined && pb.bids[p]!==sz;
      const r=document.createElement('div');
      r.className='brow bid'+(chg?' exec-flash-bid':'');
      r.innerHTML=\`<span class="px">\${(p/10000).toFixed(2)}</span><span class="sz">\${sz.toLocaleString()}</span><div class="bar-w"><div class="bar" style="width:\${Math.round(sz/maxSz*110)}px"></div></div>\`;
      bidsEl.appendChild(r);
    });
  }
  prevBk[sym]={bids:{},asks:{}};
  book.bids.forEach(({p,s:sz})=>prevBk[sym].bids[p]=sz);
  book.asks.forEach(({p,s:sz})=>prevBk[sym].asks[p]=sz);
}

function updateEvtLog(sym, evts) {
  const el = cols[sym]?.evtlog; if (!el || !evts?.length) return;
  el.innerHTML='';
  [...evts].reverse().forEach(e => {
    const r=document.createElement('div');
    r.className='erow '+e.kind;
    const pxStr = e.kind==='MOD' && e.oldPrice
      ? \`\${e.oldPrice}→\${e.price}\`
      : \`\$\${e.price}\`;
    r.innerHTML=\`<span class="etag">\${e.kind}</span><span class="eside \${e.side}">\${e.side}</span><span class="epx">\${pxStr}</span><span class="esz">\${e.shares.toLocaleString()}</span><span class="ets">\${e.ts}</span>\`;
    el.appendChild(r);
  });
}

function flashTrade(sym, side, price, shares, ts) {
  const c = cols[sym]; if (!c) return;
  c.el.classList.remove('flash-buy','flash-sell');
  void c.el.offsetWidth;
  c.el.classList.add(side==='BUY'?'flash-buy':'flash-sell');
  const b = c.banner;
  b.className = 'f-banner '+side+' show';
  b.textContent = (side==='BUY'?'▲':'▼') + '  FILLED  ' + shares.toLocaleString() + ' sh @ $' + price;
  setTimeout(()=>{ b.className='f-banner '+side; }, side==='BUY'?1800:1800);
  // tape
  const row=document.createElement('div');
  row.className=\`trow \${side} new-trade\`;
  row.innerHTML=\`<span class="t-ts">\${ts}</span><span class="t-sd">\${side}</span><span class="t-px">\$\${price}</span><span class="t-sz">\${shares.toLocaleString()} sh</span>\`;
  c.tape.prepend(row);
  while(c.tape.children.length>MAX_TAPE) c.tape.removeChild(c.tape.lastChild);
}

// ── detail overlay ────────────────────────────────────────────────────────────
let detailTimer=null;
function showDetail(d, durationMs) {
  const ov=document.getElementById('detail-ov');
  document.getElementById('d-title').textContent=\`TRADE #\${d.tradeNum} — \${d.sym} — HOW IT FILLED\`;
  document.getElementById('d-headline').textContent=
    \`Aggressive \${d.aggrSide} hit resting \${d.restSide} @ \$\${d.price} · \${d.shares.toLocaleString()} sh\`;
  document.getElementById('d-bb').textContent='\$'+d.bb;
  document.getElementById('d-ba').textContent='\$'+d.ba;
  document.getElementById('d-sp').textContent=d.spread+' ticks';

  // mini book
  const mb=document.getElementById('d-book');
  mb.innerHTML='';
  const addHdr=(txt)=>{ const h=document.createElement('div'); h.className='hdr'; h.textContent=txt; return h; };
  mb.appendChild(addHdr('BID')); mb.appendChild(document.createElement('div')); mb.appendChild(addHdr('ASK'));

  const maxR=Math.max(d.bookBefore.bids.length, d.bookBefore.asks.length);
  for(let i=0;i<maxR;i++){
    const bid=d.bookBefore.bids[i], ask=d.bookBefore.asks[i];
    const bEl=document.createElement('div');
    bEl.className='mb-bid'+(bid&&bid.p===Math.round(parseFloat(d.price)*10000)?' hit':'');
    bEl.textContent=bid?((bid.p/10000).toFixed(2)+' · '+bid.s.toLocaleString()):'';
    const sp=document.createElement('div');
    sp.className='mb-spread';
    if(i===0){ const sv=document.createElement('span'); sv.className='sp-val'; sv.textContent=d.spread+'t'; sp.appendChild(sv); }
    const aEl=document.createElement('div');
    aEl.className='mb-ask'+(ask&&ask.p===Math.round(parseFloat(d.price)*10000)?' hit':'');
    aEl.textContent=ask?((ask.p/10000).toFixed(2)+' · '+ask.s.toLocaleString()):'';
    mb.appendChild(bEl); mb.appendChild(sp); mb.appendChild(aEl);
  }

  // steps
  const stEl=document.getElementById('d-steps');
  stEl.innerHTML='';
  d.steps.forEach((txt,i)=>{
    const row=document.createElement('div'); row.className='step';
    row.innerHTML=\`<span class="num">\${i+1}</span><span>\${txt}</span>\`;
    stEl.appendChild(row);
  });

  // timer bar
  const tf=document.getElementById('d-timer');
  tf.style.transition='none'; tf.style.width='100%';
  void tf.offsetWidth;
  tf.style.transition=\`width \${durationMs}ms linear\`;
  tf.style.width='0%';

  ov.classList.add('show');
  clearTimeout(detailTimer);
  detailTimer=setTimeout(()=>ov.classList.remove('show'), durationMs);
}

// ── session end ───────────────────────────────────────────────────────────────
let sessTimer=null;
function showSessionEnd() {
  const ov=document.getElementById('sess-ov'); ov.classList.add('show');
  let n=5; s('sess-cd',n);
  sessTimer=setInterval(()=>{ n--; s('sess-cd',n); if(n<=0){clearInterval(sessTimer);ov.classList.remove('show');} },1000);
}

// ── SSE ───────────────────────────────────────────────────────────────────────
const es=new EventSource('/stream');

es.addEventListener('book', e=>{
  const d=JSON.parse(e.data);
  if(!syms.length){ syms=d.syms; syms.forEach(buildCol); }
  syms.forEach(sym=>{ if(d.books[sym]){ updateBook(sym,d.books[sym],d.stats[sym]||{}); updateEvtLog(sym,d.evts[sym]); } });
});

es.addEventListener('trade', e=>{
  const d=JSON.parse(e.data);
  if(!syms.length) return;
  s('trade-cnt', d.session+' / '+d.total);
  document.getElementById('prog').style.width=(d.session/d.total*100)+'%';
  if(d.book) updateBook(d.sym,d.book,d.stat||{});
  flashTrade(d.sym,d.side,d.price,d.shares,d.ts);
  if(d.isDetail && d.detail) showDetail(d.detail, d.pauseMs);
});

es.addEventListener('session_end', ()=>showSessionEnd());
es.addEventListener('session_start', ()=>{
  s('trade-cnt','0 / 100');
  document.getElementById('prog').style.width='0%';
  syms.forEach(sym=>{ if(cols[sym]){ cols[sym].tape.innerHTML=''; cols[sym].evtlog.innerHTML=''; } prevBk[sym]={bids:{},asks:{}}; });
});
</script>
</body>
</html>`;

const server = http.createServer((req, res) => {
  if (req.url === "/stream") {
    res.writeHead(200,{"Content-Type":"text/event-stream","Cache-Control":"no-cache",
      "Access-Control-Allow-Origin":"*",Connection:"keep-alive"});
    res.write("retry:2000\n\n");
    clients.add(res); req.on("close",()=>clients.delete(res)); return;
  }
  res.writeHead(200,{"Content-Type":"text/html"}); res.end(HTML);
});

server.listen(PORT,"0.0.0.0",()=>{
  console.log(`Nasdaq replay at http://localhost:${PORT}`);
  init();
});
