# MiroFish Trader

News and sentiment driven trading decisions for Nasdaq stocks, built by joining
two multi-agent systems first used for political analysis:

- **BettaFish** (`mirofish_trader/sentiment/`): independent analyst engines
  (finance lexicon, event classifier, political/macro policy engine, optional
  Claude) read every headline, and a forum reconciles them into one signal per
  stock per day, with spillover to peers and policy-exposed sectors.
- **MiroFish** (`mirofish_trader/swarm/`): a swarm of simulated traders (retail,
  institutional, momentum, value, contrarian, macro, market makers) on a
  small-world social graph reacts to that signal over several rounds. The
  emergent price path says whether the move is already priced, will overshoot,
  or will keep drifting.

A decision layer fuses the two, a risk layer sizes and caps positions, and a
paper broker executes. **Paper trading and backtests only; there is no live
trading.** Design and architecture: [docs/DESIGN.md](docs/DESIGN.md).

## Quick start

```bash
cd mirofish
python3 -m venv ~/.venvs/mirofish && ~/.venvs/mirofish/bin/pip install -r requirements.txt
PY=~/.venvs/mirofish/bin/python

$PY -m mirofish_trader demo                 # synthetic year: backtest + layer comparison -> out/demo_report.md
$PY -m mirofish_trader analyze "US to tighten export controls on advanced chips to China"
$PY -m mirofish_trader analyze "Tesla beats estimates as deliveries surge" --observed 0.06
```

`analyze` prints what every engine, the forum and the swarm made of one headline.
`--observed` is the stock's move today: pass a big one and the swarm turns a
"buy the beat" into "already priced".

## Real data

```bash
$PY -m mirofish_trader fetch-prices --out prices.csv --start 2024-01-01   # Stooq, no key
$PY -m mirofish_trader backtest --news my_news.jsonl --prices prices.csv --report report.md
```

News JSONL, one item per line (`body` and `tickers` optional):

```json
{"ts": "2026-03-04T14:05:00Z", "source": "reuters", "headline": "Nvidia beats estimates as data center revenue surges"}
```

Prices CSV: `date,ticker,open,high,low,close,volume`, including `QQQ`.

## Paper trading

Run once a day after the close:

```bash
$PY -m mirofish_trader paper --state paper_state                  # RSS news + Stooq prices, local ledger
$PY -m mirofish_trader paper --state paper_state --broker alpaca  # Alpaca *paper* account
```

The local ledger fills today's orders at the next session's open on the next
run. `--broker alpaca` needs `APCA_API_KEY_ID` and `APCA_API_SECRET_KEY` and
sends market-on-open orders to `paper-api.alpaca.markets` (hard-coded). Each run
writes `paper_state/reports/paper-YYYY-MM-DD.md`.

## Claude in the forum

```bash
$PY -m pip install anthropic
export ANTHROPIC_API_KEY=...        # or `ant auth login`
$PY -m mirofish_trader demo --llm
```

`--llm` adds Claude (`claude-opus-5-5`, structured JSON output, low effort) as the
highest-weighted analyst. Answers are cached in `.cache/claude_sentiment.jsonl`,
so re-running a backtest costs nothing. If the API is unavailable the other
engines carry on.

## Configuration

Every tunable is in `mirofish_trader/config.py`. Override any of them with a
JSON file, for example long-only with tighter stops:

```json
{"decision": {"allow_short": false}, "risk": {"stop_vol_mult": 1.5, "max_weight": 0.05}}
```

```bash
$PY -m mirofish_trader backtest --config my.json --news ... --prices ...
```

## Tests

```bash
python3 -m unittest discover -s tests -t .
```

Covers each layer, broker accounting, risk caps and halts, a no-lookahead check
(cutting off future news and prices must not change any past order), the paper
loop, and the Claude engine's parsing and cache.

## Honest caveats

- The demo's synthetic market has a planted reaction model, so its numbers show
  the pipeline working, not an edge. Real performance needs real news.
- RSS headlines are thin and delayed; a proper news API is the first upgrade.
- The swarm's persona mix per stock is a prior, not fitted to history yet.
