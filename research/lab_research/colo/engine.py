"""Minimal discrete-event engine and latency primitives. All times are in nanoseconds."""
from __future__ import annotations

import heapq
import itertools
import math
import random
from dataclasses import dataclass, field


class Sim:
    def __init__(self) -> None:
        self.now = 0.0
        self._q: list = []
        self._seq = itertools.count()

    def at(self, t: float, fn, *args) -> None:
        heapq.heappush(self._q, (t, next(self._seq), fn, args))

    def after(self, delay: float, fn, *args) -> None:
        self.at(self.now + delay, fn, *args)

    def run(self) -> None:
        while self._q:
            t, _, fn, args = heapq.heappop(self._q)
            self.now = t
            fn(*args)


@dataclass
class Latency:
    """A latency distribution.

    mean/sd: body of the distribution (ns). dist: "normal" for hardware (tight,
    symmetric jitter) or "lognormal" for software (right-skewed). tail_p/tail_ns:
    probability of a rare stall (IRQ, context switch, cache miss storm) adding a
    uniform delay in [lo, hi].
    """

    mean: float
    sd: float = 0.0
    dist: str = "normal"
    tail_p: float = 0.0
    tail_ns: tuple[float, float] = (0.0, 0.0)
    points: tuple[tuple[float, float], ...] = ()  # dist "empirical": (percentile, ns), sorted

    @classmethod
    def of(cls, spec) -> "Latency":
        if isinstance(spec, Latency):
            return spec
        if isinstance(spec, (int, float)):
            return cls(float(spec))
        spec = dict(spec)
        if "tail_ns" in spec:
            spec["tail_ns"] = tuple(spec["tail_ns"])
        if "points" in spec:
            spec["points"] = tuple(tuple(p) for p in spec["points"])
        return cls(**spec)

    def sample(self, rng: random.Random) -> float:
        if self.dist == "empirical":
            return self._empirical(rng.random() * 100)
        if self.sd <= 0:
            v = self.mean
        elif self.dist == "lognormal":
            s2 = math.log(1 + (self.sd / self.mean) ** 2)
            v = rng.lognormvariate(math.log(self.mean) - s2 / 2, math.sqrt(s2))
        else:
            v = max(self.mean * 0.5, rng.gauss(self.mean, self.sd))
        if self.tail_p and rng.random() < self.tail_p:
            v += rng.uniform(*self.tail_ns)
        return v


    def _empirical(self, u: float) -> float:
        pts = self.points
        for (p0, v0), (p1, v1) in zip(pts, pts[1:]):
            if u <= p1:
                return v0 if p1 == p0 else v0 + (v1 - v0) * (u - p0) / (p1 - p0)
        return pts[-1][1]


class Port:
    """An egress serialiser. Frames leave one at a time at line rate (Gbps == bits/ns)."""

    def __init__(self, gbps: float) -> None:
        self.gbps = gbps
        self.busy_until = 0.0

    def ser_ns(self, size: int) -> float:
        return size * 8 / self.gbps

    def reserve(self, ready: float, size: int) -> float:
        """Return the time the first bit of the frame leaves the port."""
        start = max(ready, self.busy_until)
        self.busy_until = start + self.ser_ns(size)
        return start


@dataclass
class Packet:
    kind: str  # "md" (market data) or "order"
    size: int
    feed: int
    seq: int
    signal: bool = False
    t0: float = 0.0  # first bit leaves the exchange
    marks: list = field(default_factory=list)  # [(stage, time first bit leaves stage)]
    server: int = -1
    t_nic_in: float = 0.0
    t_nic_out: float = 0.0
    t_exchange: float = 0.0

    def mark(self, stage: str, t: float) -> None:
        self.marks.append((stage, t))

    def clone(self) -> "Packet":
        p = Packet(self.kind, self.size, self.feed, self.seq, self.signal, self.t0)
        p.marks = list(self.marks)
        return p
