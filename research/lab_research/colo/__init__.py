"""Colo network model for the lab: options C, D and G on Arista 7130 L1 + MetaMux.

The lab runs on loopback in one VM, so it has no switches, cables or NICs. This
package is a discrete-event model of the path the lab's firm would sit on in an
exchange colo, at nanosecond resolution:

    exchange feed -50 m fiber-> 7130 L1 fan-out -2 m DAC-> trader hosts
        -2 m DAC-> 7130 MetaMux -50 m fiber-> exchange order gateway

The options differ in the host: C is a kernel-bypass server, D an FPGA feed
handler in front of a CPU strategy, G the strategy in the 7130's own FPGA.
Host processing comes from catalog.py or, for C and D, from the trader's own
measured latency (see option_scenarios()).
"""

import re

from .catalog import HOSTS, SWITCHES
from .model import DEFAULT_TRAFFIC, head_to_head, run_scenario

_L1_METAMUX = {"md_switch": "arista_7130_l1", "order_switch": "arista_7130_metamux"}

# The three colo designs the lab can enable. All share the 7130 L1 fan-out and MetaMux; they
# differ in where the feed is decoded and where the decision is made.
OPTIONS = {
    "C": {**_L1_METAMUX, "id": "C", "label": "Kernel bypass on 7130 L1 + MetaMux",
          "summary": "Busy-polling userspace hosts. Feeds fan out on Layer 1 circuits and MetaMux "
                     "merges orders toward the exchange.",
          "host": "kernel_bypass"},
    "D": {**_L1_METAMUX, "id": "D", "label": "FPGA feed handler + CPU strategy",
          "summary": "An FPGA NIC decodes and filters the feed and builds books in hardware, then "
                     "DMAs updates to the CPU, which decides and sends through kernel bypass.",
          "host": "fpga_feed_cpu_strategy"},
    "G": {**_L1_METAMUX, "id": "G", "label": "Strategy inside the 7130 FPGA",
          "summary": "The strategy runs in the 7130's user FPGA: no NIC, no rack cables, only "
                     "on-board traces between the L1 crosspoint, the strategy and MetaMux.",
          "host": "switch_fpga_app",
          "cables": {"md_to_host": [0.1, "pcb"], "host_to_order": [0.1, "pcb"]}},
}

_LINE = (r"\s+n=(\d+)\s+min=(\d+) p50=(\d+) p90=(\d+) p99=(\d+) p99\.9=(\d+) "
         r"p99\.99=(\d+) max=(\d+)")
_TICK_TO_WIRE = re.compile(r"^Tick to wire" + _LINE, re.M)
_DECIDE_TO_WIRE = re.compile(r"^Decide to wire" + _LINE, re.M)


def _parse(pattern, label: str, text: str) -> dict:
    m = pattern.search(text)
    if not m:
        raise ValueError(f"no '{label}' line in the trader output")
    n, lo, p50, p90, p99, p999, p9999, hi = map(int, m.groups())
    if n == 0:
        raise ValueError(f"trader recorded no '{label}' samples (it sent no orders)")
    return {"n": n, "min": lo, "p50": p50, "p90": p90, "p99": p99, "p99.9": p999,
            "p99.99": p9999, "max": hi}


def parse_tick_to_wire(text: str) -> dict:
    """Read the "Tick to wire" line that trader prints at exit (ns)."""
    return _parse(_TICK_TO_WIRE, "Tick to wire", text)


def parse_decide_to_wire(text: str) -> dict:
    """Read the "Decide to wire" line that trader prints at exit (ns)."""
    return _parse(_DECIDE_TO_WIRE, "Decide to wire", text)


def _empirical(h: dict) -> dict:
    points = [(0, h["min"]), (50, h["p50"]), (90, h["p90"]), (99, h["p99"]),
              (99.9, h["p99.9"]), (99.99, h["p99.99"]), (100, h["max"])]
    return {"dist": "empirical", "mean": h["p50"], "points": points}


def trader_host(t2w: dict) -> dict:
    """An option C host whose decision time is the trader's measured tick-to-wire.

    trader measures from a feed packet leaving recvmmsg to its order being handed
    to the socket, so the model keeps the kernel-bypass NIC rx and tx stages
    around it: those are the parts a loopback lab cannot measure.
    """
    bypass = HOSTS["kernel_bypass"]
    return {
        "name": "Kernel-bypass NIC + lab trader (measured)",
        "ii_ns": bypass["ii_ns"],
        "pre_stages": [bypass["pre_stages"][0]],
        "post_stages": [("lab trader: feed to order (measured)", _empirical(t2w)),
                        bypass["post_stages"][-1]],
    }


def trader_host_d(d2w: dict) -> dict:
    """An option D host: FPGA feed handler, then the trader's measured decide-to-wire.

    In option D the FPGA replaces the trader's rx and book threads, so only the
    trader's order path (decision handed to order entry, encoded, sent) is
    measured; the strategy evaluation and NIC tx stay modelled.
    """
    d = HOSTS["fpga_feed_cpu_strategy"]
    return {
        "name": "FPGA feed handler + lab trader order path (measured)",
        "ii_ns": d["ii_ns"],
        "pre_stages": d["pre_stages"],
        "post_stages": [d["post_stages"][0],
                        ("lab trader: decision to order (measured)", _empirical(d2w)),
                        d["post_stages"][-1]],
    }


def parse_options(spec: str) -> list[str]:
    """"C", "C,D,G" or "all" -> option ids, in ladder order."""
    ids = list(OPTIONS) if spec.strip().lower() == "all" else \
        [x.strip().upper() for x in spec.split(",") if x.strip()]
    bad = [x for x in ids if x not in OPTIONS]
    if bad or not ids:
        raise ValueError(f"unknown option {', '.join(bad) or spec!r}; choose from "
                         f"{', '.join(OPTIONS)} or all")
    return [o for o in OPTIONS if o in ids]


def option_scenarios(options: list[str], trader_log: str | None = None) -> list[dict]:
    """The enabled options, each followed by its lab-measured variant when a trader log is given.

    G runs the strategy in the switch FPGA, so the lab's software is not on its
    path and it has no measured variant.
    """
    out = []
    for o in options:
        out.append(OPTIONS[o])
        if not trader_log or o == "G":
            continue
        if o == "C":
            h = parse_tick_to_wire(trader_log)
            host, what = trader_host(h), "tick-to-wire"
        else:
            h = parse_decide_to_wire(trader_log)
            host, what = trader_host_d(h), "decide-to-wire"
        out.append({**OPTIONS[o], "id": f"{o}-lab", "label": f"Option {o} with the lab's trader",
                    "summary": f"Option {o} network, with the trader's measured {what} from this "
                               f"run ({h['n']:,} orders, p50 {h['p50']:,} ns) in the host.",
                    "host": host})
    return out


__all__ = ["DEFAULT_TRAFFIC", "HOSTS", "OPTIONS", "SWITCHES", "head_to_head", "option_scenarios",
           "parse_decide_to_wire", "parse_options", "parse_tick_to_wire", "run_scenario",
           "trader_host", "trader_host_d"]
