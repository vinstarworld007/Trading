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
| `tools/itch-gen` | Writes a synthetic ITCH day, for trying the tools without a multi-GB download |
| `tools/lab-check` | Checks the machine (CPUs, TSC, huge pages, loopback multicast, disk) and measures a core-to-core round trip |
| `scripts/` | WSL setup, sample data download, `.wslconfig` template |
| `docs/core-plan.md` | Which thread runs on which core |
| `docs/specs/itch50-layout.md` | ITCH 5.0 field tables the codec implements, with the source |
| `docs/specs/moldudp64-layout.md` | MoldUDP64 packet and request layouts, with the source |
| `scripts/e2e-live-feed.sh` | Replays a day with packet loss into `itch-fh` and checks the books match `itch-dump` |
| `bench/` | Microbenchmarks: core library, ITCH codec and books, MoldUDP64 |

## Build phases

1. **Foundations**: build system, core library, WSL setup, CI.
2. **ITCH codec and offline book**: `itch-dump` rebuilds every order book from a sample day.
3. **Live feed** (this): `itch-replay` (MoldUDP64, fault injection, retransmission) and `itch-fh`.
4. **Exchange**: `simex` matching engine, OUCH/SoupBinTCP, ITCH output, historical liquidity.
5. **Firm hot path**: order gateway, inline risk, OMS, a market-making strategy.
6. **Post-trade and research**: drop copy, PnL, TCA, Python backtests.
7. **Hardening**: profiling, tail latency, agent-based liquidity, a FIX venue.

Latency numbers measured under WSL2 are relative, not production figures: the
Windows host schedules the VM's CPUs, and there is no kernel-bypass networking.
