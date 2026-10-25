# Interview exercise: release incident MD-414

You are on call for the trading platform. This branch is exactly what went
out in this week's release: twenty commits from several teams on top of the
last good build. Since then the reports below have come in from every desk.

CI on this branch is red, and the release went out anyway. Some reports have
more than one cause, some causes show up in more than one report, and some
defects in this release have not been reported by anyone yet. You will
probably not fix everything in the time you have. Decide what matters most,
and say what you left and why.

## The reports

> **1. Market data (overnight replay)**
> "The full-day replay used to finish in under 15 seconds. Since the release it
> never finishes; we killed it after two hours. The 5M-message test day still
> runs in normal time."
>
> Full day: `itch-gen /tmp/big.gz --messages 20000000`. Before the release,
> `itch-dump` exited 0 with book digest `b0edde0a27a2eaa4`.

> **2. Market data (vendor check)**
> "Daily volume on auction days is higher than the vendor's, sometimes by a
> lot. Normal days look closer."

> **3. Risk**
> "A test order for 900 shares at $5,000 passed pre-trade risk. That is
> $4.5M against a $100k order notional limit."

> **4. Exchange connectivity**
> "After the lunch lull the trader sent thousands of orders in one second and
> the exchange throttled our session."

> **5. Trading desk**
> "During this morning's LULD pause we sent hundreds of orders and the
> exchange rejected all of them. Later the strategy stopped quoting with
> 'position limit' while the position was flat."

> **6. Trading desk**
> "Fill rate is down and replaces are up about three times. We seem to lose
> queue position on every small move."

> **7. Compliance**
> "FIRM01 traded with itself several times on Tuesday. Self-trades are
> reportable. Why did self-match prevention not stop them?"

> **8. Operations**
> "End-of-day PnL from `research/report.py` does not agree with the risk
> system, even on days when reconciliation is clean."

> **9. Client connectivity (FIX)**
> "After a network blip a FIX client is missing two execution reports. Our
> logs show we sent them, and the session never asked for them again."

> **10. Simulation team**
> "Fills in simex no longer match what the replayed book says they should
> be, so our backtests disagree with the simulator."

> **11. Performance**
> "Last week's latency report looked better than ever, but nobody changed the
> hot path. On the colo boxes, feed handler p99 went up after the release."

## Build and run

Ubuntu 24.04 or WSL2, with `cmake`, `ninja`, a C++20 compiler, `zlib1g-dev`,
`libgtest-dev`, `libbenchmark-dev`, Python 3 with `numpy` and `pandas`:

```bash
cmake --preset release && cmake --build --preset release
ctest --preset release
python3 -m unittest discover research/tests
./build/release/tools/itch-gen/itch-gen /tmp/synth.gz --messages 5000000
./build/release/tools/itch-dump/itch-dump /tmp/synth.gz
```

The `asan` and `tsan` presets build the same tests with sanitizers.
`git log main..HEAD` shows exactly what the release changed.

## What we would like from you

1. Triage first: which problems would you stop trading for, and in what
   order would you fix them?
2. Find the root cause of each problem you take on and explain it in plain
   words.
3. Fix them in small, separate commits and open a pull request against this
   branch. Add tests that would have caught what the existing tests missed.
4. Write a short note for the desks, compliance and operations: what was
   wrong, since when, and which numbers they should not trust.

`CLAUDE.md` and `docs/specs/` describe the codebase and the protocols. Use any
tools you would use at work.
