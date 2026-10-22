"""EventEngine: what kind of event an item is, and how big (DESIGN.md 4.3).

Rules are tried in order and the first match wins, so specific patterns
("cuts guidance") sit above generic ones ("guidance"). ``sign`` is the usual
direction of the event for the company named; ``None`` means the event type
says nothing about direction and the engine abstains on the score.
"""

from __future__ import annotations

import re
from dataclasses import dataclass

from ..models import EngineView


@dataclass(frozen=True)
class EventRule:
    event_type: str
    pattern: re.Pattern
    sign: float | None
    magnitude: float  # typical absolute move, fraction
    horizon_days: int
    confidence: float


def _r(event_type: str, pattern: str, sign: float | None, magnitude: float, horizon: int,
       confidence: float) -> EventRule:
    return EventRule(event_type, re.compile(pattern, re.I), sign, magnitude, horizon, confidence)


RULES: list[EventRule] = [
    _r("guidance_cut", r"\b(cuts?|lowers?|slash(es)?|trims?|reduces?|weak|soft)\b.{0,30}\b(guidance|outlook|forecast)\b|\bwarns\b", -1, 0.045, 5, 0.85),
    _r("guidance_raise", r"\b(raises?|lifts?|boosts?|hikes?|strong|ups)\b.{0,30}\b(guidance|outlook|forecast)\b", 1, 0.04, 5, 0.85),
    _r("earnings_miss", r"\b(miss(es|ed)?|falls? short|below|worse[- ]than[- ]expected)\b.{0,40}\b(estimates?|expectations|forecasts?|consensus)\b|\bdisappointing (results|quarter|earnings)\b", -1, 0.04, 4, 0.85),
    _r("earnings_beat", r"\b(beats?|tops?|exceed(s|ed)?|above|better[- ]than[- ]expected|crush(es)?)\b.{0,40}\b(estimates?|expectations|forecasts?|consensus)\b|\brecord (revenue|quarter|profit)\b", 1, 0.035, 4, 0.85),
    _r("earnings", r"\b(earnings|quarterly results|q[1-4] results|reports? (first|second|third|fourth)[- ]quarter)\b", None, 0.03, 3, 0.4),
    _r("downgrade", r"\bdowngrad(e|es|ed)\b|\bprice target (cut|lowered)\b|\b(cuts?|lowers?) (its |the )?price target\b|\bto (sell|underperform|underweight)\b", -1, 0.018, 3, 0.75),
    _r("upgrade", r"\bupgrad(e|es|ed)\b|\bprice target (raised|hiked)\b|\b(raises?|lifts?) (its |the )?price target\b|\bto (buy|outperform|overweight)\b|\binitiat(es|ed) .{0,20}(buy|outperform|overweight)\b", 1, 0.018, 3, 0.75),
    _r("fda_rejection", r"\b(complete response letter|fda (rejects|declines)|clinical hold|trial (fails|failed|halted))\b", -1, 0.08, 5, 0.85),
    _r("fda_approval", r"\b(fda (approves|approval|clears|grants)|approved by the fda|positive (phase|trial)|trial (succeeds|met))\b", 1, 0.07, 5, 0.85),
    _r("mna_target", r"\b(to be acquired|agrees to be (bought|acquired)|takeover (bid|offer) for|buyout offer|receives? (a )?(takeover|buyout))\b", 1, 0.10, 2, 0.8),
    _r("mna_acquirer", r"\b(to (acquire|buy)|agrees to (buy|acquire)|acquisition of|in talks to (buy|acquire))\b", -0.3, 0.02, 3, 0.5),
    _r("buyback", r"\b(buyback|repurchase|raises? (its |the )?dividend|dividend (increase|hike))\b", 1, 0.015, 4, 0.7),
    _r("fraud", r"\b(fraud|accounting irregularit|restat(e|es|ement)|sec charges|short seller report)\b", -1, 0.09, 6, 0.85),
    _r("regulatory_action", r"\b(antitrust|ftc|doj|department of justice|european commission|competition (authority|regulator)|probe|investigation|subpoena|fined?|penalty)\b", -1, 0.025, 5, 0.7),
    _r("litigation", r"\b(lawsuit|sued|sues|class action|verdict|jury|patent (suit|infringement))\b", -1, 0.015, 4, 0.6),
    _r("product_issue", r"\b(recall(s|ed)?|outage|breach|hack(ed)?|defect|delay(s|ed)?|shortage|production halt)\b", -1, 0.02, 3, 0.65),
    _r("product_launch", r"\b(launch(es|ed)?|unveil(s|ed)?|introduc(es|ed)|releases?|debuts?|partnership|partners with|wins? (a )?(contract|deal))\b", 1, 0.012, 3, 0.5),
    _r("exec_change", r"\b(ceo|cfo|chief executive|chief financial)\b.{0,30}\b(resigns?|steps down|departs?|ousted|fired|retires?|exits?)\b", -1, 0.03, 4, 0.7),
    _r("layoffs", r"\b(layoffs?|job cuts|cut(s|ting)? .{0,15}jobs|workforce reduction)\b", 0.2, 0.01, 3, 0.4),
]


class EventEngine:
    name = "event"

    def __init__(self, rules: list[EventRule] | None = None):
        self.rules = rules or RULES

    def classify(self, text: str) -> EventRule | None:
        for rule in self.rules:
            if rule.pattern.search(text):
                return rule
        return None

    def assess(self, text: str) -> EngineView:
        rule = self.classify(text)
        if rule is None:
            return EngineView(self.name, 0.0, 0.1, "general", 0.01, 2, rationale="no event pattern")
        score = 0.0 if rule.sign is None else 0.8 * rule.sign
        conf = rule.confidence if rule.sign is not None else 0.1
        return EngineView(self.name, score, conf, rule.event_type, rule.magnitude, rule.horizon_days,
                          rationale=rule.event_type)
