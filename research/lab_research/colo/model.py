"""Build a topology from a scenario, run it, and summarise latency."""
from __future__ import annotations

import copy
import random
from collections import defaultdict

from .catalog import HOSTS, MEDIA_NS_PER_M, SWITCHES
from .components import Exchange, Link, Switch, TradingHost
from .engine import Sim

DEFAULT_TRAFFIC = {
    "feeds": 2,              # independent exchange feeds (e.g. two products / A+B lines)
    "base_pps": 150_000,     # Poisson background rate per feed
    "burst_every_us": 200,   # a microburst arrives this often per feed...
    "burst_size": 30,        # ...as this many back-to-back frames at line rate
    "frame_bytes": [64, 256],
    "trigger_p": 0.05,       # share of frames that carry a tradeable signal
    "duration_ms": 30,
    "seed": 7,
}

DEFAULT_CABLES = {
    "xconnect_md": [50, "fiber"],   # exchange cross-connect into our rack
    "md_to_host": [2, "dac"],
    "host_to_order": [2, "dac"],
    "xconnect_oe": [50, "fiber"],   # back out to the exchange order-entry gateway
}

CATEGORY = {
    "Exchange → MD switch": "Cabling",
    "MD switch → host": "Cabling",
    "Host → order switch": "Cabling",
    "Order switch → exchange": "Cabling",
    "MD switch": "Switching",
    "MD switch queue": "Queueing",
    "Order switch": "Switching",
    "Order switch queue": "Queueing",
    "host queue": "Queueing",
    "host NIC egress": "Queueing",
}


def gen_traffic(t: dict) -> list[list[tuple[float, int, bool]]]:
    rng = random.Random(t["seed"])
    end = t["duration_ms"] * 1e6
    lo, hi = t["frame_bytes"]
    feeds = []
    for _ in range(t["feeds"]):
        times = []
        if t["base_pps"]:
            x = rng.expovariate(t["base_pps"] / 1e9)
            while x < end:
                times.append(x)
                x += rng.expovariate(t["base_pps"] / 1e9)
        if t["burst_size"]:
            x = rng.uniform(0, t["burst_every_us"] * 1e3)
            while x < end:
                times.extend([x] * t["burst_size"])
                x += t["burst_every_us"] * 1e3
        times.sort()
        feeds.append([(x, rng.randint(lo, hi), rng.random() < t["trigger_p"]) for x in times])
    return feeds


def _resolve(name_or_spec, table):
    return copy.deepcopy(table[name_or_spec]) if isinstance(name_or_spec, str) else name_or_spec


def run_scenario(sc: dict, traffic: dict | None = None) -> dict:
    traffic = {**DEFAULT_TRAFFIC, **(traffic or {}), **sc.get("traffic", {})}
    cables = {**DEFAULT_CABLES, **sc.get("cables", {})}
    gbps = sc.get("gbps", 10.0)
    sim = Sim()
    rng = random.Random(traffic["seed"] * 1000 + 1)

    def cable(key, name, dst):
        length, medium = cables[key]
        return Link(sim, name, length, MEDIA_NS_PER_M[medium], dst)

    exchange = Exchange(sim, gbps)
    md_spec = {**_resolve(sc["md_switch"], SWITCHES), "gbps": gbps}
    oe_spec = {**_resolve(sc["order_switch"], SWITCHES), "gbps": gbps}
    host_spec = {**_resolve(sc["host"], HOSTS), "gbps": gbps}
    md_sw = Switch(sim, "MD switch", md_spec, rng)
    oe_sw = Switch(sim, "Order switch", oe_spec, rng)
    oe_sw.add_route("order", cable("xconnect_oe", "Order switch → exchange", exchange))

    hosts = [TradingHost(sim, i, host_spec, rng, lambda h: cable("host_to_order", "Host → order switch", oe_sw))
             for i in range(sc.get("servers", 4))]
    for h in hosts:
        md_sw.add_route("md", cable("md_to_host", "MD switch → host", h))

    feeds = gen_traffic(traffic)
    n_md = exchange.publish(feeds, [cable("xconnect_md", "Exchange → MD switch", md_sw) for _ in feeds])
    sim.run()

    orders = exchange.orders
    t2t = sorted(o.t_nic_out - o.t_nic_in for o in orders)
    e2e = sorted(o.t_exchange - o.t0 for o in orders)
    stage_sum: dict[str, float] = defaultdict(float)
    stage_order: list[str] = []
    for o in orders:
        marks = o.marks
        for (_, t_prev), (stage, t) in zip(marks, marks[1:]):
            if stage not in stage_sum:
                stage_order.append(stage)
            stage_sum[stage] += t - t_prev
    n = max(len(orders), 1)
    breakdown = [{"stage": s, "mean_ns": stage_sum[s] / n, "category": CATEGORY.get(s, "Host compute")}
                 for s in stage_order]
    duration_ns = traffic["duration_ms"] * 1e6
    race = {f"{o.feed}:{o.seq}": o.t_exchange - o.t0 for o in orders if o.server == 0}
    return {
        "id": sc["id"],
        "label": sc["label"],
        "summary": sc.get("summary", ""),
        "md_switch": md_spec["model"],
        "order_switch": oe_spec["model"],
        "host": host_spec["name"],
        "servers": len(hosts),
        "md_frames": n_md,
        "orders": len(orders),
        "host_utilisation": sum(h.busy_ns for h in hosts) / len(hosts) / duration_ns,
        "tick_to_trade": summarise(t2t),
        "wire_to_wire": summarise(e2e),
        "breakdown": breakdown,
        "_race": race,
    }


def pct(xs: list[float], p: float) -> float:
    if not xs:
        return float("nan")
    k = (len(xs) - 1) * p / 100
    f = int(k)
    c = min(f + 1, len(xs) - 1)
    return xs[f] + (xs[c] - xs[f]) * (k - f)


def summarise(xs: list[float]) -> dict:
    return {
        "mean": sum(xs) / len(xs) if xs else float("nan"),
        "min": xs[0] if xs else float("nan"),
        "p50": pct(xs, 50), "p90": pct(xs, 90), "p99": pct(xs, 99), "p999": pct(xs, 99.9),
        "max": xs[-1] if xs else float("nan"),
        "cdf": [round(pct(xs, p), 2) for p in range(0, 101)],
    }


def head_to_head(results: list[dict]) -> list[list[float]]:
    """P(row scenario's order reaches the exchange before column's) for the same tick."""
    m = []
    for a in results:
        row = []
        for b in results:
            keys = a["_race"].keys() & b["_race"].keys()
            wins = sum(a["_race"][k] < b["_race"][k] for k in keys)
            row.append(wins / len(keys) if keys and a is not b else None)
        m.append(row)
    return m
