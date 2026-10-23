"""News sources: JSONL files and RSS feeds, plus signal-date bucketing.

JSONL format, one item per line::

    {"ts": "2026-03-04T14:05:00Z", "source": "reuters", "headline": "...",
     "body": "...", "tickers": ["NVDA"]}

``body`` and ``tickers`` are optional.
"""

from __future__ import annotations

import bisect
import json
import logging
import urllib.request
import xml.etree.ElementTree as ET
from collections import defaultdict
from datetime import date, datetime, time, timezone
from email.utils import parsedate_to_datetime
from pathlib import Path
from zoneinfo import ZoneInfo

from ..models import NewsItem

log = logging.getLogger(__name__)

NY = ZoneInfo("America/New_York")
CLOSE = time(16, 0)


def _parse_ts(s: str) -> datetime:
    ts = datetime.fromisoformat(s.replace("Z", "+00:00"))
    return ts if ts.tzinfo else ts.replace(tzinfo=timezone.utc)


def load_jsonl(path: str | Path) -> list[NewsItem]:
    items = []
    for n, line in enumerate(Path(path).read_text().splitlines(), 1):
        if not line.strip():
            continue
        row = json.loads(line)
        try:
            items.append(NewsItem(_parse_ts(row["ts"]), row.get("source", "unknown"), row["headline"],
                                  row.get("body", ""), list(row.get("tickers", [])), row.get("id", "")))
        except KeyError as e:
            raise ValueError(f"{path}:{n}: missing field {e}") from None
    return sorted(items, key=lambda i: i.ts)


def save_jsonl(items: list[NewsItem], path: str | Path) -> None:
    with Path(path).open("w") as f:
        for i in items:
            f.write(json.dumps({"id": i.id, "ts": i.ts.astimezone(timezone.utc).isoformat(),
                                "source": i.source, "headline": i.headline, "body": i.body,
                                "tickers": i.tickers}) + "\n")


def session_date(ts: datetime, sessions: list[date]) -> date | None:
    """The session whose close is the first one at or after ``ts``.

    News before the 16:00 ET close belongs to that day; later news, weekend
    news and holiday news roll to the next session. ``None`` past the last one.
    """
    local = ts.astimezone(NY)
    d = local.date()
    after_close = local.time() >= CLOSE
    i = bisect.bisect_left(sessions, d)
    if i < len(sessions) and sessions[i] == d and after_close:
        i += 1
    return sessions[i] if i < len(sessions) else None


def bucket_by_session(items: list[NewsItem], sessions: list[date]) -> dict[date, list[NewsItem]]:
    out: dict[date, list[NewsItem]] = defaultdict(list)
    for item in items:
        d = session_date(item.ts, sessions)
        if d is not None:
            out[d].append(item)
    return out


# --- RSS -------------------------------------------------------------------

FEEDS = {
    "yahoo": "https://feeds.finance.yahoo.com/rss/2.0/headline?s={ticker}&region=US&lang=en-US",
    "nasdaq": "https://www.nasdaq.com/feed/rssoutbound?symbol={ticker}",
}
MACRO_FEEDS = {
    "cnbc": "https://search.cnbc.com/rs/search/combinedcms/view.xml?partnerId=wrss01&id=10000664",
    "marketwatch": "https://feeds.content.dowjones.io/public/rss/mw_topstories",
}


def parse_rss(xml_text: str, source: str, ticker: str | None = None) -> list[NewsItem]:
    root = ET.fromstring(xml_text)
    out = []
    for it in root.iter("item"):
        title = (it.findtext("title") or "").strip()
        pub = it.findtext("pubDate")
        if not title or not pub:
            continue
        try:
            ts = parsedate_to_datetime(pub)
        except (TypeError, ValueError):
            continue
        if ts.tzinfo is None:
            ts = ts.replace(tzinfo=timezone.utc)
        desc = (it.findtext("description") or "").strip()
        out.append(NewsItem(ts.astimezone(timezone.utc), source, title, desc[:500],
                            [ticker] if ticker else []))
    return out


def fetch_rss(tickers: list[str], feeds: dict[str, str] | None = None, macro: bool = True,
              timeout: float = 10.0) -> list[NewsItem]:
    """Recent headlines for ``tickers`` plus general market news. Best effort."""
    feeds = feeds or FEEDS
    jobs = [(src, url.format(ticker=t), t) for t in tickers for src, url in feeds.items()]
    if macro:
        jobs += [(src, url, None) for src, url in MACRO_FEEDS.items()]
    items: dict[str, NewsItem] = {}
    for src, url, t in jobs:
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "mirofish-trader/0.1"})
            with urllib.request.urlopen(req, timeout=timeout) as r:
                for i in parse_rss(r.read().decode("utf-8", "replace"), src, t):
                    items.setdefault(i.headline.lower(), i)
        except Exception as e:  # noqa: BLE001 - one dead feed must not stop the run
            log.warning("feed %s failed: %s", url, e)
    return sorted(items.values(), key=lambda i: i.ts)
