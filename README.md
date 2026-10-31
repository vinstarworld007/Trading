# Trading Lab

A systematic trading firm in miniature that runs entirely in WSL2 on one PC:
Nasdaq TotalView-ITCH 5.0 data replayed into a low-latency feed handler, a
simulated exchange that matches orders, and a firm side with strategies,
pre-trade risk and order entry gateways.

## Quick start (WSL2, Ubuntu 24.04)

```bash
git clone https://github.com/vinstarworld007/Trading.git ~/trading-lab
cd ~/trading-lab
scripts/wsl-setup.sh            # packages, loopback multicast, huge pages, build, tests, lab-check
scripts/fetch-itch-sample.sh    # one sample trading day (4.8 GB) into ~/data/itch
```

Then on Windows, copy `scripts/wslconfig.example` to `%UserProfile%\.wslconfig`,
adjust `memory=` and `processors=`, and run `wsl --shutdown` in PowerShell.

Rebuild every book from a day and check its data quality:

```bash
./build/release/tools/itch-dump/itch-dump ~/data/itch/01302019.NASDAQ_ITCH50.gz \
    --symbol AAPL --depth 10 --top 20 --csv ~/data/itch/01302019-stats.csv
```

`itch-dump` exits 0 when every integrity check is zero and 1 otherwise, so it
can gate scripts. No download yet? `itch-gen /tmp/synth.gz --messages 5000000`
writes a synthetic day to try it on.

Run the same day as a live feed: `itch-replay` publishes it as MoldUDP64 on
loopback multicast and `itch-fh` rebuilds every book from the packets, asking
the replay's re-request server for anything lost. In two terminals:

```bash
./build/release/tools/itch-fh/itch-fh --rx-cpu 2 --book-cpu 3 --symbol AAPL
./build/release/tools/itch-replay/itch-replay ~/data/itch/01302019.NASDAQ_ITCH50.gz \
    --rate 2000000 --cpu 7 --drop 0.001 --burst 0.0001 --dup 0.001 --reorder 0.001
```

`itch-fh` prints gaps, retransmissions and receive-to-book latency, then the
same integrity checks and `Book digest` line as `itch-dump`: equal digests
mean the live feed rebuilt exactly the books the file did.
`scripts/e2e-live-feed.sh` does all of this in one command, and CI runs it.

Run the exchange: `simex` matches orders from firms over OUCH 5.0/SoupBinTCP
against a replayed day's order flow, which it uses as historical liquidity, and
publishes its own ITCH feed over MoldUDP64. In three terminals:

```bash
./build/release/tools/itch-fh/itch-fh --group 127.0.0.1:31001 --retrans 127.0.0.1:31002
./build/release/exchange/simex/simex --replay ~/data/itch/01302019.NASDAQ_ITCH50.gz \
    --rate 200000 --md-dest 127.0.0.1:31001 --journal /tmp/simex.journal --duration 60
./build/release/tools/ouch-client/ouch-client --reference ~/data/itch/01302019.NASDAQ_ITCH50.gz \
    --accounts FIRM01:lab,FIRM02:lab --orders 50000 --rate 10000 --replay-check
```

`simex` rebuilds books from its own feed while it runs and, at the close,
checks they equal the matching engine's books. `ouch-client` checks every
response (sequence numbers, fills inside limits, every request answered, a
re-login replaying byte for byte) and reports order round trips.
`scripts/e2e-simex.sh` runs all three, and CI runs it under every sanitizer.

Trade on it: `trader` is the firm. It rebuilds books from simex's feed, makes
markets in the symbols you give it, checks every order against pre-trade risk
limits and tracks orders and positions in its OMS. Start it before simex so it
sees the stock directory:

```bash
./build/release/firm/trader/trader --symbols AAPL,MSFT --account FIRM01:lab --duration 60 \
    --rx-cpu 2 --md-cpu 3 --trader-cpu 4 --oe-cpu 5
```

At the end it cancels its orders and reports positions, PnL, risk blocks, and
latency from a market data packet to the order it caused leaving the socket.
`scripts/e2e-firm.sh` runs it against simex with noise flow from `ouch-client`.

Check the day afterwards. simex sends a drop copy of every firm execution to
its `DROP01` session, which `dropcopy` records. `research/report.py`
reconciles the trader's own record of its fills against it, then prints
positions, PnL and transaction cost analysis (spread capture and markouts).
`research/backtest.py` sweeps the market maker's parameters on any symbol's
top of book from an ITCH day. See `research/README.md`.
`scripts/e2e-posttrade.sh` runs the whole loop.

Trade over FIX: `fixgw` is a FIX 4.4 venue in front of simex. Each FIX
session (by SenderCompID) trades through its own simex account; NewOrderSingle,
OrderCancelRequest and OrderCancelReplaceRequest become OUCH, and OUCH answers
come back as ExecutionReports and OrderCancelRejects. Sequence numbers persist
across reconnects, and a firm that missed messages gets them again by
ResendRequest. The subset and the mapping are in `docs/specs/fix44-subset.md`.

```bash
./build/release/exchange/simex/simex --symbols AAPL,MSFT --accounts OUCHA:pa,OUCHB:pb
./build/release/exchange/fixgw/fixgw --session FIXA:OUCHA:pa --session FIXB:OUCHB:pb
./build/release/tools/fix-client/fix-client --sessions FIXA,FIXB --orders 5000 --reconnect-at 2000
```

`scripts/e2e-fix.sh` runs them with a drop copy and checks every FIX fill
against it.

Trade without a replay: `agents` makes the market from a population of
makers, noise traders, value traders and momentum traders reacting to a
hidden fundamental value that moves a tick at a time. `scripts/e2e-agents.sh`
runs the firm's trader in that market.

Build by hand:

```bash
cmake --preset release && cmake --build --preset release && ctest --preset release
cmake --preset asan    && cmake --build --preset asan    && ctest --preset asan
./build/release/tools/lab-check/lab-check
./build/release/bench/core_bench
```

## What is here

| Path | What it is |
| --- | --- |
| `libs/core` | Lock-free SPSC ring, shared-memory regions, TSC clock, thread pinning, latency histogram, async logger, big-endian helpers |
| `libs/itch` | ITCH 5.0 codec for all 23 message types, gzip BinaryFILE reader/writer, synthetic day generator |
| `libs/book` | Order store, price-level books, and `BookBuilder`, which rebuilds every book from ITCH and runs data quality checks |
| `tools/itch-dump` | Replays a day file, rebuilds all books, prints integrity checks, top symbols, a symbol's book, per-symbol CSV, one symbol's top of book as CSV |
| `libs/mold` | MoldUDP64 codec, receiver-side sequencing (gaps, duplicates, reordering, retransmission requests), sender-side retransmit store, seeded fault injector |
| `libs/net` | UDP unicast and multicast sockets, `recvmmsg` batch receive |
| `tools/itch-replay` | Publishes a day file as a MoldUDP64 feed with pacing, heartbeats, End of Session, a re-request server and fault injection |
| `tools/itch-fh` | Feed handler: rx thread plus book thread, gap recovery, live books, latency and integrity report |
| `libs/feed` | MoldUDP64 publisher (packing, heartbeats, re-request server, faults) and receiver (rx thread, sequencing, recovery) shared by the tools, simex and the firm |
| `libs/net` (TCP) | Non-blocking TCP streams with a send queue, and listeners |
| `libs/soup` | SoupBinTCP 3.00 packets, login, and a stream reassembler |
| `libs/ouch` | OUCH 5.0 codec, one field list per message as in `libs/itch` |
| `libs/match` | Price-time matching engine: preallocated order pool, open-addressing key index, IOC and day orders, replace, cancel-on-disconnect |
| `exchange/simex` | The exchange: gateway thread (OUCH sessions), matching thread (sequencer and engine), market data thread (ITCH over MoldUDP64), journal, historical liquidity |
| `firm/oms` | Order management: order states through enter/replace/cancel, positions, cash and worst-case exposure |
| `firm/risk` | Inline pre-trade risk: kill switch, order size and notional, price collar, worst-case position, open orders, order rate |
| `firm/strategy` | A one-level market maker that leans against its position; pairs statistical arbitrage, trend following (moving-average cross), a fundamental thesis state machine, and execution helpers (work toward a target, participation of volume) |
| `tools/quant-lab` | The Quant Research Lab: a simulated year with all four strategies trading on the engine, ITCH feed, `BookBuilder`, risk and OMS, written out as JSON; `web/index.html` replays it |
| `firm/trader` | The firm's trading engine: market data, trading and order entry threads joined by rings |
| `tools/dropcopy` | Records simex's drop copy (every firm execution) to CSV and prints each account's positions |
| `libs/fix` | FIX 4.4 tag=value framing (BodyLength, CheckSum) and a session layer: logon, sequence numbers across reconnects, resend with gap fill, heartbeats and test requests |
| `exchange/fixgw` | FIX 4.4 venue in front of simex, mapping FIX orders onto OUCH sessions |
| `exchange/agents` | Agent-based liquidity: makers, noise, value and momentum traders around a hidden fundamental value |
| `tools/fix-client` | Trades FIX sessions against each other through `fixgw`, drops and resumes a session, and checks every answer |
| `research/` | Python: positions and PnL, reconciliation against the drop copy, TCA, a queue-model market-making backtest, and `quant_lab.py`, which runs the quant lab over several seeds and builds its page |
| `research/colo.py` | Option C colo network model: the firm on Arista 7130 Layer 1 fan-out, kernel-bypass hosts and MetaMux order aggregation, simulated at nanosecond resolution, with the trader's measured tick-to-wire as the host's decision time |
| `tools/ouch-client` | Trades random orders into simex on several accounts and checks every response |
| `tools/itch-gen` | Writes a synthetic ITCH day, for trying the tools without a multi-GB download |
| `tools/lab-check` | Checks the machine (CPUs, TSC, huge pages, loopback multicast, disk) and measures a core-to-core round trip |
| `scripts/` | WSL setup, sample data download, `.wslconfig` template |
| `docs/core-plan.md` | Which thread runs on which core |
| `docs/specs/itch50-layout.md` | ITCH 5.0 field tables the codec implements, with the source |
| `docs/specs/moldudp64-layout.md` | MoldUDP64 packet and request layouts, with the source |
| `docs/specs/fix44-subset.md` | The FIX 4.4 messages and fields `fixgw` speaks, with the source, and how they map onto OUCH |
| `docs/specs/dropcopy-layout.md` | The lab's own drop copy Execution Report |
| `docs/specs/soupbintcp-layout.md`, `docs/specs/ouch50-layout.md` | SoupBinTCP and OUCH 5.0 layouts, and how simex reads the spec where it leaves room |
| `scripts/e2e-live-feed.sh` | Replays a day with packet loss into `itch-fh` and checks the books match `itch-dump` |
| `scripts/e2e-simex.sh` | simex with historical liquidity, `ouch-client` trading into it and `itch-fh` on its feed; checks responses and that all books agree |
| `scripts/e2e-firm.sh` | `trader` making markets on simex against noise flow; checks risk limits, OMS consistency, no self-trades, clean shutdown |
| `scripts/e2e-posttrade.sh` | The firm session plus drop copy, then reconciliation, TCA, a backtest and the option C colo model on the trader's latency |
| `scripts/e2e-fix.sh` | Two FIX sessions trading through `fixgw`, one dropped and resumed midway; FIX fills checked against the drop copy |
| `scripts/e2e-agents.sh` | `trader` making markets among the agents, with no replay |
| `scripts/e2e-scenarios.sh` | simex `--scenario all` with an implied strip, agents, `itch-fh` and two vendors; checks every scenario ran and every book agrees |
| `scripts/e2e-quant-lab.sh` | The quant lab for a shortened year: clean books every day, consistent OMS, every strategy trades within its limits, and the page builds |
| `docs/scenarios.md` | Auctions, halts, LULD, circuit breakers, IPOs and implied strips: what each part of the lab does |
| `bench/` | Microbenchmarks: core library, ITCH codec and books, MoldUDP64, matching engine (with p50/p99/p99.9), FIX codec |

## Build phases

1. **Foundations**: build system, core library, WSL setup, CI.
2. **ITCH codec and offline book**: `itch-dump` rebuilds every order book from a sample day.
3. **Live feed**: `itch-replay` (MoldUDP64, fault injection, retransmission) and `itch-fh`.
4. **Exchange**: `simex` matching engine, OUCH/SoupBinTCP, ITCH output, historical liquidity.
5. **Firm hot path**: order gateway, inline risk, OMS, a market-making strategy.
6. **Post-trade and research**: drop copy, PnL, TCA, Python backtests.
7. **Hardening**: profiling, tail latency, agent-based liquidity, a FIX venue.
8. **Market data vendors**: three vendors that consolidate simex's feed and
   redistribute it: `icf-server` (an ICE Consolidated Feed look-alike over
   SoupBinTCP), `rt-server` (the public Refinitiv WebSocket API, OMM JSON with
   RDMFieldDictionary field ids) and `bpipe-server` (a Bloomberg B-PIPE
   look-alike carrying BLPAPI's session, service, subscription and event
   model). `vendor-client` subscribes through any of them and checks the
   result against the direct feed. ICE's and Bloomberg's wire formats are
   licensed, so those two protocols are the lab's own, documented in
   `docs/specs/vendor-*.md`.
9. **Market scenarios** (this): opening and closing auctions with imbalance
   messages, news halts, LULD bands, limit states, pauses and reopening
   auctions, market-wide circuit breakers, an IPO, an operational halt, a
   broken trade, Reg SHO, and implied strip books, in simex, on the feed, in
   the feed handler, in the synthetic day and through the vendors. See
   `docs/scenarios.md`.
10. **Quant Research Lab** (this): stat arb, trend and fundamental strategies
    beside the market maker, run together for a simulated year by
    `quant-lab`, and a page that replays the run (see below).

## Quant Research Lab

`quant-lab` runs four strategies for a simulated year in one process, on the
lab's own code. A market model moves a hidden value for five stocks;
liquidity providers and takers send orders around it to the matching engine.
Every engine event goes out as an ITCH 5.0 message into a fresh `BookBuilder`
each day, and the strategies trade only from those rebuilt books, through
pre-trade risk and the OMS, back into the same engine:

| Strategy | Symbols | Code | Decides |
| --- | --- | --- | --- |
| Market making | AAPL | `market_maker.hpp` | every book update |
| Statistical arbitrage | SEMA / SEMB | `pairs.hpp` + `work_toward()` | every 5 minutes |
| Trend following | TRND | `trend.hpp` + `work_toward()` | at each close |
| Fundamental | VALU | `thesis.hpp` + `pov_child()` | at each close; trades at 6% of volume |

```bash
./build/release/tools/quant-lab/quant-lab --days 252 --seed 3 --out /tmp/quant-lab.json
python3 research/quant_lab.py --bin build/release/tools/quant-lab/quant-lab \
    --seed 3 --seeds 1-10 --page /tmp/quant-research-lab.html
```

The run exits 1 unless every day's books have zero integrity errors and match
the engine's at the close and every OMS response is consistent. The page
shows a scoreboard, replays each strategy (900 book updates for the market
maker, the year for the others) and compares ten seeds. SEMA, SEMB, TRND and
VALU are made-up symbols and all prices are simulated; no fees or rebates
are counted.

## Colo network (option C)

The lab runs on loopback, so it has no switches, cables or NICs.
`research/colo.py` models the network the firm would sit on in an exchange
colo, using option C from the network study: the feed arrives over a 50 m
cross-connect, an Arista 7130 fans it out on Layer 1 circuits (about 4.5 ns, no
queueing) to four kernel-bypass hosts, and a 7130 MetaMux (about 39 ns) merges
their orders onto the 50 m cross-connect back to the exchange. It is a
discrete-event simulation at nanosecond resolution with port serialisation,
contention between simultaneous orders, and microbursts.

```bash
trader ... > trader.out
python3 research/colo.py --trader-log trader.out --page /tmp/colo.html --json /tmp/colo.json
```

It prints two rows. `C` uses a reference kernel-bypass host (about 2.8 µs from
the exchange to the order gateway at the median). `C-lab` replaces the host's
decision time with the trader's own "Tick to wire" histogram, between the NIC
rx and tx stages that loopback cannot measure. The page breaks each order's
time down by stage and stress-tests microbursts. Device figures live in
`research/lab_research/colo/catalog.py`, are representative rather than vendor
specs, and can be overridden with `--config`. `scripts/e2e-posttrade.sh` runs
it on its session's trader.

Latency numbers measured under WSL2 are relative, not production figures: the
Windows host schedules the VM's CPUs, and there is no kernel-bypass networking.
