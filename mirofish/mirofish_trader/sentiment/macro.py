"""MacroPolicyEngine: political and macro news as theme shocks (DESIGN.md 4.4).

This is where the political-analysis heritage lives. A policy headline rarely
names a stock; it names a policy. The engine turns it into signed shocks on
themes (``universe.THEMES``), and each company's sector exposure turns the
shocks into a per-ticker view, so one export-control headline reaches every
chip name with a weight that depends on how exposed it is.

A positive shock is good for exposed companies (tariff relief, rate cuts,
subsidies); a negative one is bad (new tariffs, a breakup ruling).
"""

from __future__ import annotations

import math
import re

from ..models import EngineView
from ..universe import Company

_P = lambda s: re.compile(s, re.I)  # noqa: E731

# (theme, pattern, shock). All matching patterns add up.
THEME_RULES: list[tuple[str, re.Pattern, float]] = [
    ("tariffs", _P(r"\b(new|higher|raises?|imposes?|threatens?|hikes?|sweeping|retaliatory)\b.{0,25}\btariffs?\b|\btariffs? (hike|increase|threat|escalat)|\btrade war\b"), -1.0),
    ("tariffs", _P(r"\btariffs?\b.{0,25}\b(relief|pause|paused|exemption|exempts?|cut|lowered|rolled back|suspend)|\btrade (deal|truce|agreement)\b|\b(lifts?|drops?|delays?) .{0,15}tariffs?\b"), 1.0),
    ("export_controls", _P(r"\b(tighten|new|expand|stricter|broader)\w*\b.{0,30}\b(export (controls?|restrictions?|curbs?|ban)|chip (curbs|ban|restrictions))|\bentity list\b|\b(bans?|blocks?|restricts?) .{0,25}(chip|semiconductor) (exports?|sales)"), -1.0),
    ("export_controls", _P(r"\b(eases?|loosen|lifts?|relax)\w*\b.{0,25}\b(export|chip) (controls?|restrictions?|curbs?|ban)|\bexport licen[cs]es? (granted|approved)"), 1.0),
    ("china", _P(r"\bchina\b.{0,30}\b(stimulus|reopens?|eases?|approves?|demand (rebounds?|recovers?))"), 1.0),
    ("china", _P(r"\b(china|beijing)\b.{0,30}\b(retaliat\w*|crackdown|bans?|probe|sanctions?|tensions?|invade|blockade|slowdown)|\btaiwan (strait|tensions|blockade)"), -1.0),
    ("antitrust", _P(r"\b(break ?up|breakup|antitrust (ruling|suit|lawsuit|case) against|doj sues|ftc sues|monopoly ruling|digital markets act fine|eu fines?)\b"), -1.0),
    ("antitrust", _P(r"\b(antitrust (case|suit) (dismissed|dropped)|wins antitrust|judge rejects (breakup|doj)|merger (approved|cleared))\b"), 1.0),
    ("rates", _P(r"\b(fed|federal reserve|fomc|powell)\b.{0,40}\b(cuts?|lowers?|dovish|eases?|pause hikes)|\brate cuts?\b|\bcooler[- ]than[- ]expected inflation|\binflation (cools|eases|slows)|\byields? (fall|drop|tumble|slide)"), 1.0),
    ("rates", _P(r"\b(fed|federal reserve|fomc|powell)\b.{0,40}\b(hikes?|raises?|hawkish|tighten)|\brate hikes?\b|\bhot(ter)?[- ]than[- ]expected (inflation|cpi)|\binflation (accelerates|jumps|surges)|\byields? (surge|jump|spike|climb)"), -1.0),
    ("election", _P(r"\b(government shutdown|debt ceiling (standoff|crisis)|contested election|political (crisis|turmoil)|impeachment)\b"), -1.0),
    ("election", _P(r"\b(stimulus (bill|package) (passes|signed)|tax cuts? (passes|signed|extended)|shutdown averted|budget deal|spending bill (passes|signed))\b"), 1.0),
    ("drug_pricing", _P(r"\b(drug pric(e|ing) (caps?|controls?|negotiation|crackdown)|medicare (negotiation|price)|most[- ]favou?red[- ]nation)\b"), -1.0),
    ("industrial_policy", _P(r"\b(chips act|subsidy|subsidies|grant|tax credits?|incentives?)\b.{0,30}\b(awarded|approved|expanded|boost|receives?|wins?)|\b(awarded|receives?|wins?) .{0,30}(chips act|subsid|grant)"), 1.0),
    ("industrial_policy", _P(r"\b(repeal|cut|claw ?back|cancel|freeze)\w*\b.{0,30}\b(chips act|subsid|ev tax credits?|tax credits?|grants?)"), -1.0),
    ("ai_capex", _P(r"\b(raises?|boosts?|increases?|doubles?|record|massive|ramps?)\b.{0,30}\b(capex|capital spending|ai (spending|infrastructure|investment)|data cent(er|re) (spending|build))|\bstargate\b"), 1.0),
    ("ai_capex", _P(r"\b(cuts?|pauses?|slows?|scales? back|delays?)\b.{0,30}\b(capex|capital spending|ai (spending|infrastructure)|data cent(er|re)s?)|\bai bubble\b"), -1.0),
    ("consumer", _P(r"\b(strong|robust|blowout|better[- ]than[- ]expected) (jobs|payrolls|retail sales|consumer spending)|consumer confidence (rises|jumps|improves)"), 1.0),
    ("consumer", _P(r"\b(weak|soft|disappointing|worse[- ]than[- ]expected) (jobs|payrolls|retail sales|consumer spending)|\brecession (fears?|warning|risk)|consumer (confidence|sentiment) (falls|drops|slumps|plunges)|\blayoffs? (surge|spike)"), -1.0),
]


class MacroPolicyEngine:
    name = "macro"

    def __init__(self, rules: list[tuple[str, re.Pattern, float]] | None = None):
        self.rules = rules or THEME_RULES

    def themes(self, text: str) -> dict[str, float]:
        out: dict[str, float] = {}
        for theme, pat, shock in self.rules:
            if pat.search(text):
                out[theme] = out.get(theme, 0.0) + shock
        return {t: max(-1.0, min(1.0, s)) for t, s in out.items() if s != 0.0}

    def assess(self, text: str) -> EngineView:
        th = self.themes(text)
        if not th:
            return EngineView(self.name, 0.0, 0.0, "general", rationale="no policy or macro theme")
        return EngineView(self.name, 0.0, 0.7, "macro_policy", 0.012, 4, th,
                          rationale=", ".join(f"{k}{v:+.0f}" for k, v in th.items()))

    @staticmethod
    def for_company(view: EngineView, company: Company) -> EngineView:
        """The text-level theme view, projected onto one company's exposures."""
        if not view.themes:
            return view
        raw = sum(shock * company.exposure(theme) for theme, shock in view.themes.items())
        exposure = max((company.exposure(t) for t in view.themes), default=0.0)
        return EngineView(view.engine, math.tanh(raw), view.confidence * min(1.0, exposure),
                          view.event_type, view.magnitude * max(0.5, exposure), view.horizon_days,
                          view.themes, view.rationale)
