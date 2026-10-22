# MiroFish Trader demo (synthetic)

Synthetic data: the reaction model is planted (retail-heavy names overshoot and reverse,
institutional names drift), so these numbers show the pipeline working end to end, not
an edge on real markets.

## Results

| Metric | Strategy | QQQ |
|---|---|---|
| Total return | +62.9% | -5.6% |
| Sharpe | 6.22 | -0.24 |
| CAGR | +70.3% | |
| Annual vol | 8.6% | |
| Max drawdown | -2.8% | |
| Correlation to QQQ | -0.34 | |
| Information ratio | 2.52 | |
| Round trips | 485 | |
| Hit rate | 59% | |
| Avg trade PnL | $1,301 | |
| Fees | $2,546 | |
| Swarm weights (continuation, gap) | 1.85, 0.00 (750 samples) | |

## What each layer adds

| Variant | Return | Sharpe | Max DD | Trades | Hit rate |
|---|---|---|---|---|---|
| Sentiment + swarm (default) | +62.9% | 6.22 | -2.8% | 485 | 59% |
| Sentiment only | +67.3% | 5.69 | -3.8% | 515 | 57% |
| Swarm only | +46.6% | 5.11 | -2.9% | 438 | 58% |

## Best and worst trades

| Ticker | Side | Opened | Closed | PnL | Exit |
|---|---|---|---|---|---|
| COST | short | 2025-09-16 | 2025-09-23 | $16,699 | take profit +11.7% |
| ASML | short | 2025-12-03 | 2025-12-09 | $15,944 | take profit +11.1% |
| MSFT | short | 2025-12-01 | 2025-12-08 | $15,807 | take profit +8.3% |
| ASML | short | 2025-11-13 | 2025-11-21 | $14,380 | take profit +8.3% |
| ADBE | short | 2025-11-07 | 2025-11-14 | $13,548 | take profit +9.7% |
| COST | short | 2025-12-08 | 2025-12-12 | $-10,185 | stop -7.7% |
| MSFT | short | 2025-10-06 | 2025-10-09 | $-9,847 | stop -7.2% |
| SBUX | short | 2025-07-10 | 2025-07-15 | $-8,707 | stop -6.2% |
| AMZN | short | 2025-03-07 | 2025-03-17 | $-8,655 | stop -6.7% |
| CSCO | long | 2025-10-15 | 2025-10-21 | $-8,473 | stop -5.2% |

## Recent decision cycles

### 2025-12-16
- Exit MU: max hold 5 sessions
- Order MU +131 shares (max hold 5 sessions)

#### META: SHORT (alpha -0.33, conviction 0.24, target -5.2%)
- Sentiment -0.56, confidence 0.48, buzz 1.90, expected magnitude 1.5%
  - "Meta Platforms sued in class action over disclosures" [reuters]: litigation, forum -0.82; lexicon -0.83, event -0.80
  - "UPDATE 1-Meta Platforms sued in class action over disclosures" [marketwatch]: litigation, forum -0.82; lexicon -0.83, event -0.80
- Swarm: day one -1.01%, end -1.25%, continuation -0.24%, gap to today's move +2.47%, calibrated -0.44%, p_up 1.00, herding 0.97, overshoot 1.03
- Swarm flow by persona: institutional -0.70, retail -0.34, value -0.26, momentum -0.19, macro -0.05, contrarian -0.00
- sentiment -0.56 (conf 0.48)
- swarm -0.44% expected (-0.18 vol), p_up 1.00
- risk: already holding, no top-ups

#### AMGN: no trade (alpha +0.12, conviction 0.07, target +0.0%)
- Sentiment +0.18, confidence 0.22, buzz 2.04, expected magnitude 1.0%
  - "Amgen could rally, some traders say" [reuters]: general, forum +0.54; lexicon +0.54
  - "UPDATE 1-Amgen could rally, some traders say" [businesswire]: general, forum +0.54; lexicon +0.54
- Swarm: day one +0.19%, end +0.27%, continuation +0.08%, gap to today's move +0.75%, calibrated +0.15%, p_up 1.00, herding 0.96, overshoot 1.01
- Swarm flow by persona: institutional +0.30, value +0.10, momentum +0.08, retail +0.07, macro +0.02, contrarian +0.00
- sentiment +0.18 (conf 0.22)
- swarm +0.15% expected (+0.08 vol), p_up 1.00
- conviction 0.07 below entry 0.12

### 2025-12-17
- Exit CSCO: max hold 5 sessions
- Exit MSFT: max hold 5 sessions
- Exit AMD: max hold 5 sessions
- Exit NFLX: max hold 5 sessions
- Exit NVDA: max hold 5 sessions
- Exit META: max hold 5 sessions
- Exit ADBE: max hold 5 sessions
- Order CSCO +476 shares (max hold 5 sessions)
- Order MSFT +1353 shares (max hold 5 sessions)
- Order AMD +245 shares (max hold 5 sessions)
- Order NFLX +652 shares (max hold 5 sessions)
- Order NVDA +983 shares (max hold 5 sessions)
- Order META +130 shares (max hold 5 sessions)
- Order ADBE +401 shares (max hold 5 sessions)
- Order GILD -1867 shares (enter)
- Order AAPL +362 shares (enter)
- Order AMGN -485 shares (enter)

#### GILD: SHORT (alpha -0.48, conviction 0.38, target -10.0%)
- Sentiment -0.45, confidence 0.61, buzz 1.35, expected magnitude 2.5%
  - "Gilead CFO resigns unexpectedly" [yahoo]: exec_change, forum -0.73; lexicon -0.60, event -0.80
  - "Moderna reports disappointing quarter, profit misses consensus" [yahoo] (spillover): earnings_miss, forum -0.16; lexicon -0.75, event -0.80
  - "UPDATE 1-Moderna reports disappointing quarter, profit misses consensus" [yahoo] (spillover): earnings_miss, forum -0.16; lexicon -0.75, event -0.80
- Swarm: day one -1.19%, end -1.64%, continuation -0.45%, gap to today's move -1.86%, calibrated -0.86%, p_up 0.00, herding 0.96, overshoot 1.02
- Swarm flow by persona: institutional -0.82, value -0.25, momentum -0.19, retail -0.15, macro -0.04, contrarian +0.00
- sentiment -0.45 (conf 0.61)
- swarm -0.86% expected (-0.54 vol), p_up 0.00
- risk: weight -11.9% capped at 10%

#### AAPL: LONG (alpha +0.39, conviction 0.24, target +8.8%)
- Sentiment +0.56, confidence 0.50, buzz 1.35, expected magnitude 1.8%
  - "Analyst upgrades Apple to buy, raises price target" [reuters]: upgrade, forum +0.84; lexicon +0.89, event +0.80
- Swarm: day one +1.21%, end +1.48%, continuation +0.27%, gap to today's move -0.16%, calibrated +0.52%, p_up 0.88, herding 0.97, overshoot 1.04
- Swarm flow by persona: institutional +0.66, retail +0.34, value +0.26, momentum +0.20, macro +0.05, contrarian +0.00
- sentiment +0.56 (conf 0.50)
- swarm +0.52% expected (+0.29 vol), p_up 0.88

#### AMGN: SHORT (alpha -0.20, conviction 0.15, target -4.5%)
- Sentiment -0.21, confidence 0.51, buzz 0.00, expected magnitude 1.3%
  - "Moderna reports disappointing quarter, profit misses consensus" [yahoo] (spillover): earnings_miss, forum -0.27; lexicon -0.75, event -0.80
  - "UPDATE 1-Moderna reports disappointing quarter, profit misses consensus" [yahoo] (spillover): earnings_miss, forum -0.27; lexicon -0.75, event -0.80
  - "Gilead CFO resigns unexpectedly" [yahoo] (spillover): exec_change, forum -0.07; lexicon -0.60, event -0.80
- Swarm: day one -0.17%, end -0.35%, continuation -0.18%, gap to today's move -2.28%, calibrated -0.34%, p_up 0.00, herding 0.93, overshoot 1.00
- Swarm flow by persona: institutional -0.32, value -0.14, momentum -0.11, retail -0.04, macro -0.02, contrarian +0.00
- sentiment -0.21 (conf 0.51)
- swarm -0.34% expected (-0.19 vol), p_up 0.00

#### MRNA: no trade (alpha -0.19, conviction 0.07, target +0.0%)
- Sentiment -0.58, confidence 0.60, buzz 1.35, expected magnitude 3.9%
  - "Moderna reports disappointing quarter, profit misses consensus" [yahoo]: earnings_miss, forum -0.78; lexicon -0.75, event -0.80
  - "UPDATE 1-Moderna reports disappointing quarter, profit misses consensus" [yahoo]: earnings_miss, forum -0.78; lexicon -0.75, event -0.80
  - "Gilead CFO resigns unexpectedly" [yahoo] (spillover): exec_change, forum -0.04; lexicon -0.60, event -0.80
- Swarm: day one -3.47%, end -3.35%, continuation +0.12%, gap to today's move +1.85%, calibrated +0.24%, p_up 1.00, herding 0.99, overshoot 1.20
- Swarm flow by persona: retail -0.47, institutional -0.33, value -0.27, momentum -0.19, macro -0.05, contrarian -0.01
- sentiment -0.58 (conf 0.60)
- swarm +0.24% expected (+0.07 vol), p_up 1.00
- sentiment and swarm disagree
- conviction 0.07 below entry 0.12

### 2025-12-18
- Exit AMZN: max hold 5 sessions
- Exit AAPL: signal flipped (conviction 0.19)
- Order AMZN +444 shares (max hold 5 sessions)
- Order AAPL -617 shares (signal flipped (conviction 0.19))
- Order GOOGL +660 shares (enter)
- Order PDD +365 shares (enter)

#### GOOGL: LONG (alpha +0.34, conviction 0.19, target +8.7%)
- Sentiment +0.43, confidence 0.40, buzz 1.61, expected magnitude 1.5%
  - "Alphabet announces $10 billion share buyback program" [wsj]: buyback, forum +0.77; lexicon +0.72, event +0.80
- Swarm: day one +0.72%, end +0.95%, continuation +0.23%, gap to today's move -0.15%, calibrated +0.44%, p_up 0.94, herding 0.97, overshoot 1.02
- Swarm flow by persona: institutional +0.59, retail +0.23, value +0.22, momentum +0.16, macro +0.04, contrarian +0.00
- sentiment +0.43 (conf 0.40)
- swarm +0.44% expected (+0.28 vol), p_up 0.94

#### AAPL: SHORT (alpha -0.28, conviction 0.19, target -6.0%)
- Sentiment -0.41, confidence 0.36, buzz 1.25, expected magnitude 1.5%
  - "Apple sued in class action over disclosures" [cnbc]: litigation, forum -0.82; lexicon -0.83, event -0.80
- Swarm: day one -0.74%, end -0.92%, continuation -0.18%, gap to today's move +0.95%, calibrated -0.36%, p_up 1.00, herding 0.97, overshoot 1.05
- Swarm flow by persona: institutional -0.48, retail -0.26, value -0.21, momentum -0.15, macro -0.04, contrarian -0.00
- sentiment -0.41 (conf 0.36)
- swarm -0.36% expected (-0.19 vol), p_up 1.00

#### PDD: LONG (alpha +0.18, conviction 0.13, target +3.5%)
- Sentiment +0.43, confidence 0.40, buzz 1.35, expected magnitude 1.5%
  - "PDD Holdings announces $10 billion share buyback program" [wsj]: buyback, forum +0.77; lexicon +0.72, event +0.80
- Swarm: day one +0.91%, end +0.93%, continuation +0.02%, gap to today's move -0.87%, calibrated +0.04%, p_up 0.00, herding 0.98, overshoot 1.19
- Swarm flow by persona: retail +0.32, institutional +0.30, value +0.21, momentum +0.15, macro +0.04, contrarian +0.01
- sentiment +0.43 (conf 0.40)
- swarm +0.04% expected (+0.02 vol), p_up 0.00

### 2025-12-19
- Exit SBUX: max hold 5 sessions
- Exit GILD: signal flipped (conviction 0.20)
- Order SBUX -640 shares (max hold 5 sessions)
- Order GILD +3289 shares (signal flipped (conviction 0.20))
- Order ASML -242 shares (enter)
- Order NVDA +824 shares (enter)

#### ASML: SHORT (alpha -0.27, conviction 0.21, target -4.4%)
- Sentiment -0.40, confidence 0.64, buzz 1.25, expected magnitude 1.8%
  - "ASML hit by widespread outage" [reuters]: product_issue, forum -0.77; lexicon -0.72, event -0.80
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
- Swarm: day one -0.84%, end -1.08%, continuation -0.24%, gap to today's move -0.58%, calibrated -0.45%, p_up 0.00, herding 0.97, overshoot 1.02
- Swarm flow by persona: institutional -0.69, value -0.22, momentum -0.17, retail -0.15, macro -0.04, contrarian -0.00
- sentiment -0.40 (conf 0.64)
- swarm -0.45% expected (-0.18 vol), p_up 0.00

#### GILD: LONG (alpha +0.32, conviction 0.20, target +7.6%)
- Sentiment +0.37, confidence 0.33, buzz 1.25, expected magnitude 1.5%
  - "Gilead announces $10 billion share buyback program" [marketwatch]: buyback, forum +0.77; lexicon +0.72, event +0.80
- Swarm: day one +0.53%, end +0.80%, continuation +0.27%, gap to today's move -0.59%, calibrated +0.50%, p_up 0.00, herding 0.95, overshoot 1.00
- Swarm flow by persona: institutional +0.64, value +0.21, momentum +0.16, retail +0.12, macro +0.04, contrarian -0.00
- sentiment +0.37 (conf 0.33)
- swarm +0.50% expected (+0.30 vol), p_up 0.00

#### NVDA: LONG (alpha +0.25, conviction 0.18, target +4.5%)
- Sentiment +0.56, confidence 0.60, buzz 1.61, expected magnitude 1.7%
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj]: upgrade, forum +0.84; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch]: upgrade, forum +0.84; lexicon +0.89, event +0.80
  - "ASML hit by widespread outage" [reuters] (spillover): product_issue, forum -0.12; lexicon -0.72, event -0.80
- Swarm: day one +1.37%, end +1.42%, continuation +0.05%, gap to today's move +0.69%, calibrated +0.09%, p_up 1.00, herding 0.98, overshoot 1.13
- Swarm flow by persona: institutional +0.46, retail +0.43, value +0.25, momentum +0.17, macro +0.04, contrarian +0.01
- sentiment +0.56 (conf 0.60)
- swarm +0.09% expected (+0.04 vol), p_up 1.00

#### AVGO: no trade (alpha +0.02, conviction 0.02, target +0.0%)
- Sentiment +0.05, confidence 0.38, buzz 0.00, expected magnitude 0.2%
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
- Swarm: day one +0.01%, end +0.01%, continuation +0.01%, gap to today's move +2.31%, calibrated +0.01%, p_up 1.00, herding 0.96, overshoot 1.01
- Swarm flow by persona: institutional +0.10, value +0.03, momentum +0.03, retail +0.02, macro +0.01, contrarian +0.00
- sentiment +0.05 (conf 0.38)
- swarm +0.01% expected (+0.01 vol), p_up 1.00
- conviction 0.02 below entry 0.12

#### MU: no trade (alpha +0.02, conviction 0.02, target +0.0%)
- Sentiment +0.05, confidence 0.38, buzz 0.00, expected magnitude 0.2%
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
- Swarm: day one +0.01%, end +0.01%, continuation +0.01%, gap to today's move -1.02%, calibrated +0.01%, p_up 0.00, herding 0.95, overshoot 1.01
- Swarm flow by persona: institutional +0.08, value +0.03, momentum +0.03, retail +0.02, macro +0.01, contrarian +0.00
- sentiment +0.05 (conf 0.38)
- swarm +0.01% expected (+0.00 vol), p_up 0.00
- conviction 0.02 below entry 0.12

#### AMD: no trade (alpha +0.02, conviction 0.02, target +0.0%)
- Sentiment +0.05, confidence 0.38, buzz 0.00, expected magnitude 0.2%
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
- Swarm: day one +0.01%, end +0.01%, continuation +0.00%, gap to today's move -1.33%, calibrated +0.01%, p_up 0.00, herding 0.95, overshoot 1.06
- Swarm flow by persona: institutional +0.06, retail +0.03, value +0.03, momentum +0.03, macro +0.01, contrarian +0.00
- sentiment +0.05 (conf 0.38)
- swarm +0.01% expected (+0.00 vol), p_up 0.00
- conviction 0.02 below entry 0.12

#### TSLA: no trade (alpha +0.02, conviction 0.02, target +0.0%)
- Sentiment +0.05, confidence 0.38, buzz 0.00, expected magnitude 0.2%
  - "Analyst upgrades Nvidia to buy, raises price target" [wsj] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
  - "UPDATE 1-Analyst upgrades Nvidia to buy, raises price target" [marketwatch] (spillover): upgrade, forum +0.08; lexicon +0.89, event +0.80
- Swarm: day one +0.01%, end +0.01%, continuation +0.00%, gap to today's move -0.18%, calibrated +0.00%, p_up 0.00, herding 0.94, overshoot 1.20
- Swarm flow by persona: retail +0.04, value +0.03, institutional +0.03, momentum +0.02, macro +0.01, contrarian +0.00
- sentiment +0.05 (conf 0.38)
- swarm +0.00% expected (+0.00 vol), p_up 0.00
- conviction 0.02 below entry 0.12

### 2025-12-22
- Exit AMGN: stop -4.9%
- Exit GOOGL: take profit +6.3%
- Order AMGN +485 shares (stop -4.9%)
- Order GOOGL -660 shares (take profit +6.3%)
