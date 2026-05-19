#!/usr/bin/env python3
"""
romraider_forum_system.py
=========================

Local metadata / index / download-management system for the RomRaider phpBB3
forum (https://www.romraider.com/forum/).

Authorized, local use only. Reads cookies from a Netscape cookies.txt exported
from the user's logged-in browser. One request at a time, polite delays, no
concurrency, no bypass behaviour. Mirrors the design of epifan_ecu_system.py
in the same directory.

Python 3.11+. Requires: requests, beautifulsoup4.

Workflow
--------
    python romraider_forum_system.py discover            # find subforums
    python romraider_forum_system.py enable 15 8 47      # mark interesting ones
    python romraider_forum_system.py index               # walk enabled forums
    python romraider_forum_system.py status              # what do we have?
    python romraider_forum_system.py list-attachments    # show what's downloadable
    python romraider_forum_system.py download            # fetch (resumable)

All subcommands are resumable. Re-running picks up where we stopped.
"""

from __future__ import annotations

import argparse
import http.cookiejar
import json
import random
import re
import sqlite3
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterable, Optional
from urllib.parse import urljoin, urlparse, parse_qs

try:
    import requests
    from bs4 import BeautifulSoup
except ImportError:
    print(
        "Missing dependency. Run: pip install requests beautifulsoup4",
        file=sys.stderr,
    )
    raise

try:
    from forum_dl_organize import target_path, place_file
    _ORGANIZE_OK = True
except ImportError:
    _ORGANIZE_OK = False


# --------------------------------------------------------------------------- #
# Constants
# --------------------------------------------------------------------------- #

BASE_DIR = Path(__file__).resolve().parent
DEFAULT_COOKIES = BASE_DIR / "roms_extracted" / "forumdownloads" / "cookies.txt"
SESSION_COOKIES = BASE_DIR / "roms_extracted" / "forumdownloads" / "cookies_session.txt"
DEFAULT_OUTPUT_DIR = BASE_DIR / "roms_extracted" / "forumdownloads"
DB_PATH = BASE_DIR / "romraider_forum_index.sqlite"
ATTEMPTS_JSONL = DEFAULT_OUTPUT_DIR / "download_attempts.jsonl"

FORUM_BASE = "https://www.romraider.com/forum/"
TOPICS_PER_PAGE = 50  # phpBB3 default — RomRaider's fisubsilver2 returns ~38

UA_CHROME = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)

# Hard floors. Cannot be overridden by flags.
MIN_DELAY_SECONDS = 1.0
DEFAULT_DELAY_SECONDS = 4.0

CF_TITLE_MARKERS = (
    "Just a moment",
    "Attention Required",
    "One more step",
    "Please Wait",
)

# Keywords for auto-enabling subforums on a first `discover` run.
INTERESTING_KEYWORDS = (
    "subaru",
    "rom",
    "ecu",
    "definition",
    "logger",
    "tune",
    "tuning",
    "calib",
)

# Subforums to never enable automatically (off-topic for ROM work).
EXCLUDED_FOREVER = {"News", "Donate", "Lounge", "phpBB Problems/Testing"}


# --------------------------------------------------------------------------- #
# DB
# --------------------------------------------------------------------------- #

SCHEMA = """
CREATE TABLE IF NOT EXISTS subforums (
    f          INTEGER PRIMARY KEY,
    title      TEXT NOT NULL,
    url        TEXT NOT NULL,
    enabled    INTEGER NOT NULL DEFAULT 0,
    last_indexed_at TEXT
);

CREATE TABLE IF NOT EXISTS topics (
    t              INTEGER PRIMARY KEY,
    f              INTEGER NOT NULL,
    title          TEXT,
    url            TEXT NOT NULL,
    has_attachments INTEGER NOT NULL DEFAULT 0,
    pages_indexed  INTEGER NOT NULL DEFAULT 0,
    last_indexed_at TEXT,
    FOREIGN KEY (f) REFERENCES subforums(f)
);

CREATE TABLE IF NOT EXISTS attachments (
    aid            INTEGER PRIMARY KEY,
    t              INTEGER NOT NULL,
    post_id        INTEGER,
    filename       TEXT,
    byte_size      INTEGER,
    posted_at      TEXT,
    poster         TEXT,
    comment        TEXT,
    discovered_at  TEXT NOT NULL,
    FOREIGN KEY (t) REFERENCES topics(t)
);

CREATE TABLE IF NOT EXISTS attempts (
    rowid           INTEGER PRIMARY KEY AUTOINCREMENT,
    aid             INTEGER NOT NULL,
    ts              TEXT NOT NULL,
    http_status     INTEGER,
    classification  TEXT NOT NULL,
    saved_path      TEXT,
    bytes           INTEGER,
    error           TEXT,
    FOREIGN KEY (aid) REFERENCES attachments(aid)
);

CREATE INDEX IF NOT EXISTS idx_topics_f ON topics(f);
CREATE INDEX IF NOT EXISTS idx_attachments_t ON attachments(t);
CREATE INDEX IF NOT EXISTS idx_attempts_aid ON attempts(aid);
"""


def db_open() -> sqlite3.Connection:
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(DB_PATH)
    conn.row_factory = sqlite3.Row
    conn.executescript(SCHEMA)
    return conn


def now_iso() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


# --------------------------------------------------------------------------- #
# Session
# --------------------------------------------------------------------------- #


def _load_jar(path: Path) -> http.cookiejar.MozillaCookieJar:
    jar = http.cookiejar.MozillaCookieJar()
    jar.load(str(path), ignore_discard=True, ignore_expires=True)
    return jar


def _save_jar(jar: http.cookiejar.MozillaCookieJar, path: Path) -> None:
    # MozillaCookieJar.save needs a path that exists or a writable parent dir.
    path.parent.mkdir(parents=True, exist_ok=True)
    # Temporarily swap the filename so .save() writes to our path.
    prev = jar.filename
    jar.filename = str(path)
    try:
        jar.save(ignore_discard=True, ignore_expires=True)
    finally:
        jar.filename = prev


def build_session(cookies_path: Path) -> requests.Session:
    """
    Load cookies. Prefers the rotating session file (cookies_session.txt) if it
    is newer than the user-managed cookies.txt — phpBB3 rotates the autologin
    key on every request, so we MUST persist the live jar between runs or the
    second invocation will be reset to anonymous.
    """
    candidates = []
    if SESSION_COOKIES.exists():
        candidates.append(SESSION_COOKIES)
    if cookies_path.exists():
        candidates.append(cookies_path)
    if not candidates:
        raise SystemExit(f"No cookies found at {cookies_path} or {SESSION_COOKIES}")
    # Pick the most recently modified file as the source of truth.
    chosen = max(candidates, key=lambda p: p.stat().st_mtime)
    print(f"Loading cookies from: {chosen}")
    jar = _load_jar(chosen)

    s = requests.Session()
    s.cookies = jar  # type: ignore[assignment]
    s.headers.update(
        {
            "User-Agent": UA_CHROME,
            "Accept": (
                "text/html,application/xhtml+xml,application/xml;q=0.9,"
                "image/avif,image/webp,*/*;q=0.8"
            ),
            "Accept-Language": "en-US,en;q=0.5",
            # Brotli omitted on purpose; the `brotli` pip package isn't a hard
            # dep and we don't want garbled bodies if it's absent.
            "Accept-Encoding": "gzip, deflate",
            "DNT": "1",
            "Upgrade-Insecure-Requests": "1",
        }
    )

    # Persist the rotating cookies after every authenticated response. We
    # detect anonymous resets via the phpbb3_*_u=1 (guest) cookie and refuse
    # to overwrite a good session file with that state.
    def _persist_hook(resp, *a, **kw):
        try:
            cur = {c.name: c.value for c in jar}
            u_keys = [k for k in cur if k.startswith("phpbb3_") and k.endswith("_u")]
            if not u_keys:
                return  # No phpbb cookie yet — nothing useful to save.
            u_val = cur[u_keys[0]]
            if u_val == "1" or not u_val:
                # Anonymous reset. Don't trample a previously-good session file.
                return
            _save_jar(jar, SESSION_COOKIES)
        except Exception as e:
            print(f"  (warning: failed to save session cookies: {e})", file=sys.stderr)

    s.hooks["response"].append(_persist_hook)
    return s


def sleep_with_jitter(base: float) -> None:
    base = max(base, MIN_DELAY_SECONDS)
    # +/- 20% jitter — looks more human and reduces hammering signature.
    delay = base * (0.8 + 0.4 * random.random())
    time.sleep(delay)


# --------------------------------------------------------------------------- #
# Response classification
# --------------------------------------------------------------------------- #


def _title(body: str) -> str:
    m = re.search(r"<title[^>]*>([^<]+)</title>", body, re.IGNORECASE)
    return m.group(1).strip() if m else ""


def classify_html(resp: requests.Response) -> str:
    body = resp.text or ""
    if resp.status_code in (403, 503) and "cloudflare" in body.lower():
        return "cf_block"
    title = _title(body)
    if any(m.lower() in title.lower() for m in CF_TITLE_MARKERS):
        return "cf_challenge"
    if resp.status_code != 200:
        return f"http_{resp.status_code}"
    if "<html" not in body.lower():
        return "non_html"
    return "html_ok"


def classify_attachment(resp: requests.Response) -> str:
    if resp.status_code in (403, 503) and "cloudflare" in (resp.text or "").lower():
        return "cf_block"
    if resp.status_code != 200:
        return f"http_{resp.status_code}"
    ct = (resp.headers.get("content-type") or "").lower()
    cd = (resp.headers.get("content-disposition") or "").lower()
    if "html" in ct:
        # Forum returned an HTML error page in lieu of the file.
        title = _title(resp.text or "")
        if any(m.lower() in title.lower() for m in CF_TITLE_MARKERS):
            return "cf_challenge"
        return "html_error"
    if "image/" in ct:
        return "image"
    if (
        "application/octet-stream" in ct
        or "application/zip" in ct
        or "application/x-" in ct
        or "attachment" in cd
    ):
        return "binary"
    if "text/" in ct:
        return "text"
    return "unknown"


# --------------------------------------------------------------------------- #
# Page fetching helpers
# --------------------------------------------------------------------------- #


def fetch_html(s: requests.Session, url: str) -> tuple[str, requests.Response]:
    r = s.get(url, timeout=30, allow_redirects=True)
    cls = classify_html(r)
    if cls != "html_ok":
        raise RuntimeError(f"non-OK HTML fetch: {url} -> {cls} (status={r.status_code})")
    return r.text, r


def check_login(s: requests.Session) -> Optional[str]:
    """Return the logged-in username, or None if anonymous."""
    html, _ = fetch_html(s, FORUM_BASE)
    soup = BeautifulSoup(html, "html.parser")
    # phpBB3 puts the logout link as: Logout [ <username> ]
    for a in soup.find_all("a", href=True):
        if "mode=logout" in a["href"]:
            txt = a.get_text(strip=True)
            m = re.search(r"\[\s*([^\]]+?)\s*\]", txt)
            return m.group(1) if m else txt
    return None


# --------------------------------------------------------------------------- #
# discover
# --------------------------------------------------------------------------- #


def cmd_discover(args: argparse.Namespace) -> int:
    s = build_session(Path(args.cookies))
    user = check_login(s)
    print(f"Logged in as: {user or '(anonymous)'}")
    if user is None and not args.allow_anonymous:
        print(
            "Refusing to proceed anonymously. Re-export cookies.txt from a "
            "logged-in browser, or pass --allow-anonymous.",
            file=sys.stderr,
        )
        return 2

    sleep_with_jitter(args.delay)
    html, resp = fetch_html(s, urljoin(FORUM_BASE, "index.php"))
    soup = BeautifulSoup(html, "html.parser")

    forums: list[dict] = []
    seen: set[int] = set()
    for a in soup.find_all("a", href=True):
        if "viewforum.php" not in a["href"]:
            continue
        absolute = urljoin(resp.url, a["href"])
        f_id_str = parse_qs(urlparse(absolute).query).get("f", [None])[0]
        if not f_id_str:
            continue
        f_id = int(f_id_str)
        if f_id in seen:
            continue
        seen.add(f_id)
        title = a.get_text(strip=True) or f"f={f_id}"
        forums.append({"f": f_id, "title": title, "url": absolute})

    conn = db_open()
    with conn:
        for fm in forums:
            existing = conn.execute(
                "SELECT enabled FROM subforums WHERE f=?", (fm["f"],)
            ).fetchone()
            auto = (
                any(k in fm["title"].lower() for k in INTERESTING_KEYWORDS)
                and fm["title"] not in EXCLUDED_FOREVER
            )
            if existing is None:
                conn.execute(
                    "INSERT INTO subforums(f, title, url, enabled) VALUES(?,?,?,?)",
                    (fm["f"], fm["title"], fm["url"], 1 if auto else 0),
                )
            else:
                # Update title/url; keep enabled as user has set it.
                conn.execute(
                    "UPDATE subforums SET title=?, url=? WHERE f=?",
                    (fm["title"], fm["url"], fm["f"]),
                )
    print(f"Discovered {len(forums)} subforums (saved to {DB_PATH}).")

    rows = conn.execute(
        "SELECT f, title, enabled FROM subforums ORDER BY enabled DESC, f"
    ).fetchall()
    print("\nf      enabled  title")
    print("-----  -------  -----")
    for r in rows:
        print(f"{r['f']:>5}  {'YES' if r['enabled'] else ' . ':>7}  {r['title']}")
    return 0


# --------------------------------------------------------------------------- #
# enable / disable
# --------------------------------------------------------------------------- #


def cmd_enable(args: argparse.Namespace) -> int:
    conn = db_open()
    flag = 1 if args._action == "enable" else 0
    with conn:
        for f in args.f:
            cur = conn.execute(
                "UPDATE subforums SET enabled=? WHERE f=?", (flag, f)
            )
            if cur.rowcount == 0:
                print(f"  f={f} not in DB; run `discover` first", file=sys.stderr)
            else:
                print(f"  f={f} -> enabled={flag}")
    return 0


# --------------------------------------------------------------------------- #
# index
# --------------------------------------------------------------------------- #


_ATTACH_ICON_RE = re.compile(r"(icon_topic_attach|attach|paperclip)", re.IGNORECASE)
_ATTACH_ALT_RE = re.compile(r"^attachment", re.IGNORECASE)


def _row_has_attachment(row) -> bool:
    """Detect the phpBB3 paperclip marker. Handles both the modern prosilver
    template (CSS classes or <img>) and the legacy sub-silver/fisubsilver2
    imageset which renders `<img alt="Attachment(s)" src=".../icon_topic_attach.gif">`."""
    if row.find("img", src=_ATTACH_ICON_RE):
        return True
    if row.find("img", attrs={"alt": _ATTACH_ALT_RE}):
        return True
    if row.find(class_=_ATTACH_ICON_RE):
        return True
    return False


def parse_topic_listing(html: str, page_url: str) -> list[dict]:
    """Extract topics on a viewforum.php page. Returns rows with attachment
    indicator if the topic row shows a paperclip icon. Supports both the modern
    phpBB3 prosilver template (ul.topiclist.topics li.row) and the legacy
    table-based sub-silver/fisubsilver2 imageset (tr containing topictitle)."""
    soup = BeautifulSoup(html, "html.parser")
    out: list[dict] = []

    # Prefer modern template if present
    rows = soup.select("ul.topiclist.topics li")
    if rows:
        for row in rows:
            a = row.find("a", class_="topictitle", href=True)
            if not a:
                continue
            absolute = urljoin(page_url, a["href"])
            t = parse_qs(urlparse(absolute).query).get("t", [None])[0]
            if not t:
                continue
            out.append(
                {
                    "t": int(t),
                    "title": a.get_text(strip=True),
                    "url": absolute,
                    "has_attachments": _row_has_attachment(row),
                }
            )
        return out

    # Legacy table fallback: each topic is a <tr> containing <a class="topictitle">
    seen: set[int] = set()
    for a in soup.find_all("a", class_="topictitle", href=True):
        absolute = urljoin(page_url, a["href"])
        t_str = parse_qs(urlparse(absolute).query).get("t", [None])[0]
        if not t_str:
            continue
        t_id = int(t_str)
        if t_id in seen:
            continue
        seen.add(t_id)
        # Walk up to the enclosing <tr> (or table row container) to look for
        # the paperclip image marker.
        row = a.find_parent("tr") or a.parent
        has_attach = _row_has_attachment(row) if row else False
        out.append(
            {
                "t": t_id,
                "title": a.get_text(strip=True),
                "url": absolute,
                "has_attachments": has_attach,
            }
        )
    return out


_SIZE_RE = re.compile(r"\((\d+(?:\.\d+)?)\s*(B|KiB|MiB|GiB|KB|MB|GB)\)|\[(\d+(?:\.\d+)?)\s*(B|KiB|MiB|GiB|KB|MB|GB)\]")
_SIZE_MUL = {
    "B": 1, "KiB": 1024, "MiB": 1024**2, "GiB": 1024**3,
    "KB": 1000, "MB": 1000**2, "GB": 1000**3,
}


def _parse_size(text: str) -> Optional[int]:
    m = _SIZE_RE.search(text)
    if not m:
        return None
    n_str = m.group(1) or m.group(3)
    unit = m.group(2) or m.group(4)
    return int(float(n_str) * _SIZE_MUL[unit])


def parse_topic_page(html: str, page_url: str) -> tuple[list[dict], Optional[int]]:
    """From a viewtopic.php page, return (attachments, next_start).

    Scans the entire page for download/file.php anchors rather than scoping to
    post containers — the legacy fisubsilver2 template places attachment links
    in a sibling cell of the post body, so scoping to .postbody misses them.
    Filters out avatar references."""
    soup = BeautifulSoup(html, "html.parser")
    attachments: list[dict] = []
    seen_aids: set[int] = set()

    for a in soup.find_all("a", href=True):
        href = a["href"]
        if "download/file.php" not in href:
            continue
        href_abs = urljoin(page_url, href)
        qs = parse_qs(urlparse(href_abs).query)
        if "avatar" in qs:
            continue
        aid_str = qs.get("id", [None])[0]
        if not aid_str:
            continue
        try:
            aid = int(aid_str)
        except ValueError:
            continue
        if aid in seen_aids:
            continue
        seen_aids.add(aid)

        filename = a.get_text(strip=True) or None

        # Attribute to enclosing post: walk up for an id="pNNN" ancestor.
        post_id = None
        post_ctx = a.find_parent(id=re.compile(r"^p\d+$"))
        if post_ctx:
            try:
                post_id = int(post_ctx["id"][1:])
            except (KeyError, ValueError):
                pass

        # Poster: nearest username anchor in the same row/cell or post container
        poster = None
        ctx = post_ctx or a.find_parent("tr") or a.parent
        if ctx is not None:
            u = ctx.find(class_=re.compile(r"username"))
            if u:
                poster = u.get_text(strip=True)

        # Posted-at: <time datetime="..."> in the post context if present
        posted_at = None
        if ctx is not None:
            tm = ctx.find("time")
            if tm and tm.has_attr("datetime"):
                posted_at = tm["datetime"]

        # Filesize: search the immediate text neighborhood. fisubsilver2 uses
        # `<a href="...">file.zip</a> [361.89 KiB]` and a separate sibling
        # `<span>Downloaded N times</span>`.
        byte_size = None
        comment = None
        parent = a.parent
        if parent is not None:
            txt = parent.get_text(" ", strip=True)
            byte_size = _parse_size(txt)
            comment = txt[:500] or None

        attachments.append(
            {
                "aid": aid,
                "post_id": post_id,
                "filename": filename,
                "byte_size": byte_size,
                "posted_at": posted_at,
                "poster": poster,
                "comment": comment,
            }
        )

    # Pagination — find next page start value
    next_start = None
    for a in soup.find_all("a", href=True):
        href = a["href"]
        if "viewtopic.php" not in href or "start=" not in href:
            continue
        txt = a.get_text(strip=True).lower()
        if txt in ("next", ">", "next »"):
            qs = parse_qs(urlparse(urljoin(page_url, href)).query)
            try:
                next_start = int(qs.get("start", [None])[0])
            except (TypeError, ValueError):
                pass
            break
    return attachments, next_start


def cmd_index(args: argparse.Namespace) -> int:
    conn = db_open()
    s = build_session(Path(args.cookies))

    user = check_login(s)
    print(f"Logged in as: {user or '(anonymous)'}")

    if args.f:
        enabled = [
            dict(r)
            for r in conn.execute(
                "SELECT * FROM subforums WHERE f IN ({})".format(
                    ",".join("?" for _ in args.f)
                ),
                args.f,
            )
        ]
    else:
        enabled = [
            dict(r)
            for r in conn.execute(
                "SELECT * FROM subforums WHERE enabled=1 ORDER BY f"
            )
        ]
    if not enabled:
        print("No subforums to index. Run `discover` and `enable` first.", file=sys.stderr)
        return 2

    print(f"Indexing {len(enabled)} subforum(s):")
    for sf in enabled:
        print(f"  f={sf['f']} {sf['title']}")

    for sf in enabled:
        index_subforum(conn, s, sf, args)

    return 0


def index_subforum(
    conn: sqlite3.Connection,
    s: requests.Session,
    sf: dict,
    args: argparse.Namespace,
) -> None:
    print(f"\n=== Indexing f={sf['f']} {sf['title']} ===")
    start = 0
    new_topics = 0
    pages_walked = 0
    while True:
        if args.max_pages and pages_walked >= args.max_pages:
            print(f"  Reached --max-pages={args.max_pages}, stopping.")
            break
        url = urljoin(FORUM_BASE, f"viewforum.php?f={sf['f']}&start={start}")
        print(f"  GET listing start={start}")
        sleep_with_jitter(args.delay)
        try:
            html, resp = fetch_html(s, url)
        except RuntimeError as e:
            print(f"    {e}", file=sys.stderr)
            break

        topics = parse_topic_listing(html, resp.url)
        if not topics:
            print("    no topics found, done.")
            break
        with conn:
            for tp in topics:
                conn.execute(
                    """
                    INSERT INTO topics(t, f, title, url, has_attachments)
                    VALUES(?,?,?,?,?)
                    ON CONFLICT(t) DO UPDATE SET
                        title=excluded.title,
                        url=excluded.url,
                        has_attachments=
                            MAX(topics.has_attachments, excluded.has_attachments)
                    """,
                    (
                        tp["t"],
                        sf["f"],
                        tp["title"],
                        tp["url"],
                        1 if tp["has_attachments"] else 0,
                    ),
                )
                new_topics += 1
        attach_topics = sum(1 for t in topics if t["has_attachments"])
        print(f"    {len(topics)} topics ({attach_topics} with attachment icon)")

        # phpBB3 pagination: many templates return ~38 topics/page (not 50).
        # Detect end via empty page or by parsing the "Next" link if present.
        if not topics:
            break
        # Probe for a "Next" pagination link; if absent, we're on the last page.
        next_start = None
        for a in BeautifulSoup(html, "html.parser").find_all("a", href=True):
            href = a["href"]
            if "viewforum.php" not in href or "start=" not in href:
                continue
            txt = a.get_text(strip=True).lower()
            if txt in ("next", ">", "next »"):
                qs = parse_qs(urlparse(urljoin(resp.url, href)).query)
                try:
                    next_start = int(qs.get("start", [None])[0])
                except (TypeError, ValueError):
                    pass
                break
        if next_start is None or next_start <= start:
            break
        start = next_start
        pages_walked += 1

    with conn:
        conn.execute(
            "UPDATE subforums SET last_indexed_at=? WHERE f=?",
            (now_iso(), sf["f"]),
        )

    # Now walk topic pages to harvest attachment metadata. Only topics flagged
    # has_attachments=1 — skip discussion-only threads to save requests.
    candidates = [
        dict(r)
        for r in conn.execute(
            """
            SELECT * FROM topics
            WHERE f=? AND has_attachments=1
              AND (pages_indexed=0 OR ?)
            ORDER BY t DESC
            """,
            (sf["f"], 1 if args.reindex else 0),
        )
    ]
    print(f"\n  Topics with attachments to index: {len(candidates)}")
    if args.max_topics:
        candidates = candidates[: args.max_topics]
        print(f"  Limited to {len(candidates)} by --max-topics")

    for i, tp in enumerate(candidates, 1):
        index_topic(conn, s, tp, args, i, len(candidates))


def index_topic(
    conn: sqlite3.Connection,
    s: requests.Session,
    tp: dict,
    args: argparse.Namespace,
    i: int,
    total: int,
) -> None:
    start = 0
    pages = 0
    total_attachments = 0
    while True:
        url = urljoin(FORUM_BASE, f"viewtopic.php?f={tp['f']}&t={tp['t']}&start={start}")
        print(f"  [{i}/{total}] t={tp['t']} start={start}  {tp['title'][:60]}")
        sleep_with_jitter(args.delay)
        try:
            html, resp = fetch_html(s, url)
        except RuntimeError as e:
            print(f"    {e}", file=sys.stderr)
            break
        attachments, next_start = parse_topic_page(html, resp.url)
        with conn:
            for at in attachments:
                conn.execute(
                    """
                    INSERT INTO attachments(
                        aid, t, post_id, filename, byte_size,
                        posted_at, poster, comment, discovered_at)
                    VALUES(?,?,?,?,?,?,?,?,?)
                    ON CONFLICT(aid) DO UPDATE SET
                        filename=COALESCE(excluded.filename, attachments.filename),
                        byte_size=COALESCE(excluded.byte_size, attachments.byte_size),
                        poster=COALESCE(excluded.poster, attachments.poster),
                        posted_at=COALESCE(excluded.posted_at, attachments.posted_at),
                        comment=COALESCE(excluded.comment, attachments.comment)
                    """,
                    (
                        at["aid"],
                        tp["t"],
                        at["post_id"],
                        at["filename"],
                        at["byte_size"],
                        at["posted_at"],
                        at["poster"],
                        at["comment"],
                        now_iso(),
                    ),
                )
            total_attachments += len(attachments)
        pages += 1
        if next_start is None or next_start <= start:
            break
        start = next_start
    with conn:
        conn.execute(
            "UPDATE topics SET pages_indexed=?, last_indexed_at=? WHERE t=?",
            (pages, now_iso(), tp["t"]),
        )
    print(f"      {total_attachments} attachment(s)")


# --------------------------------------------------------------------------- #
# status / list
# --------------------------------------------------------------------------- #


def cmd_status(args: argparse.Namespace) -> int:
    conn = db_open()
    print("Subforums:")
    rows = conn.execute(
        """
        SELECT s.f, s.title, s.enabled,
               (SELECT COUNT(*) FROM topics WHERE f=s.f) AS topics,
               (SELECT COUNT(*) FROM topics WHERE f=s.f AND has_attachments=1) AS attach_topics,
               (SELECT COUNT(*) FROM attachments a JOIN topics t ON a.t=t.t WHERE t.f=s.f) AS attachments,
               s.last_indexed_at
        FROM subforums s ORDER BY s.enabled DESC, s.f
        """
    ).fetchall()
    print(f"{'f':>5} {'EN':>3} {'topics':>7} {'+att':>6} {'atts':>6}  title")
    for r in rows:
        print(
            f"{r['f']:>5} {'Y' if r['enabled'] else '.':>3} "
            f"{r['topics']:>7} {r['attach_topics']:>6} {r['attachments']:>6}  "
            f"{r['title']}"
        )
    print()
    totals = conn.execute(
        """
        SELECT
          (SELECT COUNT(*) FROM attachments) AS total_attachments,
          (SELECT COUNT(*) FROM attachments WHERE aid IN
              (SELECT aid FROM attempts WHERE classification IN ('binary','image','text'))) AS downloaded,
          (SELECT COUNT(*) FROM attempts) AS attempts
        """
    ).fetchone()
    print(
        f"Total attachments known: {totals['total_attachments']}  "
        f"downloaded ok: {totals['downloaded']}  "
        f"attempts logged: {totals['attempts']}"
    )
    return 0


def cmd_list_attachments(args: argparse.Namespace) -> int:
    conn = db_open()
    q = """
        SELECT a.aid, a.filename, a.byte_size, a.poster, a.posted_at,
               t.t, t.title AS topic_title, t.f
        FROM attachments a JOIN topics t ON a.t=t.t
    """
    where = []
    params: list = []
    if args.f:
        where.append("t.f IN ({})".format(",".join("?" for _ in args.f)))
        params.extend(args.f)
    if args.ext:
        # match by filename suffix
        ors = []
        for ext in args.ext:
            ors.append("LOWER(a.filename) LIKE ?")
            params.append(f"%.{ext.lower().lstrip('.')}")
        where.append("(" + " OR ".join(ors) + ")")
    if args.grep:
        where.append("(LOWER(a.filename) LIKE ? OR LOWER(t.title) LIKE ?)")
        params.extend([f"%{args.grep.lower()}%", f"%{args.grep.lower()}%"])
    if where:
        q += " WHERE " + " AND ".join(where)
    q += " ORDER BY a.aid DESC"
    if args.limit:
        q += f" LIMIT {int(args.limit)}"
    rows = conn.execute(q, params).fetchall()
    print(f"{'aid':>7}  {'size':>10}  f   t       file/topic")
    for r in rows:
        sz = "" if r["byte_size"] is None else f"{r['byte_size']:>10,}"
        fn = r["filename"] or "(no name)"
        print(f"{r['aid']:>7}  {sz:>10}  {r['f']:>2}  {r['t']:>6}  {fn}  | {r['topic_title']}")
    print(f"\n{len(rows)} attachment(s).")
    return 0


# --------------------------------------------------------------------------- #
# download
# --------------------------------------------------------------------------- #


def append_attempt(record: dict) -> None:
    ATTEMPTS_JSONL.parent.mkdir(parents=True, exist_ok=True)
    with ATTEMPTS_JSONL.open("a", encoding="utf-8") as fh:
        fh.write(json.dumps(record) + "\n")


def safe_filename(name: str) -> str:
    # Strip directories, control chars, and path separators.
    name = name.replace("\\", "_").replace("/", "_")
    name = re.sub(r"[\x00-\x1f]", "", name)
    return name[:200] if name else "attachment"


def already_downloaded(conn: sqlite3.Connection, aid: int) -> Optional[str]:
    row = conn.execute(
        """
        SELECT saved_path FROM attempts
        WHERE aid=? AND classification IN ('binary','image','text')
              AND saved_path IS NOT NULL
        ORDER BY rowid DESC LIMIT 1
        """,
        (aid,),
    ).fetchone()
    return row["saved_path"] if row else None


def cmd_download(args: argparse.Namespace) -> int:
    conn = db_open()
    s = build_session(Path(args.cookies))

    user = check_login(s)
    print(f"Logged in as: {user or '(anonymous)'}")

    where = []
    params: list = []
    if args.f:
        where.append("t.f IN ({})".format(",".join("?" for _ in args.f)))
        params.extend(args.f)
    if args.ext:
        ors = []
        for ext in args.ext:
            ors.append("LOWER(a.filename) LIKE ?")
            params.append(f"%.{ext.lower().lstrip('.')}")
        where.append("(" + " OR ".join(ors) + ")")
    if args.grep:
        where.append("(LOWER(a.filename) LIKE ? OR LOWER(t.title) LIKE ?)")
        params.extend([f"%{args.grep.lower()}%", f"%{args.grep.lower()}%"])
    if args.aid:
        where.append("a.aid IN ({})".format(",".join("?" for _ in args.aid)))
        params.extend(args.aid)

    q = """
        SELECT a.aid, a.filename, a.byte_size, t.f, t.t, t.title AS topic_title
        FROM attachments a JOIN topics t ON a.t=t.t
    """
    if where:
        q += " WHERE " + " AND ".join(where)
    q += " ORDER BY a.aid DESC"
    rows = [dict(r) for r in conn.execute(q, params)]
    if args.limit:
        rows = rows[: args.limit]

    if not rows:
        print("No attachments match the filter.")
        return 0

    DEFAULT_OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Will attempt {len(rows)} attachment(s). Saving under {DEFAULT_OUTPUT_DIR}")

    downloaded = 0
    skipped = 0
    failed = 0
    for i, r in enumerate(rows, 1):
        aid = r["aid"]
        if not args.force:
            existing = already_downloaded(conn, aid)
            if existing and Path(existing).exists():
                skipped += 1
                if args.verbose:
                    print(f"  [{i}/{len(rows)}] aid={aid} already at {existing}, skip")
                continue
        ok = download_one(conn, s, r, args)
        if ok:
            downloaded += 1
        else:
            failed += 1
    print(
        f"\nDone. downloaded={downloaded} skipped={skipped} failed={failed} "
        f"(total considered {len(rows)})"
    )
    return 0 if failed == 0 else 1


def download_one(
    conn: sqlite3.Connection,
    s: requests.Session,
    row: dict,
    args: argparse.Namespace,
) -> bool:
    aid = row["aid"]
    url = urljoin(FORUM_BASE, f"download/file.php?id={aid}")
    sleep_with_jitter(args.delay)
    ts = now_iso()
    try:
        resp = s.get(url, timeout=120, allow_redirects=True, stream=False)
    except Exception as e:
        rec = {
            "aid": aid,
            "ts": ts,
            "http_status": None,
            "classification": "exception",
            "saved_path": None,
            "bytes": None,
            "error": str(e),
        }
        with conn:
            conn.execute(
                """
                INSERT INTO attempts(aid, ts, http_status, classification,
                    saved_path, bytes, error) VALUES(?,?,?,?,?,?,?)
                """,
                (aid, ts, None, "exception", None, None, str(e)),
            )
        append_attempt(rec)
        print(f"  aid={aid} EXCEPTION: {e}")
        return False

    cls = classify_attachment(resp)
    saved_path: Optional[str] = None
    nbytes: Optional[int] = None
    err: Optional[str] = None
    if cls in ("binary", "image", "text"):
        # Determine filename
        cd = resp.headers.get("content-disposition") or ""
        fn_match = re.search(r'filename\*?=(?:UTF-8\'\')?"?([^"]+?)"?(?:;|$)', cd)
        fname = (
            fn_match.group(1) if fn_match else row.get("filename") or f"aid_{aid}.bin"
        )
        # phpBB3 sometimes URL-encodes the filename
        from urllib.parse import unquote
        fname = unquote(fname)
        fname = safe_filename(fname)
        out_path = DEFAULT_OUTPUT_DIR / f"{aid:07d}_{fname}"
        if out_path.exists() and not args.force:
            # Don't overwrite; suffix.
            stem, suffix = out_path.stem, out_path.suffix
            n = 1
            while True:
                cand = DEFAULT_OUTPUT_DIR / f"{stem}.{n}{suffix}"
                if not cand.exists():
                    out_path = cand
                    break
                n += 1
        try:
            out_path.write_bytes(resp.content)
            saved_path = str(out_path)
            nbytes = len(resp.content)
            # Auto-sort into archives/binaries/images/... by extension, and
            # rename to <ECU_ID>_aid<N>.<ext> when we can identify the ECU.
            if _ORGANIZE_OK:
                try:
                    file_head = resp.content[:65536]
                    tp = target_path(
                        DEFAULT_OUTPUT_DIR, aid, fname,
                        file_data=file_head, src_path=out_path,
                    )
                    final = place_file(DEFAULT_OUTPUT_DIR, out_path, tp)
                    saved_path = str(final)
                except Exception as e:
                    print(f"  (warn: organize failed for aid={aid}: {e})",
                          file=sys.stderr)
        except OSError as e:
            cls = "write_error"
            err = str(e)
    else:
        err = f"status={resp.status_code} ct={resp.headers.get('content-type')}"

    rec = {
        "aid": aid,
        "ts": ts,
        "http_status": resp.status_code,
        "classification": cls,
        "saved_path": saved_path,
        "bytes": nbytes,
        "error": err,
        "topic_t": row["t"],
        "subforum_f": row["f"],
    }
    with conn:
        conn.execute(
            """
            INSERT INTO attempts(aid, ts, http_status, classification,
                saved_path, bytes, error) VALUES(?,?,?,?,?,?,?)
            """,
            (aid, ts, resp.status_code, cls, saved_path, nbytes, err),
        )
    append_attempt(rec)
    print(
        f"  aid={aid:>7} {cls:>10} status={resp.status_code:<3} "
        f"{(nbytes or 0):>10} bytes  {saved_path or err or ''}"
    )
    return cls in ("binary", "image", "text")


# --------------------------------------------------------------------------- #
# CLI wiring
# --------------------------------------------------------------------------- #


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    p.add_argument(
        "--cookies",
        default=str(DEFAULT_COOKIES),
        help=f"Path to Netscape cookies.txt (default: {DEFAULT_COOKIES})",
    )
    p.add_argument(
        "--delay",
        type=float,
        default=DEFAULT_DELAY_SECONDS,
        help=f"Base seconds between requests (default {DEFAULT_DELAY_SECONDS}, floor {MIN_DELAY_SECONDS}).",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("discover", help="Find and persist the forum list")
    sp.add_argument("--allow-anonymous", action="store_true")
    sp.set_defaults(func=cmd_discover)

    sp = sub.add_parser("enable", help="Mark a subforum as in-scope for indexing")
    sp.add_argument("f", type=int, nargs="+")
    sp.set_defaults(func=cmd_enable, _action="enable")

    sp = sub.add_parser("disable", help="Mark a subforum as out-of-scope")
    sp.add_argument("f", type=int, nargs="+")
    sp.set_defaults(func=cmd_enable, _action="disable")

    sp = sub.add_parser("index", help="Walk enabled subforums and harvest attachments")
    sp.add_argument("--f", type=int, nargs="*", help="Override: index only these f IDs")
    sp.add_argument("--max-pages", type=int, default=0, help="Per-subforum listing-page cap")
    sp.add_argument("--max-topics", type=int, default=0, help="Per-subforum topic cap")
    sp.add_argument("--reindex", action="store_true", help="Re-index already-indexed topics")
    sp.set_defaults(func=cmd_index)

    sp = sub.add_parser("status", help="Show what we've discovered/indexed")
    sp.set_defaults(func=cmd_status)

    sp = sub.add_parser("list-attachments", help="Filter and list known attachments")
    sp.add_argument("--f", type=int, nargs="*", help="Filter by subforum")
    sp.add_argument("--ext", type=str, nargs="*", help="Filter by file extension")
    sp.add_argument("--grep", type=str, help="Substring match on filename or topic title")
    sp.add_argument("--limit", type=int, default=200)
    sp.set_defaults(func=cmd_list_attachments)

    sp = sub.add_parser("download", help="Fetch attachments matching a filter")
    sp.add_argument("--f", type=int, nargs="*")
    sp.add_argument("--ext", type=str, nargs="*")
    sp.add_argument("--grep", type=str)
    sp.add_argument("--aid", type=int, nargs="*", help="Download specific attachment IDs")
    sp.add_argument("--limit", type=int, default=0)
    sp.add_argument("--force", action="store_true", help="Re-download even if already saved")
    sp.add_argument("--verbose", action="store_true")
    sp.set_defaults(func=cmd_download)

    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
