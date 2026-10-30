# MD-415 bug bank

Sixty planted bugs (C++, market data, network, latency, trading core, order gateway; 3 easy,
4 medium and 3 hard per area), what the existing checks catch, and a scripted replay of Priya
solving each one.

- `bugs.py`: every bug as exact `(file, before, after)` edits against main, with report, cause and fix.
- `priya.py`: Priya's solve for each bug (timed steps and the regression test she adds).
- `evaluate.py`: plants each bug alone, builds, runs ctest, `itch-dump`, the e2e firm, live-feed and
  FIX scripts (plus a 20M-message day, ASan or benchmarks where the bug calls for it), restores the
  files and writes `results.json`. Generate the days first with `itch-gen` into `data/`
  (`day1m.gz` with `--messages 1000000`, `day20m.gz` with `--messages 20000000`); they are not committed.
- `build.py`: builds `md-415-replays.html` and `md-415-bug-bank.md` (the answer key) from the three files above.

`python3 interview/md-415/evaluate.py [BUG_ID ...]` then `python3 interview/md-415/build.py`.
