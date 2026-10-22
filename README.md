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
| `tools/itch-dump` | Replays a day file, rebuilds all books, prints integrity checks, top symbols, a symbol's book, per-symbol CSV |
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
| `firm/strategy` | A one-level market maker that leans against its position |
| `firm/trader` | The firm's trading engine: market data, trading and order entry threads joined by rings |
| `tools/ouch-client` | Trades random orders into simex on several accounts and checks every response |
| `tools/itch-gen` | Writes a synthetic ITCH day, for trying the tools without a multi-GB download |
| `tools/lab-check` | Checks the machine (CPUs, TSC, huge pages, loopback multicast, disk) and measures a core-to-core round trip |
| `scripts/` | WSL setup, sample data download, `.wslconfig` template |
| `docs/core-plan.md` | Which thread runs on which core |
| `docs/specs/itch50-layout.md` | ITCH 5.0 field tables the codec implements, with the source |
| `docs/specs/moldudp64-layout.md` | MoldUDP64 packet and request layouts, with the source |
| `docs/specs/soupbintcp-layout.md`, `docs/specs/ouch50-layout.md` | SoupBinTCP and OUCH 5.0 layouts, and how simex reads the spec where it leaves room |
| `scripts/e2e-live-feed.sh` | Replays a day with packet loss into `itch-fh` and checks the books match `itch-dump` |
| `scripts/e2e-simex.sh` | simex with historical liquidity, `ouch-client` trading into it and `itch-fh` on its feed; checks responses and that all books agree |
| `scripts/e2e-firm.sh` | `trader` making markets on simex against noise flow; checks risk limits, OMS consistency, no self-trades, clean shutdown |
| `bench/` | Microbenchmarks: core library, ITCH codec and books, MoldUDP64, matching engine |

## Build phases

1. **Foundations**: build system, core library, WSL setup, CI.
2. **ITCH codec and offline book**: `itch-dump` rebuilds every order book from a sample day.
3. **Live feed**: `itch-replay` (MoldUDP64, fault injection, retransmission) and `itch-fh`.
4. **Exchange**: `simex` matching engine, OUCH/SoupBinTCP, ITCH output, historical liquidity.
5. **Firm hot path** (this): order gateway, inline risk, OMS, a market-making strategy.
6. **Post-trade and research**: drop copy, PnL, TCA, Python backtests.
7. **Hardening**: profiling, tail latency, agent-based liquidity, a FIX venue.

Latency numbers measured under WSL2 are relative, not production figures: the
Windows host schedules the VM's CPUs, and there is no kernel-bypass networking.
