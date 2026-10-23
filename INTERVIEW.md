# Interview exercise: market data ticket MD-412

You have joined the market data team. This branch is the code that went out in
last night's release. A trader and the data quality job raised the ticket
below this morning. Work it as you would on a real desk.

## The ticket

> **MD-412: Volumes and book sizes look wrong since last night's release**
>
> *From the equities desk:* "Our end-of-day volume for our names is about 10%
> below what the exchange reports. Book sizes at the touch also look thin compared
> with what we see on the vendor screen."
>
> *From the nightly data quality job:* `itch-dump` on the reference synthetic day
> now exits with status 1. It was exit 0 before the release, with book digest
> `ed86626abd7e7a4f` and 887,329 live orders at the end of the day.

## Reproduce it

Ubuntu 24.04 or WSL2, with `cmake`, `ninja`, a C++20 compiler, `zlib1g-dev` and
`libgtest-dev` installed:

```bash
cmake --preset release && cmake --build --preset release
./build/release/tools/itch-gen/itch-gen /tmp/synth.gz --messages 5000000
./build/release/tools/itch-dump/itch-dump /tmp/synth.gz
```

`itch-gen` is deterministic, so you get the same day the data quality job used.

## What we would like from you

1. Find the root cause and explain it in plain words.
2. Fix it.
3. Show that the fix works, and how you would stop this from happening again.
4. Write a short note to the trader explaining what was wrong and whether any
   of yesterday's numbers need to be re-run.

`CLAUDE.md` and `docs/specs/itch50-layout.md` describe the codebase and the
ITCH 5.0 message layouts. You can use any tools you would use at work.
