"""Network and host components: exchange, links, Arista switch models, trading hosts."""
from __future__ import annotations

import random

from .engine import Latency, Packet, Port, Sim


class Link:
    """A cable. Fiber ~4.9 ns/m, copper DAC ~4.3 ns/m, hollow-core fiber ~3.4 ns/m."""

    def __init__(self, sim: Sim, name: str, length_m: float, ns_per_m: float, dst) -> None:
        self.sim, self.name, self.dst = sim, name, dst
        self.prop_ns = length_m * ns_per_m

    def send(self, pkt: Packet) -> None:
        self.sim.after(self.prop_ns, self._arrive, pkt)

    def _arrive(self, pkt: Packet) -> None:
        pkt.mark(self.name, self.sim.now)
        self.dst.receive(pkt)


class Switch:
    """Generic switch model, parameterised to represent each Arista platform.

    - Layer 1 (7130 L1 crosspoint): a few ns, no buffering, no contention. Each
      input is a physical circuit to its output(s); fan-out is free replication.
    - MetaMux (7130 FPGA mux): tens of ns, N:1 merge so frames contend for egress.
    - 7050X3 (cut-through L2/L3): hundreds of ns, egress contention.
    - 7280R3 (deep-buffer VOQ, effectively store-and-forward here): microseconds.
    """

    def __init__(self, sim: Sim, name: str, spec: dict, rng: random.Random) -> None:
        self.sim, self.name, self.rng = sim, name, rng
        self.model = spec.get("model", name)
        self.latency = Latency.of(spec["latency"])
        self.cut_through = spec.get("cut_through", True)
        self.contention = spec.get("contention", True)
        self.gbps = spec.get("gbps", 10.0)
        self.routes: dict[str, list] = {}

    def add_route(self, kind: str, link: Link) -> None:
        port = Port(self.gbps) if self.contention else None
        self.routes.setdefault(kind, []).append((port, link))

    def receive(self, pkt: Packet) -> None:
        lat = self.latency.sample(self.rng)
        if not self.cut_through:
            lat += pkt.size * 8 / self.gbps  # wait for the last bit before forwarding
        # Claim the egress port when the frame is ready, not when it arrived, so frames
        # contend in the order they reach the port.
        self.sim.after(lat, self._egress, pkt)

    def _egress(self, pkt: Packet) -> None:
        ready = self.sim.now
        routes = self.routes[pkt.kind]
        for i, (port, link) in enumerate(routes):
            p = pkt if i == len(routes) - 1 else pkt.clone()
            if pkt.kind == "md":
                p.server = i
            else:
                p.server, p.t_nic_in, p.t_nic_out = pkt.server, pkt.t_nic_in, pkt.t_nic_out
            start = port.reserve(ready, p.size) if port else ready
            p.mark(f"{self.name}", ready)
            p.mark(f"{self.name} queue", start)
            self.sim.at(start, link.send, p)


class TradingHost:
    """A strategy box: software server, FPGA NIC, or FPGA inside the switch.

    pre_stages run on every market-data frame (rx, decode). post_stages run only
    when the frame carries a trading signal (strategy, order build, tx).
    ii_ns is the per-frame occupancy of the processing element: a CPU core is
    busy for microseconds per frame, so bursts queue; an FPGA pipeline accepts a
    new frame every few clock cycles, so it essentially never queues.
    """

    def __init__(self, sim: Sim, idx: int, spec: dict, rng: random.Random, order_link_factory) -> None:
        self.sim, self.idx, self.rng = sim, idx, rng
        self.name = spec["name"]
        self.pre = [(n, Latency.of(d)) for n, d in spec["pre_stages"]]
        self.post = [(n, Latency.of(d)) for n, d in spec["post_stages"]]
        self.ii = Latency.of(spec["ii_ns"])
        self.order_size = spec.get("order_size", 128)
        self.port = Port(spec.get("gbps", 10.0))
        self.core_busy_until = 0.0
        self.busy_ns = 0.0
        self.order_link: Link = order_link_factory(self)

    def receive(self, pkt: Packet) -> None:
        now = self.sim.now
        start = max(now, self.core_busy_until)
        occ = self.ii.sample(self.rng)
        self.core_busy_until = start + occ
        self.busy_ns += occ
        pkt.mark("host queue", start)
        t = start
        for name, lat in self.pre:
            t += lat.sample(self.rng)
            pkt.mark(name, t)
        if not pkt.signal:
            return
        order = Packet("order", self.order_size, pkt.feed, pkt.seq, True, pkt.t0)
        order.marks = list(pkt.marks)
        order.server = self.idx
        order.t_nic_in = now
        for name, lat in self.post:
            t += lat.sample(self.rng)
            order.mark(name, t)
        self.sim.at(t, self._egress, order)

    def _egress(self, order: Packet) -> None:
        out = self.port.reserve(self.sim.now, order.size)
        order.mark("host NIC egress", out)
        order.t_nic_out = out
        self.sim.at(out, self.order_link.send, order)


class Exchange:
    """Publishes market data feeds and timestamps arriving orders at its gateway."""

    def __init__(self, sim: Sim, gbps: float = 10.0) -> None:
        self.sim = sim
        self.gbps = gbps
        self.orders: list[Packet] = []

    def publish(self, feeds: list[list[tuple[float, int, bool]]], links: list[Link]) -> int:
        n = 0
        for f, (events, link) in enumerate(zip(feeds, links)):
            port = Port(self.gbps)
            for seq, (t, size, signal) in enumerate(events):
                start = port.reserve(t, size)
                pkt = Packet("md", size, f, seq, signal, start)
                pkt.mark("exchange publish", start)
                self.sim.at(start, link.send, pkt)
                n += 1
        return n

    def receive(self, pkt: Packet) -> None:
        pkt.t_exchange = self.sim.now
        self.orders.append(pkt)
