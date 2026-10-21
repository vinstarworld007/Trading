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
| `tools/lab-check` | Checks the machine (CPUs, TSC, huge pages, loopback multicast, disk) and measures a core-to-core round trip |
| `scripts/` | WSL setup, sample data download, `.wslconfig` template |
| `docs/core-plan.md` | Which thread runs on which core |
| `bench/` | Microbenchmarks for the core library |

## Build phases

1. **Foundations** (this): build system, core library, WSL setup, CI.
2. **ITCH codec and offline book**: `itch-dump` rebuilds every order book from a sample day.
3. **Live feed**: `itch-replay` (MoldUDP64, fault injection, retransmission) and `itch-fh`.
4. **Exchange**: `simex` matching engine, OUCH/SoupBinTCP, ITCH output, historical liquidity.
5. **Firm hot path**: order gateway, inline risk, OMS, a market-making strategy.
6. **Post-trade and research**: drop copy, PnL, TCA, Python backtests.
7. **Hardening**: profiling, tail latency, agent-based liquidity, a FIX venue.

Latency numbers measured under WSL2 are relative, not production figures: the
Windows host schedules the VM's CPUs, and there is no kernel-bypass networking.
