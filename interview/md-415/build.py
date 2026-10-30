#!/usr/bin/env python3
"""Builds md415-replays.html and the answer key from bugs.py, priya.py and results.json."""
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from bugs import BUGS  # noqa: E402
from priya import P  # noqa: E402

OUT_HTML = os.path.join(HERE, "md-415-replays.html")
OUT_KEY = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "md-415-bug-bank.md")
R = json.load(open(os.path.join(HERE, "results.json")))
BASE = R["BASE"]
CATS = [("cpp", "C++"), ("md", "Market data"), ("net", "Network"), ("lat", "Latency"),
        ("trd", "Trading core"), ("gw", "Order gateway")]
LAYERS = [("compile", "Compiler"), ("unit", "Unit tests"), ("digest", "Book digest"),
          ("firm", "e2e firm"), ("live", "e2e live feed"), ("fix", "e2e FIX"),
          ("asan", "ASan"), ("bench", "Benchmark")]


def rate(s):
    m = re.match(r"([\d.]+)([kMG]?)", s or "")
    if not m:
        return None
    return float(m.group(1)) * {"": 1, "k": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]


def digest_diff(d, base):
    """What itch-dump reports that differs from main."""
    out = []
    if d["rc"] != 0:
        out.append(f"itch-dump exited {d['rc']}")
    if d.get("digest") != base["digest"]:
        out.append(f"digest {d.get('digest') or 'missing'} (main {base['digest']})")
    for name, (st, n) in (d.get("checks") or {}).items():
        b = base["checks"].get(name)
        if st not in ("OK", "INFO") or (b and b[1] != n):
            out.append(f"{st} {name} {n}" + (f" (main {b[1]})" if b and b[1] != n else ""))
    return out


def detect(r):
    """(layers that caught it, evidence lines per layer)."""
    if not r:
        return None, {}
    ev = {}
    if not r["build"]["ok"]:
        ev["compile"] = r["build"]["errors"][:2]
        return ["compile"], ev
    c = r["ctest"]
    if c["failed"]:
        ev["unit"] = [f"{c['failed']} of 183 tests fail"] + c["tests"][:4]
    dd = digest_diff(r["digest"], BASE["digest"])
    if "bigday" in r:
        dd += ["20M-message day: " + x for x in digest_diff(r["bigday"], BASE["bigday"])]
        if r["bigday"]["secs"] > 2 * BASE["bigday"]["secs"]:
            dd.append(f"20M-message day took {r['bigday']['secs']} s (main {BASE['bigday']['secs']} s)")
    if dd:
        ev["digest"] = dd[:5]
    for k, name in (("firm", "e2e_firm"), ("live", "e2e_live-feed"), ("fix", "e2e_fix")):
        e = r.get(name)
        if e and e["rc"] != 0:
            ev[k] = [f"exit {e['rc']}" + (" (timed out)" if e["rc"] == 124 else "")] + e["lines"][-3:]
    a = r.get("asan")
    if a and a["failed"]:
        ev["asan"] = [f"{a['failed']} tests fail under ASan"] + a["tests"][:3]
    for key, base in (("bench_book", BASE["bench_book"]), ("bench_ring", BASE["bench_ring"])):
        b = r.get(key)
        if b and b["median"]:
            now, was = rate(b["median"][0]), rate(base["median"][0])
            if now and was and now < 0.85 * was:
                ev["bench"] = [f"{'BM_ItchBookRebuild' if key == 'bench_book' else 'BM_RingCrossThread'} "
                               f"{b['median'][0]}/s (main {base['median'][0]}/s, "
                               f"{round(100 * (1 - now / was))}% slower)"]
    return [k for k, _ in LAYERS if k in ev], ev


def checks_line(r):
    """What a CI run with the bug planted prints, one line per check."""
    if not r:
        return "not run yet", ""
    if not r["build"]["ok"]:
        return "\n".join(r["build"]["errors"][:3]), "bad"
    lines, bad = [], False
    c = r["ctest"]
    if c["failed"]:
        bad = True
        lines.append(f"ctest: {c['failed']} of 183 FAILED: " + ", ".join(c["tests"][:3]))
    else:
        lines.append("ctest: 100% tests passed, 0 failed out of 183")
    dd = digest_diff(r["digest"], BASE["digest"])
    lines.append("itch-dump: " + ("; ".join(dd[:2]) if dd else f"digest {r['digest']['digest']}, integrity OK"))
    bad |= bool(dd)
    for k, name in (("firm", "e2e_firm"), ("live-feed", "e2e_live-feed"), ("fix", "e2e_fix")):
        e = r.get(name)
        if e:
            ok = e["rc"] == 0
            bad |= not ok
            lines.append(f"e2e-{k}: " + ("PASS" if ok else f"FAIL (exit {e['rc']})"))
    return "\n".join(lines), ("bad" if bad else "good")


def bug_json(b):
    r = R.get(b["id"])
    layers, ev = detect(r)
    p = P[b["id"]]
    out, cls = checks_line(r)
    steps = [[0, "ci", "CI on the commit with the bug: ctest, itch-dump, e2e firm / live feed / FIX", out, cls]]
    if layers:
        first = ev[layers[0]]
        steps.append([1 if p["steps"][0][0] > 1 else 0, "think",
                      f"CI is red ({dict(LAYERS)[layers[0]]}: {first[1] if len(first) > 1 else first[0]}). "
                      "She starts from that failure and reads the desk report next to it.", "", ""])
    elif layers is not None:
        steps.append([0, "think", "CI is green, so the checks are no help. She starts from the desk report "
                      "and reproduces it by hand.", "", ""])
    for m, kind, text in p["steps"]:
        steps.append([m, kind, text, "", ""])
    tname, tfile = p["test"]
    last = max(m for m, _, _ in p["steps"])
    tm = min(max(last + 1, p["minutes"] - 2), p["minutes"] - 1)
    steps.append([tm, "test", f"Adds {tname} in {tfile}", "fails on the bug, passes with the fix", "good"])
    steps.append([p["minutes"], "verify", "ctest, itch-dump and the three e2e scripts again",
                  f"183 tests + the new one pass · digest {BASE['digest']['digest']} · e2e firm, live feed, FIX PASS",
                  "good"])
    edits = [[f, o.strip("\n"), n.strip("\n")] for f, o, n in b["edits"]]
    return dict(id=b["id"], cat=b["cat"], level=b["level"], title=b["title"], report=b["report"],
                cause=b["cause"], fix=b["fix"], test=[tname, tfile], minutes=p["minutes"],
                steps=steps, edits=edits, layers=layers, ev=ev,
                secs=(r or {}).get("secs"))


def main():
    bugs = [bug_json(b) for b in BUGS.values()]
    data = dict(bugs=bugs, cats=CATS, layers=LAYERS,
                base=dict(digest=BASE["digest"]["digest"], big=BASE["bigday"]["digest"]))
    html = open(os.path.join(HERE, "page.html")).read()
    html = html.replace("/*DATA*/", "const DATA=" + json.dumps(data, separators=(",", ":"))
                        .replace("</", "<\\/") + ";")
    open(OUT_HTML, "w").write(html)
    write_key(bugs)
    done = [b for b in bugs if b["layers"] is not None]
    silent = [b["id"] for b in done if not b["layers"]]
    print(f"{len(done)}/60 evaluated; silent: {silent}")
    for b in done:
        print(b["id"], b["level"], ",".join(b["layers"]) or "SILENT")


def write_key(bugs):
    names = dict(LAYERS)
    L = ["# MD-415 bug bank: 60 planted bugs and how Priya solves them", "",
         "Every bug is a small patch against `vinstarworld007/Trading` main (5f398f4). "
         "Each was planted alone, built, and run through ctest (183 tests), `itch-dump` on a "
         "1M-message synthetic day, and `e2e-firm`, `e2e-live-feed` and `e2e-fix`; the bigger "
         "suspects also ran a 20M-message day, ASan, or the book and ring benchmarks. "
         "\"Caught by\" is what actually failed. **Silent** means every existing check passed.", "",
         "To plant a bug: `md-415-bugs.py` next to this file holds every patch as "
         "`BUGS[id][\"edits\"] = [(file, before, after)]`. In a clean checkout of main, replace "
         "`before` with `after` in each file; every `before` string occurs exactly once. "
         "`git checkout -- <file>` undoes it.", ""]
    for key, label in CATS:
        L += [f"## {label}", "",
              "| ID | Level | Bug | Caught by | Priya's time | Her regression test |",
              "|---|---|---|---|---|---|"]
        for b in [x for x in bugs if x["cat"] == key]:
            caught = ("not run" if b["layers"] is None else
                      ", ".join(names[k] for k in b["layers"]) or "**Silent**")
            L.append(f"| {b['id']} | {b['level']} | {b['title']} | {caught} | {b['minutes']} min | "
                     f"`{b['test'][0]}` |")
        L.append("")
        for b in [x for x in bugs if x["cat"] == key]:
            L += [f"### {b['id']} · {b['title']} ({b['level']})", "",
                  f"- **Report:** {b['report']}", f"- **Cause:** {b['cause']}",
                  f"- **Fix:** {b['fix']}",
                  f"- **Regression test:** `{b['test'][0]}` in `{b['test'][1]}`"]
            for k, lines in b["ev"].items():
                L.append(f"- **{names[k]}:** " + "; ".join(lines))
            for f, o, n in b["edits"]:
                L += ["", f"`{f}`", "```diff"] + ["- " + x for x in o.splitlines()] + \
                     ["+ " + x for x in n.splitlines()] + ["```"]
            L.append("")
    open(OUT_KEY, "w").write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
