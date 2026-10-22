# Market scenarios

What happens on a trading day besides continuous matching, and where each
part of the lab handles it. ITCH field values are in
`docs/specs/itch50-layout.md`, OUCH liquidity flags in
`docs/specs/ouch50-layout.md`.

| Scenario | simex (exchange) | ITCH on the feed | Feed handler, vendors |
| --- | --- | --- | --- |
| Opening auction | Pre-open: orders rest unmatched, IOC orders are canceled; imbalance messages every `--noii-interval`; then one uncross | `I` (cross type O), `C` printable N per side, `Q` type O, `H` T | `print_scenarios` shows the last NOII and the open; vendors send auction fields, then clear them at the cross |
| Closing auction | From `--close-at`, the same as the open, then the market closes | `I` C, `C` N, `Q` C | close price, auction fields |
| News halt | `H` H T1, orders refused (OUCH reject 0x0007), `H` Q T3 for `--quote-period`, halt cross, `H` T | `H`, `I` H, `Q` H | status and halt reason |
| LULD | Bands from the reference price (`lab/book/luld.hpp`); buys priced above the upper band and sells below the lower are repriced to it; a book at its band for `--luld-limit` seconds is in a limit state, then paused | `H` P LUDP, `J` collar, `I` H, `Q` H, `H` T | collar, LULD band (`UPLIMIT`/`LOLIMIT`, `LULD.*`) |
| Reopening extension | No paired shares in the collar: the pause is extended, the collar widened (up to 3 times) | `J` with extension count | collar extensions |
| Market-wide circuit breaker | Level 1, 2 or 3 decline: every symbol halted (level 3 for the day), then a quotation period and halt crosses | `V` at the start, `W`, `H` H MWCn, `H` Q MWCQ, `H` T | MWCB levels and breach |
| IPO | Listed halted (IPO1) with `K`, released for quotation (IPOQ), crossed, trading | `R` IPO flag Y, `H` H IPO1, `K`, `H` Q IPOQ, `I` H, `Q` H, `H` T | IPO line, status |
| Operational halt | Nasdaq stops the symbol without a regulatory halt | `h` H, then `h` T | operational halt state |
| Broken trade | The latest trade broken as clearly erroneous | `B` with its match number | broken trades count |
| Reg SHO | A drop of 10% from the opening reference turns on the price test | `Y` 1 | Reg SHO state |
| Implied strip | A strip NAME=A+B trades its own book and implied liquidity: a strip order lifts both legs at once | `A`/`E` on the legs, `P` for the strip fill | `--strip` prints implied-out depth and implied-in leg quotes (`lab/book/implied.hpp`) |

Strip definitions are not on ITCH; simex, `agents`, `itch-fh` and `itch-dump`
take the same `--strip NAME=A+B`.

Run them:

```bash
# every scenario once, timed to the session
simex --symbols SYM0001,SYM0002,SYM0003,SYM0004 --scenario all --strip ST12=SYM0001+SYM0002 --duration 23
agents --symbols SYM0001,SYM0002,SYM0003,SYM0004 --day-orders 0.4 --shock SYM0003@5:-12 --strip ST12=SYM0001+SYM0002 --duration 20
itch-fh --group 239.1.1.1:30001 --strip ST12=SYM0001+SYM0002

# or one at a time: --open-auction S, --close-at T --close-auction S, --halt SYM@T+D:T1,
# --op-halt SYM@T+D, --ipo SYM@T+D, --mwcb 1@T+D, --break-at T, --luld, --reg-sho

# a synthetic day with every scenario, for itch-dump
itch-gen scenarios.itch50.gz --scenarios && itch-dump scenarios.itch50.gz
```

`scripts/e2e-scenarios.sh` runs the first three plus two vendors and checks
that every scenario happened and that every book agrees.

## Lab simplifications

- The LULD reference price is the latest cross that printed shares, else the
  first trade (after an empty reopening, the book's midpoint), not the SIP's
  five-minute average; bands are not doubled at the open and close.
- `--scenario all` narrows the band to 2% and shortens the plan's 15-second
  limit state and 5-minute pause so a short simulated day reaches them.
- Auction price: most paired shares, then least imbalance, then nearest the
  reference price, then the lower price; a reopening cross is clamped to its
  collar.
