#!/usr/bin/env python3
"""Plants each MD-415 bug alone on main, builds, and records what catches it.

Results go to results.json (one entry per bug, rewritten after each bug so a
partial run is usable). Usage: evaluate.py [BUG_ID ...]
"""
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bugs import BUGS  # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OUT = os.path.join(HERE, "results.json")
DAY = os.path.join(HERE, "data", "day1m.gz")
BIG = os.path.join(HERE, "data", "day20m.gz")


def sh(cmd, timeout=900, cwd=ROOT):
    t0 = time.time()
    try:
        p = subprocess.run(cmd, shell=True, cwd=cwd, capture_output=True, text=True,
                           timeout=timeout)
        return p.returncode, p.stdout + p.stderr, time.time() - t0
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        return 124, out, time.time() - t0


def failing_tests(out):
    return re.findall(r"\d+ - (\S+) \((Failed|Timeout|SEGFAULT|Subprocess aborted|Exception)\)", out)


def dump(path, timeout=600):
    rc, out, secs = sh(f"build/release/tools/itch-dump/itch-dump {path}", timeout=timeout)
    d = re.search(r"[Dd]igest\s+([0-9a-f]{16})", out)
    live = re.search(r"Live orders\s+(\d+)", out)
    checks, bad, sect = {}, [], False
    for l in out.splitlines():
        if l.startswith("Integrity checks"):
            sect = True
            continue
        if sect and not l.startswith("  "):
            sect = False
        m = re.match(r"\s+([A-Z]+)\s+(.+?)\s+(\d+)$", l) if sect else None
        if m:
            checks[m.group(2)] = [m.group(1), int(m.group(3))]
            if m.group(1) not in ("OK", "INFO"):
                bad.append(f"{m.group(1)} {m.group(2)} {m.group(3)}")
    return dict(rc=rc, digest=d.group(1) if d else None, secs=round(secs, 1),
                live=int(live.group(1)) if live else None, checks=checks, bad=bad,
                tail=out.strip().splitlines()[-3:] if rc else [])


def bench(binary, flt):
    rc, out, _ = sh(f"build/release/bench/{binary} --benchmark_filter='{flt}' "
                    "--benchmark_repetitions=3 --benchmark_report_aggregates_only=true", 300)
    m = re.findall(r"_median\s+.*?(\d[\d.]*[kMG]?)/s", out)
    return dict(rc=rc, median=m[:2], tail=out.strip().splitlines()[-4:])


def e2e(name):
    rc, out, secs = sh(f"timeout 300 scripts/e2e-{name}.sh", timeout=330)
    lines = [l for l in out.splitlines() if re.search(r"CHECK FAILED|FAIL|PASS|error|Error", l)]
    return dict(rc=rc, secs=round(secs, 1), lines=lines[-6:])


def apply(b, reverse=False):
    for f, old, new in b["edits"]:
        p = os.path.join(ROOT, f)
        s = open(p).read()
        a, z = (new, old) if reverse else (old, new)
        if z and s.count(a) != 1 and not reverse:
            raise RuntimeError(f"{b['id']}: edit not found in {f}")
        s = s.replace(a, z, 1) if a else s
        open(p, "w").write(s)


def restore(b):
    files = sorted({f for f, _, _ in b["edits"]})
    sh("git checkout -- " + " ".join(files))


def evaluate(b, base):
    r = dict(id=b["id"])
    apply(b)
    try:
        rc, out, secs = sh("cmake --build --preset release 2>&1 | tail -40", timeout=1800)
        build_ok = "error:" not in out and "FAILED" not in out
        r["build"] = dict(ok=build_ok, secs=round(secs), errors=[l for l in out.splitlines()
                                                                  if "error" in l][:4])
        if not build_ok:
            return r
        rc, out, secs = sh("ctest --preset release -j4 --timeout 60 2>&1 | tail -40", 900)
        m = re.search(r"(\d+) tests failed out of (\d+)", out)
        r["ctest"] = dict(failed=int(m.group(1)) if m else (0 if "100% tests passed" in out else -1),
                          tests=[t for t, _ in failing_tests(out)][:12], secs=round(secs))
        d = dump(DAY)
        r["digest"] = d
        r["digest_changed"] = d["digest"] != base["digest"]["digest"]
        for name in ("firm", "live-feed", "fix"):
            r["e2e_" + name] = e2e(name)
        if "bigday" in b["checks"] or b["id"] in ("CPP09", "CPP10"):
            r["bigday"] = dump(BIG, timeout=900)
        if "bench_book" in b["checks"]:
            r["bench_book"] = bench("itch_bench", "BM_ItchBookRebuild")
        if "bench_ring" in b["checks"]:
            r["bench_ring"] = bench("core_bench", "BM_RingCrossThread")
        if "asan" in b["checks"]:
            rc, out, secs = sh("cmake --build --preset asan 2>&1 | tail -5 && "
                               "ctest --preset asan -j4 --timeout 120 2>&1 | tail -40", 3600)
            m = re.search(r"(\d+) tests failed out of (\d+)", out)
            r["asan"] = dict(failed=int(m.group(1)) if m else (0 if "100% tests passed" in out else -1),
                             tests=[t for t, _ in failing_tests(out)][:12], secs=round(secs))
    finally:
        restore(b)
    return r


def main():
    results = json.load(open(OUT)) if os.path.exists(OUT) else {}
    if "BASE" in results and "checks" not in results["BASE"]["digest"]:
        results["BASE"]["digest"] = dump(DAY)
        results["BASE"]["bigday"] = dump(BIG, 900)
        json.dump(results, open(OUT, "w"), indent=1)
    if "BASE" not in results:
        sh("cmake --build --preset release", 3600)
        os.makedirs(os.path.dirname(DAY), exist_ok=True)
        if not os.path.exists(DAY):
            sh(f"build/release/tools/itch-gen/itch-gen {DAY} --messages 1000000")
        if not os.path.exists(BIG):
            sh(f"build/release/tools/itch-gen/itch-gen {BIG} --messages 20000000", 1800)
        results["BASE"] = dict(digest=dump(DAY), bigday=dump(BIG, 900),
                               bench_book=bench("itch_bench", "BM_ItchBookRebuild"),
                               bench_ring=bench("core_bench", "BM_RingCrossThread"))
        json.dump(results, open(OUT, "w"), indent=1)
    ids = sys.argv[1:] or list(BUGS)
    for i in ids:
        if i in results and not sys.argv[1:]:
            continue
        t0 = time.time()
        results[i] = evaluate(BUGS[i], results["BASE"])
        results[i]["secs"] = round(time.time() - t0)
        json.dump(results, open(OUT, "w"), indent=1)
        print(i, json.dumps({k: v for k, v in results[i].items() if k in ("build", "ctest")}),
              flush=True)
    # Rebuild clean so the tree matches main.
    sh("cmake --build --preset release", 3600)


if __name__ == "__main__":
    main()
