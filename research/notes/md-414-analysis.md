# MD-414: what the data says

Sofia, quant research. I worked from the replay output, the fills and the
drop copy, then read the commits that touched the numbers.

## Fixed in this PR

| Report | Finding | Fix |
|---|---|---|
| 2, auction volume | Volume minus vendor equals the non-printable C shares, symbol by symbol, to the share | Stop counting printable N executions in volume |
| 6, fill rate | Replaces per order went from 1.3 to 4.1; 70% of replaces moved exactly one tick | `should_move` uses `>` again, and the test that was changed to match is restored |
| 5, quoting in a pause | Every rejected order was sent while trading state was P | Pull quotes whenever state is not T |
| 8, PnL vs risk | Report PnL equals PnL marked at each symbol's first mid of the day | Marks use the last mid; test added |
| 11, latency report | p99 of a 50-sample run is the 49th value, not the 50th | Percentile rank rounds up again; test added |

## Found, not fixed (I am not confident in the C++)

- **Report 7, self-trades.** Every self-trade has our bid exactly at our own
  working ask. The self-match check in `firm/trader/trading.cpp` compares with
  `>` where I think it needs `>=`. I would like an engineer to confirm.
- **Report 4, throttle.** The order rate after the lunch lull is far above
  `max_orders_per_second`. The refill no longer caps tokens at `burst`
  (commit "risk: drop the clamp from the token refill").
- **Report 3, notional.** 900 x 50,000,000 overflows 32 bits; the remainder is
  under the limit. The new `notional` in `risk.hpp` is computed before it is
  widened.

## Numbers not to trust since the release

Auction-day volume, every PnL from `report.py`, the desk's fill-rate and
replace statistics, and last week's latency report.
