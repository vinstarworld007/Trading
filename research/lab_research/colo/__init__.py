"""Colo network model for the lab: option C, kernel-bypass hosts on Arista 7130 L1 + MetaMux.

The lab runs on loopback in one VM, so it has no switches, cables or NICs. This
package is a discrete-event model of the path the lab's firm would sit on in an
exchange colo, at nanosecond resolution:

    exchange feed -50 m fiber-> 7130 L1 fan-out -2 m DAC-> trader hosts (kernel bypass)
        -2 m DAC-> 7130 MetaMux -50 m fiber-> exchange order gateway

Each host's processing time can come from a profile in catalog.py or from the
trader's own measured "Tick to wire" histogram (see trader_host()).
"""

import re

from .catalog import HOSTS, SWITCHES
from .model import DEFAULT_TRAFFIC, head_to_head, run_scenario

OPTION_C = {
    "id": "C",
    "label": "Kernel bypass on 7130 L1 + MetaMux",
    "summary": "Busy-polling userspace hosts. Feeds fan out on Layer 1 circuits and MetaMux "
               "merges orders toward the exchange.",
    "md_switch": "arista_7130_l1", "order_switch": "arista_7130_metamux", "host": "kernel_bypass",
}

_TICK_TO_WIRE = re.compile(
    r"^Tick to wire\s+n=(\d+)\s+min=(\d+) p50=(\d+) p90=(\d+) p99=(\d+) p99\.9=(\d+) "
    r"p99\.99=(\d+) max=(\d+)", re.M)


def parse_tick_to_wire(text: str) -> dict:
    """Read the "Tick to wire" line that trader prints at exit (ns)."""
    m = _TICK_TO_WIRE.search(text)
    if not m:
        raise ValueError("no 'Tick to wire' line in the trader output")
    n, lo, p50, p90, p99, p999, p9999, hi = map(int, m.groups())
    if n == 0:
        raise ValueError("trader recorded no tick-to-wire samples (it sent no orders)")
    return {"n": n, "min": lo, "p50": p50, "p90": p90, "p99": p99, "p99.9": p999,
            "p99.99": p9999, "max": hi}


def trader_host(t2w: dict) -> dict:
    """An option C host whose decision time is the trader's measured tick-to-wire.

    trader measures from a feed packet leaving recvmmsg to its order being handed
    to the socket, so the model keeps the kernel-bypass NIC rx and tx stages
    around it: those are the parts a loopback lab cannot measure.
    """
    bypass = HOSTS["kernel_bypass"]
    points = [(0, t2w["min"]), (50, t2w["p50"]), (90, t2w["p90"]), (99, t2w["p99"]),
              (99.9, t2w["p99.9"]), (99.99, t2w["p99.99"]), (100, t2w["max"])]
    return {
        "name": "Kernel-bypass NIC + lab trader (measured)",
        "ii_ns": bypass["ii_ns"],
        "pre_stages": [bypass["pre_stages"][0]],
        "post_stages": [
            ("lab trader: feed to order (measured)", {"dist": "empirical", "mean": t2w["p50"],
                                                      "points": points}),
            bypass["post_stages"][-1],
        ],
    }


def option_c_scenarios(t2w: dict | None = None) -> list[dict]:
    scenarios = [OPTION_C]
    if t2w:
        scenarios.append({**OPTION_C, "id": "C-lab", "label": "Option C with the lab's trader",
                          "summary": "Option C network, with each host's decision time drawn from "
                                     f"this trader run ({t2w['n']:,} orders, p50 "
                                     f"{t2w['p50']:,} ns).",
                          "host": trader_host(t2w)})
    return scenarios


__all__ = ["DEFAULT_TRAFFIC", "HOSTS", "OPTION_C", "SWITCHES", "head_to_head", "option_c_scenarios",
           "parse_tick_to_wire", "run_scenario", "trader_host"]
