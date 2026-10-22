"""Research tools for the trading lab: positions and PnL, reconciliation
against the exchange's drop copy, transaction cost analysis, and a simple
market-making backtest on top-of-book data.

Inputs are the CSV files the lab's programs write:

- ``trader --fills-csv``: the firm's fills as its OMS saw them
- ``trader --market-csv``: the top of book the trader saw, per change
- ``dropcopy --csv``: every firm execution as the exchange reported it
- ``itch-dump --tob-csv``: top of book for one symbol from an ITCH day

Prices in every file are integer Price(4) (1/10000 dollar); this package keeps
them as integers and converts to dollars only when reporting.
"""

PRICE_SCALE = 10_000
