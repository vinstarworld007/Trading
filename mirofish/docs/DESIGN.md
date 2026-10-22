# MiroFish Trader: design and architecture

A news and sentiment driven trading system for Nasdaq stocks. It joins two
multi-agent systems that were originally built for political and public-opinion
analysis:

- **BettaFish** (the *sentiment layer*): several independent analyst engines read
  every news item, each forms a view, and a forum aggregates them into one
  sentiment signal per stock. In the political setting this measured public
  opinion on a policy or candidate; here it measures market opinion on a company.
- **MiroFish** (the *swarm layer*): a population of simulated market
  participants with different personas, connected by a social network, reacts
  to that news over several rounds. In the political setting it predicted how a
  population's opinion would evolve; here it predicts how the crowd of traders
  will push the price, and whether the move will continue or reverse.

A decision layer fuses the two, a risk layer sizes and caps positions, and an
execution layer sends orders to a paper broker. **No live orders.** The system
runs as a backtest or as a paper trader; live trading is deliberately not
implemented.

> Assumption: the two prior builds the request refers to were not found in the
> project or in the `Trading` repo. This design follows the public BettaFish and
> MiroFish architectures and re-implements the parts that matter for trading.
> If the original builds exist, their engines can replace the ones here behind
> the same interfaces (`SentimentEngine`, `SwarmSimulation`).

---

## 1. Goals and non-goals

Goals

1. Turn timestamped news into tradeable, explainable signals for a Nasdaq universe.
2. Use the swarm simulation to answer the question sentiment alone cannot:
   *has the market already priced this, will it overshoot, or will it drift?*
3. One code path for backtest and paper trading, with no lookahead in the backtest.
4. Run with no API keys (deterministic engines). Claude improves the sentiment
   layer when a key is present, and its answers are cached so backtests stay
   reproducible and cheap.
5. Every decision has a written report: which headlines, which engines, what the
   swarm did, what risk changed.

Non-goals

- Live order routing. The Alpaca adapter is hard-wired to Alpaca's paper endpoint.
- High-frequency trading. Decisions are daily (signal at the close, trade at the
  next open). The C++ lab in this repo is where the low-latency stack lives; see
  section 10 for how the two connect.
- A claim of real edge. The synthetic demo plants an effect so the pipeline can
  be tested end to end. Real performance has to be measured on real news and
  prices.

---

## 2. System overview

```
                 ┌──────────────────────────────────────────────────────────────┐
  News sources   │ 1. INGEST                                                    │
  (RSS, JSONL,   │   NewsSource → NewsItem(ts, source, headline, body, tickers?) │
   synthetic)    │   PriceSource → daily OHLCV bars                             │
                 └───────────────┬──────────────────────────────────────────────┘
                                 │
                 ┌───────────────▼──────────────────────────────────────────────┐
                 │ 2. SENTIMENT LAYER  (BettaFish)                              │
                 │   EntityLinker   headline → tickers, themes                  │
                 │   Engines (independent analysts, run on every item):         │
                 │     LexiconEngine   finance lexicon, negation, phrases       │
                 │     EventEngine     event type, base impact, horizon         │
                 │     MacroPolicyEngine  political / macro themes → exposures  │
                 │     ClaudeEngine    (optional) structured LLM read           │
                 │   Forum           weighted consensus + disagreement          │
                 │   SignalAggregator  credibility, novelty, buzz, spillover    │
                 │   → SentimentSignal(ticker, date, score, confidence, buzz)   │
                 └───────────────┬──────────────────────────────────────────────┘
                                 │
                 ┌───────────────▼──────────────────────────────────────────────┐
                 │ 3. SWARM LAYER  (MiroFish)                                   │
                 │   KnowledgeGraph  ticker ↔ sector ↔ theme ↔ peer edges       │
                 │   Population      personas: retail, momentum, value,         │
                 │                   institutional, contrarian, macro/political, │
                 │                   market maker; small-world social graph     │
                 │   Simulation      R rounds × M seeds: exposure → opinion     │
                 │                   dynamics → demand → price impact → feedback│
                 │   Calibrator      rolling fit: sim move → realized return    │
                 │   → SwarmForecast(expected remaining move, P(up), dispersion,│
                 │                   herding, overshoot)                        │
                 └───────────────┬──────────────────────────────────────────────┘
                                 │
                 ┌───────────────▼──────────────────────────────────────────────┐
                 │ 4. DECISION  fuse sentiment + swarm → alpha, conviction      │
                 │ 5. RISK      vol sizing, name/sector/gross/net caps,          │
                 │              stops, max hold, daily-loss and drawdown halts  │
                 │ 6. EXECUTION Broker: PaperBroker (sim fills) | AlpacaPaper   │
                 │ 7. REPORT    ReportAgent → markdown per run; metrics         │
                 └──────────────────────────────────────────────────────────────┘
```

The `Pipeline` object owns layers 2 to 5 and exposes one method,
`pipeline.decide(as_of, news, prices, portfolio) → DecisionBatch`. The backtester
and the paper trader both call it; only where news and prices come from, and
which broker executes, differ.

---

## 3. Data model

| Type | Fields | Notes |
|---|---|---|
| `NewsItem` | `id, ts (UTC), source, headline, body, tickers` | `tickers` optional; the linker fills it |
| `EngineView` | `engine, score ∈ [-1,1], confidence ∈ [0,1], event_type, magnitude, horizon_days, themes, rationale` | one per engine per item |
| `ItemAssessment` | `item, ticker, relevance, views, forum_score, forum_confidence, disagreement, credibility, novelty` | forum output per (item, ticker) |
| `SentimentSignal` | `ticker, date, score, confidence, buzz, magnitude, items, spillover` | aggregated per ticker per signal date |
| `SwarmForecast` | `ticker, sim_day_one, sim_total, continuation, gap, expected_move, p_up, dispersion, herding, overshoot, calibrated_return, persona_flow` | Monte Carlo summary |
| `Decision` | `ticker, alpha, conviction, target_weight, reason` | before and after risk |
| `Order` / `Fill` | `ticker, side, qty, price, ts, fees` | broker level |

Prices are floats in this package (dollars). The C++ lab's integer `Price(4)`
convention applies on the low-latency side; conversion happens at the boundary.

### Timing rules (no lookahead)

- A news item's **signal date** is its session date if it arrived before the
  16:00 ET close, otherwise the next session.
- Signals are computed after the close of the signal date. The observed reaction
  that day (stock return minus beta × QQQ return) is available to the swarm.
- Orders fill at the **next session's open**, with slippage and commission.
- The calibrator only uses forecasts whose outcome window has fully closed
  before the current date.

---

## 4. Sentiment layer (BettaFish)

BettaFish's idea is that several independent "engines" each read the material
in their own way, then a forum reconciles them. Disagreement between engines is
itself information (low confidence). The engines here:

### 4.1 EntityLinker
Maps text to tickers by symbol (`$NVDA`, `(NASDAQ: NVDA)`), company names and
aliases (`Nvidia`, `Jensen Huang`, `GeForce`), with word-boundary matching.
Relevance is 1.0 when the company is the subject of the headline (first entity
mentioned), lower for secondary mentions. It also tags **themes** (tariffs,
export controls, antitrust, rates, elections, drug pricing, AI capex,
China), which the swarm's knowledge graph spreads to exposed names.

### 4.2 LexiconEngine
A finance-specific lexicon in the spirit of Loughran-McDonald (generic sentiment
dictionaries misread words like *liability*, *tax*, *cost*). Handles:
- multi-word phrases first (`beats estimates`, `cuts guidance`, `price target raised`);
- negation within a 3-token window (`not`, `no`, `fails to`, `without`);
- intensifiers (`sharply`, `record`, `massive`) and hedges (`may`, `could`, `reportedly`);
- score = tanh(sum of weighted hits / √n), confidence rises with the number of hits.

### 4.3 EventEngine
Classifies the item into an event type with a base impact magnitude and horizon:
earnings beat/miss, guidance raise/cut, analyst upgrade/downgrade, M&A
(target/acquirer), buyback, product launch/outage/recall, litigation,
regulatory action, FDA approval/rejection, executive change, layoffs, plus the
political and macro types below. Magnitude sets how far the swarm's "fair value"
moves; horizon sets how long the signal lives.

### 4.4 MacroPolicyEngine (the political-analysis heritage)
The prior builds analysed politics. That capability is kept as an engine that
reads policy and macro news (tariffs, export controls and sanctions, antitrust,
elections and legislation, Fed and rates, drug pricing, subsidies such as the
CHIPS Act) and turns it into **theme shocks**. Each sector has an exposure vector
to those themes (for example semis to export controls and tariffs, mega-cap
platforms to antitrust, biotech to drug pricing, everything to rates). A single
"new chip export restrictions to China" headline then reaches NVDA, AMD, AVGO,
QCOM, ASML and MU with different signs and weights, without naming any of them.

### 4.5 ClaudeEngine (optional)
When `ANTHROPIC_API_KEY` is set and `--llm` is passed, Claude reads each item and
returns structured JSON (`score, confidence, event_type, magnitude,
horizon_days, themes, affected_tickers, rationale`) through structured outputs,
so the response always parses. Model `claude-opus-5-5`, low effort (this is
classification), server-side refusal fallback on. Results are cached on disk by
a hash of the text, so a backtest re-run makes no API calls. In the forum it acts
as the highest-weighted analyst, not the only one: if the API fails, the
deterministic engines still produce a signal.

### 4.6 Forum
For each (item, ticker):
- `forum_score = Σ wₑ·cₑ·sₑ / Σ wₑ·cₑ` over engines e (weight, confidence, score);
- `disagreement` = weighted standard deviation of the scores;
- `forum_confidence = mean confidence × (1 − disagreement)`.

### 4.7 SignalAggregator
Rolls items up per ticker and signal date:
- **source credibility**: wire services and filings 1.0, major press 0.9, press
  releases 0.6, blogs and social 0.4;
- **novelty**: near-duplicate headlines (token Jaccard > 0.6 with an earlier item
  in the lookback) are down-weighted, so ten rewrites of one story count once;
- **one day at a time**: only the signal day's items make a signal, so old news
  never reopens a position; the event horizon sets how long a position may be
  held (section 7);
- **spillover**: theme shocks (weight 0.8) and peer read-across from the
  knowledge graph (weight 0.5, scaled by event type: earnings and guidance read
  across, litigation mostly does not) add a signal to names not in the text;
- **buzz**: today's item count against the ticker's trailing average (attention
  feeds the swarm's exposure probability).

Output: `SentimentSignal.score = tanh(Σ weighted forum scores)`, plus a
confidence and the expected magnitude.

---

## 5. Swarm layer (MiroFish)

MiroFish seeds a knowledge graph from the material, generates a population of
agents with personas, lets them interact over rounds, and reads the prediction
off the emergent behaviour. The trading version keeps that loop but makes the
agents market participants and adds a price that they move and react to.

### 5.1 Knowledge graph
Nodes: tickers, sectors, themes. Edges: ticker→sector, sector→theme (exposure
weights), ticker↔ticker peers and supply chain (AAPL↔AVGO/QCOM, NVDA↔AMD,
ASML→semis). Used for spillover in 4.7 and to set each simulation's
`fair_move` (sentiment × magnitude × exposure).

### 5.2 Personas
| Persona | Share driver | Behaviour |
|---|---|---|
| Retail | ticker `retail_share`, popularity, buzz | fast attention (faster when the price moves), high herding, chases price, excitement fades |
| Momentum | constant | follows the simulated price trend, excitement fades |
| Value / fundamental | constant | anchors on `fair_move`, sells overshoot, buys undershoot |
| Institutional | 1 − retail share | slow to notice, large, accurate, low herding |
| Contrarian | small | fades extreme crowd stance |
| Macro / political analyst | theme content | amplifies policy and macro news, ignores single-name noise |
| Market maker | constant | absorbs flow, sets price impact (depth) |

Each agent has attention, skill (noise in how it reads the news), herding
(weight on neighbours), capital, and persona weights for momentum, value,
contrarian, macro amplification and fade (how fast excitement beyond its own
read of the news wears off). Agents sit on a Watts-Strogatz small-world graph,
with retail agents placed next to each other so retail clusters are dense.

### 5.3 Simulation loop
For each ticker with a signal, `M` Monte Carlo seeds × `R` rounds (default 16 × 12):

Everything is in units of the expected move: `fair = score / 0.6`, so a clear
headline means "a typical full-size move for this kind of news". The dynamics
are close to linear in `fair`, so the *shape* of the path (drift or overshoot)
comes from the crowd, not from how loud the headline is.

1. **Exposure**: an agent notices the news with probability attention ×
   salience (buzz, confidence); retail notices faster while the price moves.
   Perceived value = fair × macro amplification + noise·(1 − skill).
2. **Opinion dynamics**: Hegselmann-Krause bounded confidence. Beliefs move
   toward neighbours whose beliefs are within ε, weighted by herding.
   Unexposed agents can be talked into a view by exposed neighbours.
3. **Persona rules**: excitement fades back toward the agent's own read;
   value agents (only once exposed) hold `fair + 1.5·(fair − price)`, buying
   below fair and selling above; momentum adds the last price change;
   contrarians fade the crowd's excess over the price.
4. **Demand and price**: target position = capital × tanh(k·belief)/k; agents
   trade halfway to target each round; price += λ · net flow / (capital × depth),
   with depth from the market-maker share.

Rounds 1 to 4 stand for the signal day, rounds 5 to 12 for the following days.
With default settings a retail-heavy name (TSLA) jumps further on day one and
gives some back; an institutional name (PEP) moves less on day one and keeps
drifting (`tests/test_swarm.py` pins this).

### 5.4 Outputs
- `sim_day_one` (price after round 4) and `sim_total` (after round R);
- `continuation = sim_total − sim_day_one`: drift if it has the sign of the
  news, a reversal if not;
- `gap = sim_total − observed day-one move`: positive when the real stock has
  not yet moved as far as the crowd would push it, negative when it has
  already overshot;
- `p_up`, `dispersion` across seeds (model uncertainty), `herding` (share of
  agents on the majority side), `overshoot` (peak / end).

### 5.5 Calibration
Simulation units are not returns, and how much each reading is worth is an
empirical question. `Calibrator` fits a rolling ridge regression of the
realized idiosyncratic return (next open to the close three sessions later) on
`continuation` and `gap`, shrunk toward a prior of (1.0, 0.5) and clamped at
zero, so a reading the data does not support is switched off rather than
inverted. Only matured samples enter the fit. `calibrated_return` is the
fitted combination.

### 5.6 Optional Claude personas (extension)
MiroFish uses LLM agents. That is too costly per ticker per day for a backtest,
so the default agents are rule-based. The hook exists to let Claude role-play a
handful of persona archetypes for the day's top stories and seed the initial
belief distribution; see `swarm/population.py`.

---

## 6. Decision layer

```
S  = sentiment score (∈ [-1,1])
M  = calibrated swarm expected return / ticker daily vol   (a z-like number)
alpha      = w_s·S + w_m·tanh(M)                (default w_s = 0.4, w_m = 0.6)
agreement  = 1.0 if sign(S) == sign(M) else 0.5
conviction = |alpha| · agreement · confidence · (1 − dispersion penalty)
```

Enter when `conviction ≥ entry_threshold`; the side is `sign(alpha)`. Shorting
is on by default and can be turned off. Target weight is
`alpha × risk_budget / daily_vol`, capped by the risk layer. Positions close on
max hold (default 5 sessions), stop loss, take profit, or a signal that flips
sign with enough conviction.

---

## 7. Risk layer

| Control | Default |
|---|---|
| Max weight per name | 10% of equity |
| Max gross exposure | 100% |
| Max net exposure | ±60% |
| Max sector weight | 30% |
| Max open positions | 12 |
| Stop loss / take profit | 2.5 × / 4 × daily vol from entry |
| Max hold | 5 sessions |
| Daily loss halt | −3% equity: flatten and skip new entries that day |
| Drawdown halt | −15% from peak: flatten and stop trading until reset |
| Min price / liquidity | price ≥ $5, 20-day avg dollar volume ≥ $20M |

Every clip is written into the decision's reason, so the report shows when risk,
not the signal, set a size.

---

## 8. Execution

- `Broker` interface: `positions()`, `cash()`, `submit(order)`, `fills()`.
- `PaperBroker`: in-process ledger, fills at a given price with slippage (default
  5 bps) and commission (default $0.005/share, min $1). State persists to JSON
  for the paper loop.
- `AlpacaPaperBroker`: Alpaca paper trading REST API with `APCA_API_KEY_ID` and
  `APCA_API_SECRET_KEY`. The base URL is fixed to `https://paper-api.alpaca.markets`;
  there is no live switch. Turning on live trading is a deliberate code change
  that needs the owner's sign-off.

---

## 9. Run modes

| Command | What it does |
|---|---|
| `python -m mirofish_trader demo` | builds a synthetic year of news and prices, backtests, writes a report |
| `python -m mirofish_trader backtest --news N.jsonl --prices P.csv` | backtest on your data |
| `python -m mirofish_trader analyze "headline"` | prints each engine's view, the forum, and the swarm for one headline |
| `python -m mirofish_trader paper --state paper.json` | one paper-trading cycle: fetch RSS news and Stooq prices, decide, place paper orders, write the report |
| `python -m mirofish_trader fetch-prices --out P.csv` | downloads daily bars for the universe from Stooq |

The paper cycle is meant to be run once a day after the close (cron or a
scheduled task). Add `--broker alpaca` to send the orders to an Alpaca paper
account instead of the local ledger.

---

## 10. How it fits the Trading Lab

This package lives in `mirofish/` next to the C++ lab and has no build
dependency on it. The natural connection points, not built yet:

1. **Signal to the firm stack**: publish `Decision` targets as a small fixed-size
   struct over the lab's shared-memory ring, so `firm/trader` can work the orders
   against `simex` with its OMS and pre-trade risk.
2. **Vendor feed**: add a news topic to the lab's vendor servers (OMM JSON
   WebSocket) so news and market data come over one channel in replays.
3. **Research**: decisions and fills use the same CSV shape as `research/`, so
   `research/report.py` style PnL and TCA can be run on them.

---

## 11. Package layout

```
mirofish/
  docs/DESIGN.md                this file
  requirements.txt              numpy, pandas (anthropic optional)
  mirofish_trader/
    models.py                   dataclasses in section 3
    config.py                   all tunables, one dataclass per layer
    universe.py                 Nasdaq universe: names, aliases, sectors, betas, retail share, theme exposures, peers
    ingest/news.py              JSONL load/save, RSS fetch (stdlib), signal-date bucketing
    ingest/prices.py            CSV load, Stooq download, returns and vol helpers
    ingest/synthetic.py         synthetic news + prices with a planted reaction model
    sentiment/linker.py         EntityLinker
    sentiment/lexicon.py        LexiconEngine
    sentiment/events.py         EventEngine
    sentiment/macro.py          MacroPolicyEngine
    sentiment/llm.py            ClaudeEngine (optional, cached)
    sentiment/forum.py          Forum + SignalAggregator
    swarm/graph.py              KnowledgeGraph
    swarm/population.py         personas and the social graph
    swarm/simulation.py         SwarmSimulation (vectorised numpy)
    swarm/calibration.py        Calibrator
    decision.py                 fusion
    risk.py                     RiskManager
    broker.py                   PaperBroker, AlpacaPaperBroker
    pipeline.py                 Pipeline.decide()
    backtest.py                 event-driven daily backtester
    paper.py                    one paper-trading cycle
    report.py                   ReportAgent (markdown) and metrics
    cli.py, __main__.py
  tests/                        stdlib unittest
```

---

## 12. Testing

- Unit tests per layer: linker, lexicon negation, event classification, forum
  math, swarm determinism under a seed and its overshoot behaviour for a
  retail-heavy name, risk caps, broker accounting.
- No-lookahead test: shuffling or deleting future news and prices must not change
  past decisions.
- End-to-end: the synthetic demo runs, trades, and the report reconciles (cash +
  positions = equity, every fill has an order).

## 13. Known limits and next steps

- Daily bars only; intraday news timing is coarsened to the session.
- RSS headlines are thin; a paid news API (Benzinga, Polygon, Dow Jones) slots in
  as another `NewsSource`.
- The swarm's persona mix per ticker is a prior, not fitted. Fitting it to each
  name's historical reaction profile is the most valuable next step.
- Synthetic results show the plumbing works, not that the strategy makes money.
  On the synthetic year the calibrator turns the `gap` reading off (its weight
  fits to zero) and keeps `continuation`; whether `gap` earns its place on real
  data is the first thing to check.
- The Claude engine is wired and its request shape is tested against a mocked
  API, but it has not been run against the live API from this environment.
