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

## Colo network (options C, D and G)

```bash
python3 research/colo.py --option C --trader-log trader.out --page colo.html
python3 research/colo.py --option all --trader-log trader.out --json colo.json
COLO_OPTION=D,G python3 research/colo.py
```

Puts the firm on an exchange colo network: Arista 7130 Layer 1 fan-out of the
feed and MetaMux merging orders toward the exchange, over 50 m
cross-connects. `--option` (or `COLO_OPTION`) picks the host design: `C`
kernel-bypass servers, `D` an FPGA feed handler in front of a CPU strategy,
`G` the strategy in the 7130's user FPGA; a list such as `C,G`, or `all`.
The default is `C`. `lab_research/colo/` is a stdlib-only discrete-event model
(times in ns) with port serialisation and contention, so simultaneous orders
from several hosts queue at MetaMux and CPU hosts queue during microbursts.

With `--trader-log`, C and D each add a `-lab` run:

- `C-lab`: each host's decision time is drawn from the trader's "Tick to wire"
  percentiles. The NIC rx and tx stages stay modelled, because the trader
  measures from `recvmmsg` to the socket.
- `D-lab`: the FPGA feed handler, PCIe DMA and strategy evaluation stay
  modelled, and the order path is drawn from the trader's "Decide to wire"
  percentiles, because in D the FPGA replaces the trader's rx and book threads.
- G has none: its strategy is FPGA logic, so the C++ trader is not on its path.

It prints p50/p99/p99.9 tick-to-trade (at the host) and wire-to-wire (exchange
publish to order at the gateway), the mean time per stage, and a microburst
sweep; `--page` writes an interactive page. `--config my.json` overrides
`scenarios`, `traffic`, `switches` or `hosts`. Device latencies are
representative figures from public material, for comparing designs.

## Tests

```bash
python3 -m unittest discover research/tests
```
