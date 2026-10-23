# Research

Python tools that work on what the lab's programs write. Prices stay integer
Price(4) (1/10000 dollar) until they are reported.

```bash
python3 -m venv ~/.venvs/lab && ~/.venvs/lab/bin/pip install -r research/requirements.txt
```

## Post-trade report

```bash
trader ... --fills-csv fills.csv --market-csv market.csv
dropcopy --csv dropcopy.csv
python3 research/report.py --fills fills.csv --market market.csv \
    --dropcopy dropcopy.csv --account FIRM01
```

- **Positions and PnL** per symbol, marked at the last mid.
- **Reconciliation**: every trader fill must match a drop copy execution (same
  match number, UserRefNum, side, quantity, price, symbol) and the reverse. It
  exits 1 on any break.
- **TCA**: for each fill, spread capture against the mid just before it, and
  markouts at 1 ms, 10 ms, 100 ms and 1 s. The drop from capture to markout is
  adverse selection.

## Backtest

```bash
itch-dump ~/data/itch/01302019.NASDAQ_ITCH50.gz --symbol AAPL --tob-csv aapl.csv --quiet
python3 research/backtest.py aapl.csv --hold-ticks 0 1 2 --skew-ticks 0 1 2
```

Runs the trader's market maker on one symbol's top of book over a parameter
grid. Fills come from a queue model:
- shares traded at our price come off the queue ahead of us;
- cancels come off it pro rata;
- a replace goes to the back.

`lab_research/backtest.py` explains the model and its limits. Use it to compare
settings, not to forecast PnL.

## Tests

```bash
python3 -m unittest discover research/tests
```
