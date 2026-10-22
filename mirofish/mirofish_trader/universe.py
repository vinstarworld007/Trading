"""The Nasdaq universe: names, aliases, sectors, theme exposures and peers.

``retail_share`` is a prior for how much of a name's news reaction comes from
retail and fast money rather than institutions; the swarm sizes its personas
from it. ``beta`` is to QQQ. Both are rough priors, not fitted values.
"""

from __future__ import annotations

from dataclasses import dataclass, field

BENCHMARK = "QQQ"

THEMES = (
    "tariffs",  # trade war, import duties
    "export_controls",  # chip export limits, sanctions, entity list
    "china",  # China demand, regulation, geopolitics
    "antitrust",  # DOJ/FTC/EU competition action, platform regulation
    "rates",  # Fed, yields; positive shock = easier policy
    "election",  # elections, legislation, government shutdown, fiscal
    "drug_pricing",  # Medicare negotiation, price caps
    "industrial_policy",  # subsidies, CHIPS Act, IRA credits
    "ai_capex",  # AI infrastructure spending cycle
    "consumer",  # consumer spending, inflation, jobs
)

# Sector -> theme -> sensitivity. A theme shock s moves a sector by s * sensitivity.
SECTOR_EXPOSURE: dict[str, dict[str, float]] = {
    "semis": {"tariffs": 0.8, "export_controls": 1.0, "china": 0.8, "rates": 0.6,
              "industrial_policy": 0.8, "ai_capex": 1.0, "election": 0.3},
    "semi_equipment": {"tariffs": 0.7, "export_controls": 1.0, "china": 0.9, "rates": 0.5,
                       "industrial_policy": 0.9, "ai_capex": 0.8},
    "hardware": {"tariffs": 1.0, "china": 0.9, "export_controls": 0.3, "rates": 0.4,
                 "consumer": 0.6, "antitrust": 0.4},
    "software": {"rates": 0.8, "ai_capex": 0.6, "antitrust": 0.3, "election": 0.2},
    "internet": {"antitrust": 1.0, "rates": 0.7, "ai_capex": 0.5, "consumer": 0.4,
                 "election": 0.3, "china": 0.2},
    "internet_china": {"china": 1.0, "tariffs": 0.9, "export_controls": 0.4, "consumer": 0.5,
                       "election": 0.4},
    "ecommerce": {"consumer": 0.9, "tariffs": 0.6, "antitrust": 0.6, "rates": 0.6,
                  "ai_capex": 0.4},
    "autos_ev": {"tariffs": 0.7, "china": 0.7, "rates": 0.9, "industrial_policy": 0.8,
                 "election": 0.6, "consumer": 0.5},
    "media": {"consumer": 0.7, "rates": 0.5},
    "retail": {"consumer": 1.0, "tariffs": 0.5, "rates": 0.3},
    "staples": {"consumer": 0.5, "tariffs": 0.3, "rates": 0.2},
    "biotech": {"drug_pricing": 1.0, "rates": 0.7, "election": 0.5},
    "fintech": {"rates": 1.0, "consumer": 0.6, "antitrust": 0.3},
    "networking": {"ai_capex": 0.6, "tariffs": 0.5, "rates": 0.4, "china": 0.3},
}


@dataclass(frozen=True)
class Company:
    ticker: str
    name: str
    sector: str
    aliases: tuple[str, ...] = ()
    beta: float = 1.0
    retail_share: float = 0.3
    peers: tuple[str, ...] = ()
    popularity: float = 0.5  # how much attention the crowd pays, 0..1
    extra_exposure: dict[str, float] = field(default_factory=dict)

    def exposure(self, theme: str) -> float:
        if theme in self.extra_exposure:
            return self.extra_exposure[theme]
        return SECTOR_EXPOSURE.get(self.sector, {}).get(theme, 0.0)


_COMPANIES = [
    Company("AAPL", "Apple", "hardware", ("Apple Inc", "iPhone", "Tim Cook", "App Store", "iPad"),
            1.05, 0.35, ("QCOM", "AVGO"), 1.0),
    Company("MSFT", "Microsoft", "software", ("Azure", "Satya Nadella", "Windows", "Xbox", "Copilot"),
            1.0, 0.2, ("GOOGL", "AMZN"), 0.9, {"ai_capex": 0.9, "antitrust": 0.5}),
    Company("NVDA", "Nvidia", "semis", ("NVIDIA", "Jensen Huang", "GeForce", "CUDA", "Blackwell"),
            1.6, 0.5, ("AMD", "AVGO", "MU"), 1.0, {"ai_capex": 1.3}),
    Company("AMZN", "Amazon", "ecommerce", ("Amazon.com", "AWS", "Andy Jassy", "Prime Video"),
            1.15, 0.3, ("MSFT", "GOOGL", "COST"), 0.9),
    Company("GOOGL", "Alphabet", "internet", ("Google", "YouTube", "Sundar Pichai", "Gemini", "Waymo"),
            1.1, 0.25, ("META", "MSFT"), 0.85),
    Company("META", "Meta Platforms", "internet", ("Meta", "Facebook", "Instagram", "WhatsApp",
                                                   "Mark Zuckerberg"),
            1.25, 0.3, ("GOOGL",), 0.85),
    Company("TSLA", "Tesla", "autos_ev", ("Elon Musk", "Cybertruck", "Model Y", "Gigafactory"),
            1.9, 0.8, ("NVDA",), 1.0),
    Company("AVGO", "Broadcom", "semis", ("Hock Tan", "VMware"), 1.3, 0.25, ("NVDA", "QCOM", "AAPL"),
            0.6),
    Company("AMD", "Advanced Micro Devices", "semis", ("AMD", "Lisa Su", "Ryzen", "Radeon", "Instinct"),
            1.7, 0.55, ("NVDA", "INTC"), 0.8),
    Company("QCOM", "Qualcomm", "semis", ("Snapdragon", "Cristiano Amon"), 1.25, 0.3, ("AAPL", "AVGO"),
            0.5, {"ai_capex": 0.4}),
    Company("INTC", "Intel", "semis", ("Pat Gelsinger", "Lip-Bu Tan", "Intel Foundry"), 1.2, 0.5,
            ("AMD", "NVDA"), 0.6, {"ai_capex": 0.3, "industrial_policy": 1.2}),
    Company("MU", "Micron", "semis", ("Micron Technology", "Sanjay Mehrotra"), 1.6, 0.4,
            ("NVDA",), 0.5),
    Company("ASML", "ASML", "semi_equipment", ("ASML Holding", "Christophe Fouquet", "EUV"), 1.3, 0.2,
            ("NVDA", "INTC"), 0.4),
    Company("ADBE", "Adobe", "software", ("Photoshop", "Firefly", "Shantanu Narayen"), 1.1, 0.2,
            ("MSFT",), 0.4),
    Company("CSCO", "Cisco", "networking", ("Cisco Systems", "Chuck Robbins", "Webex"), 0.85, 0.15,
            ("AVGO",), 0.35),
    Company("NFLX", "Netflix", "media", ("Ted Sarandos",), 1.2, 0.35, ("META",), 0.7),
    Company("COST", "Costco", "retail", ("Costco Wholesale",), 0.75, 0.15, ("AMZN",), 0.5),
    Company("PEP", "PepsiCo", "staples", ("Pepsi", "Frito-Lay"), 0.45, 0.1, (), 0.35),
    Company("SBUX", "Starbucks", "retail", ("Brian Niccol",), 0.9, 0.3, (), 0.5,
            {"china": 0.5}),
    Company("AMGN", "Amgen", "biotech", (), 0.55, 0.15, ("GILD",), 0.3),
    Company("GILD", "Gilead", "biotech", ("Gilead Sciences",), 0.5, 0.15, ("AMGN",), 0.3),
    Company("MRNA", "Moderna", "biotech", (), 1.4, 0.7, ("AMGN",), 0.6),
    Company("PYPL", "PayPal", "fintech", ("Venmo",), 1.25, 0.45, (), 0.5),
    Company("PDD", "PDD Holdings", "internet_china", ("Temu", "Pinduoduo"), 1.3, 0.6, ("AMZN",), 0.6),
]

UNIVERSE: dict[str, Company] = {c.ticker: c for c in _COMPANIES}


def tickers() -> list[str]:
    return list(UNIVERSE)


def get(ticker: str) -> Company | None:
    return UNIVERSE.get(ticker.upper())
