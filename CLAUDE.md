# Trading lab

A systematic trading firm in miniature, running in WSL2: Nasdaq ITCH 5.0 replay,
a low-latency feed handler, a simulated exchange (`simex`), and a firm side
(strategy, risk, OMS, order gateways). Design: the "Trading Lab Design" tab of
the project playbook. Build phases are listed in README.md.

## Build and test

- `cmake --preset release && cmake --build --preset release && ctest --preset release`
- Also run `asan` and `tsan` presets before any change to concurrent code.
- `./build/release/tools/lab-check/lab-check` checks the machine.
- `itch-dump FILE` rebuilds all books from a day and must report zero integrity
  errors; run it on a synthetic day (`itch-gen`) after any codec or book change.
- `scripts/e2e-simex.sh` runs simex with historical liquidity, `ouch-client` and
  `itch-fh`; run it after any exchange, OUCH, SoupBinTCP or matching change.
- `scripts/e2e-firm.sh` runs `trader` against simex; run it after any OMS, risk,
  strategy or trader change.
- `scripts/e2e-posttrade.sh` adds the drop copy and runs `research/report.py`
  (reconciliation must be clean); run it after any drop copy, fill log or
  research change, and `python3 -m unittest discover research/tests`.
  It also runs `research/colo.py` on the trader's output, so keep trader's
  "Tick to wire" line format in step with `lab_research/colo.parse_tick_to_wire`.
- `scripts/e2e-fix.sh` runs `fixgw` and `fix-client` against simex; run it after
  any `libs/fix`, `fixgw` or OUCH change. `scripts/e2e-agents.sh` runs `agents`
  and `trader`; run it after any agents or trader change.
- `scripts/e2e-vendors.sh` runs the three vendor servers and `vendor-client`
  against simex with agents; run it after any `libs/vendor`, `vendors/` or
  `vendor-client` change.
- `scripts/e2e-scenarios.sh` runs simex `--scenario all` with an implied strip,
  agents, `itch-fh` and two vendors; run it after any auction, halt, LULD,
  strip or scenario change (`docs/scenarios.md`).
- `scripts/e2e-live-feed.sh` replays a day over UDP with packet loss into
  `itch-fh` and checks its book digest matches `itch-dump`'s; run it after any
  feed, sequencing or book change.
- `scripts/e2e-quant-lab.sh` runs `quant-lab` (all four strategies for a shortened
  year) and builds the Quant Research Lab page; run it after any strategy,
  `tools/quant-lab` or `research/quant_lab.py` change.
- Benchmarks: `./build/release/bench/core_bench`. Any hot-path change comes with
  before/after numbers from the same machine.

## Layout

- `libs/core`  rings, shared memory, clocks, pinning, histograms, async logger
- `libs/itch`  ITCH 5.0 codec (one field list per message drives encode and decode), file reader, synthetic day
- `libs/book`  order store, price-level books, `BookBuilder` with integrity checks
- `libs/mold`  MoldUDP64 codec, receiver sequencing (gaps, retransmission), retransmit store, fault injector
- `libs/net`  UDP unicast/multicast sockets, batched receive
- `libs/feed`  MoldUDP64 publisher and receiver threads built on `libs/mold` and `libs/net`
- `libs/soup`  SoupBinTCP packets and stream reassembly
- `libs/ouch`  OUCH 5.0 codec (same field-list pattern as `libs/itch`)
- `libs/match`  price-time matching engine (header-only, preallocated; tables on huge pages)
- `libs/fix`  FIX 4.4 framing and session layer (header-only)
- `firm/oms`, `firm/risk`, `firm/strategy`  header-only firm libraries (strategy: market maker, pairs,
  trend, thesis, execution helpers); `firm/trader` the trading engine
- `tools/quant-lab`  the Quant Research Lab: a simulated year of all four strategies on the engine,
  feed, BookBuilder, risk and OMS; `web/index.html` is the page template `research/quant_lab.py` fills
- `libs/vendor`  vendor ticker plant: `Consolidator` (Level 1 records from `BookBuilder`), a
  single-threaded distribution `Server` with conflation, and the ICF, OMM JSON/WebSocket and
  B-PIPE sessions; `vendors/{icf,rtws,bpipe}` the three servers
- `research/`  Python (numpy, pandas): PnL, reconciliation, TCA, backtests; stdlib unittest; `research/lab_research/colo`
  the option C colo network model (stdlib only) that `research/colo.py` runs
- `exchange/`, `tools/`  as in the design doc
- One library or executable per directory, each with its own CMakeLists.txt and
  tests under `tests/<lib>/`.

## Hot-path rules

- No allocation, locks, exceptions, syscalls or logging I/O on hot threads.
  Allocate and fault pages in at startup. Log through `lab::LogChannel` only.
- Messages between threads/processes are trivially copyable, fixed-size structs
  with `static_assert`s on size; no pointers in anything placed in shared memory.
- Timestamps on hot threads are raw `lab::rdtsc()` ticks; convert with
  `lab::TscClock` off the hot path.
- Measure with `lab::LatencyHistogram` (p50/p99/p99.9), never with averages.

## Protocols

- Vendor protocols: `docs/specs/vendor-omm-json.md` (public Refinitiv API, cites its
  sources); `vendor-icf.md` and `vendor-bpipe.md` are lab-defined look-alikes and are
  themselves the spec.
- ITCH 5.0, MoldUDP64, OUCH and SoupBinTCP field layouts come from the official
  spec PDFs in `docs/specs/`; FIX 4.4 tags and values from the FIX dictionary
  cited in `docs/specs/fix44-subset.md`. Never guess a field offset; if the spec is missing,
  stop and ask.
- All wire integers are big-endian: use `lab/core/endian.hpp`.
- Prices are integers (ITCH Price(4) = 1/10000 dollars). No floating point for prices.

## Data

- Sample days live in `~/data/itch/` (WSL disk, never `/mnt/c`), fetched with
  `scripts/fetch-itch-sample.sh`. Read `.gz` directly; never commit data.

## Style

- C++20, `.clang-format` (Google, 100 columns), warnings are errors in CI.
- Tests with GoogleTest, benchmarks with Google Benchmark.
