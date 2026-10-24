# Interview exercise: market data ticket MD-413

You have joined the market data team. This branch is what went out in this
week's release. Three reports came in over the last two days. They may or may
not be related. Work them as you would on a real desk.

## The reports

> **1. Overnight replay job (data quality)**
> "The full-day replay `itch-dump` failed twice this week with a handful of
> unknown order references. The standard 5M-message test day is still clean, so
> nobody can reproduce it locally."
>
> To reproduce the full day: `itch-gen /tmp/big.gz --messages 20000000`
> (takes about a minute). Before the release it exited 0 with book digest
> `b0edde0a27a2eaa4` and 3,567,340 live orders.

> **2. Equities desk**
> "The end-of-day report shows SYM0011 opening at 16.600. The vendor screen says
> 16.06. Some other names look off too, but not all of them."

> **3. Operations**
> "Our PnL for FIRM01 is off from the exchange's statement, but
> `research/report.py` says reconciliation is clean. If reconciliation is clean,
> how can PnL be different?"

## Build and run

Ubuntu 24.04 or WSL2, with `cmake`, `ninja`, a C++20 compiler, `zlib1g-dev`,
`libgtest-dev`, Python 3 with `numpy` and `pandas`:

```bash
cmake --preset release && cmake --build --preset release
ctest --preset release
python3 -m unittest discover research/tests
./build/release/tools/itch-gen/itch-gen /tmp/synth.gz --messages 5000000
./build/release/tools/itch-dump/itch-dump /tmp/synth.gz
```

## What we would like from you

1. Find the root cause of each report and explain it in plain words.
2. Fix them in small, separate commits, and open a pull request against this
   branch.
3. Show that each fix works, and add tests that would have caught it.
4. Write a short note to the desk and to operations: what was wrong, since
   when, and which numbers they should not trust.

`CLAUDE.md` and `docs/specs/` describe the codebase and the protocols. Use any
tools you would use at work.
