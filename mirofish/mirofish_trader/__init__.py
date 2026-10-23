"""MiroFish Trader: news and sentiment driven decisions for Nasdaq stocks.

Two multi-agent systems joined into one pipeline:

- ``sentiment`` (BettaFish): independent analyst engines read every news item
  and a forum reconciles them into one signal per stock and day.
- ``swarm`` (MiroFish): a population of simulated traders reacts to that
  signal over several rounds; the emergent price path says whether the move
  is already priced, will overshoot, or will drift.

``Pipeline.decide`` fuses them, sizes positions through ``RiskManager`` and
returns orders for a paper broker. There is no live trading.
"""

__version__ = "0.1.0"
