"""ReportAgent: a markdown account of what the system saw and did (DESIGN.md 2, layer 7).

MiroFish ends each simulation with a report agent that explains the result.
This one explains a decision cycle: which headlines moved which names, how
the engines and the swarm read them, and where risk stepped in.
"""

from __future__ import annotations

from .backtest import BacktestResult
from .models import Decision, DecisionBatch


def _pct(x: float) -> str:
    return f"{x:+.2%}"


def decision_block(d: Decision, max_items: int = 3) -> list[str]:
    s, f = d.signal, d.forecast
    side = "LONG" if d.target_weight > 0 else "SHORT" if d.target_weight < 0 else "no trade"
    out = [f"#### {d.ticker}: {side} (alpha {d.alpha:+.2f}, conviction {d.conviction:.2f}, "
           f"target {d.target_weight:+.1%})"]
    if s:
        out.append(f"- Sentiment {s.score:+.2f}, confidence {s.confidence:.2f}, buzz {s.buzz:.2f}, "
                   f"expected magnitude {s.magnitude:.1%}")
        for a in sorted(s.assessments, key=lambda a: -abs(a.score * a.confidence))[:max_items]:
            tag = " (spillover)" if a.spillover else ""
            engines = ", ".join(f"{v.engine} {v.score:+.2f}" for v in a.views if v.confidence > 0.1)
            out.append(f"  - \"{a.item.headline}\" [{a.item.source}]{tag}: {a.event_type}, "
                       f"forum {a.score:+.2f}; {engines or 'no engine view'}")
    if f:
        flow = ", ".join(f"{k} {v:+.2f}" for k, v in sorted(f.persona_flow.items(), key=lambda kv: -abs(kv[1])))
        out.append(f"- Swarm: day one {_pct(f.sim_day_one)}, end {_pct(f.sim_total)}, continuation "
                   f"{_pct(f.continuation)}, gap to today's move {_pct(f.gap)}, calibrated "
                   f"{_pct(f.calibrated_return)}, p_up {f.p_up:.2f}, "
                   f"herding {f.herding:.2f}, overshoot {f.overshoot:.2f}")
        out.append(f"- Swarm flow by persona: {flow}")
    out += [f"- {r}" for r in d.reasons]
    return out


def batch_report(b: DecisionBatch, top: int = 8) -> str:
    lines = [f"### {b.as_of}"]
    for n in b.risk_notes:
        lines.append(f"> Risk: {n}")
    for t, why in b.exits:
        lines.append(f"- Exit {t}: {why}")
    for o in b.orders:
        lines.append(f"- Order {o.ticker} {o.qty:+d} shares ({o.reason})")
    ranked = sorted(b.decisions, key=lambda d: -d.conviction)[:top]
    for d in ranked:
        lines += [""] + decision_block(d)
    return "\n".join(lines)


def backtest_report(res: BacktestResult, title: str = "Backtest", recent: int = 5,
                    comparison: dict[str, dict] | None = None, notes: list[str] | None = None) -> str:
    m = res.metrics
    lines = [f"# {title}", ""]
    lines += notes or []
    lines += ["", "## Results", "", "| Metric | Strategy | QQQ |", "|---|---|---|",
              f"| Total return | {m['total_return']:+.1%} | {m['benchmark_return']:+.1%} |",
              f"| Sharpe | {m['sharpe']:.2f} | {m['benchmark_sharpe']:.2f} |",
              f"| CAGR | {m['cagr']:+.1%} | |",
              f"| Annual vol | {m['ann_vol']:.1%} | |",
              f"| Max drawdown | {m['max_drawdown']:.1%} | |",
              f"| Correlation to QQQ | {m['correlation_to_benchmark']:.2f} | |",
              f"| Information ratio | {m['information_ratio']:.2f} | |",
              f"| Round trips | {m['trades']} | |",
              f"| Hit rate | {m['hit_rate']:.0%} | |",
              f"| Avg trade PnL | ${m['avg_trade_pnl']:,.0f} | |",
              f"| Fees | ${m['fees']:,.0f} | |",
              f"| Swarm weights (continuation, gap) | {m['calibration_continuation']:.2f}, "
              f"{m['calibration_gap']:.2f} ({m['calibration_samples']} samples) | |"]
    if comparison:
        lines += ["", "## What each layer adds", "",
                  "| Variant | Return | Sharpe | Max DD | Trades | Hit rate |", "|---|---|---|---|---|---|"]
        for name, cm in comparison.items():
            lines.append(f"| {name} | {cm['total_return']:+.1%} | {cm['sharpe']:.2f} | "
                         f"{cm['max_drawdown']:.1%} | {cm['trades']} | {cm['hit_rate']:.0%} |")
    if res.trades:
        best = sorted(res.trades, key=lambda t: -t.pnl)[:5]
        worst = sorted(res.trades, key=lambda t: t.pnl)[:5]
        lines += ["", "## Best and worst trades", "", "| Ticker | Side | Opened | Closed | PnL | Exit |",
                  "|---|---|---|---|---|---|"]
        for t in best + worst:
            lines.append(f"| {t.ticker} | {'long' if t.side > 0 else 'short'} | {t.opened} | {t.closed} | "
                         f"${t.pnl:,.0f} | {t.reason} |")
    active = [b for b in res.batches if b.orders][-recent:]
    if active:
        lines += ["", "## Recent decision cycles", ""]
        for b in active:
            lines += [batch_report(b), ""]
    return "\n".join(lines)
