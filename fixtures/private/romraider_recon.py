"""
RomRaider forum recon probe.

Validates that cookies.txt still gets past Cloudflare against romraider.com,
identifies forum structure (subforum list), and looks for the phpBB3 attachment
URL pattern.

Stateless, read-only. One request at a time with a polite delay.
"""

from __future__ import annotations

import argparse
import http.cookiejar
import re
import sys
import time
from pathlib import Path
from urllib.parse import urljoin, urlparse, parse_qs

import requests
from bs4 import BeautifulSoup

BASE = "https://www.romraider.com/"
FORUM_CANDIDATES = [
    "forum/",
    "forum/index.php",
]

# Windows Chrome UA — most cf_clearance cookies are earned under this kind of
# browser fingerprint. Try Firefox as a fallback.
UA_CHROME = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"
)
UA_FIREFOX = (
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:133.0) "
    "Gecko/20100101 Firefox/133.0"
)

CF_TITLE_MARKERS = (
    "Just a moment",
    "Attention Required",
    "One more step",
    "Please Wait",
)


def load_cookies(path: Path) -> http.cookiejar.MozillaCookieJar:
    jar = http.cookiejar.MozillaCookieJar()
    jar.load(str(path), ignore_discard=True, ignore_expires=True)
    return jar


def _title(body: str) -> str | None:
    m = re.search(r"<title[^>]*>([^<]+)</title>", body, re.IGNORECASE)
    return m.group(1).strip() if m else None


def classify(resp: requests.Response) -> str:
    body = resp.text or ""
    if resp.status_code in (403, 503) and "cloudflare" in body.lower():
        return "cf_block"
    title = _title(body) or ""
    if any(marker.lower() in title.lower() for marker in CF_TITLE_MARKERS):
        return "cf_challenge"
    if resp.status_code != 200:
        return f"http_{resp.status_code}"
    if "<html" not in body.lower():
        return "non_html"
    return "html_ok"


def probe(session: requests.Session, url: str, ua_label: str) -> dict:
    t0 = time.time()
    resp = session.get(url, timeout=30, allow_redirects=True)
    dt = time.time() - t0
    cls = classify(resp)
    info = {
        "url": url,
        "ua": ua_label,
        "status": resp.status_code,
        "final_url": resp.url,
        "bytes": len(resp.content),
        "elapsed_s": round(dt, 2),
        "classification": cls,
    }
    if cls == "html_ok":
        soup = BeautifulSoup(resp.text, "html.parser")
        title = soup.title.string.strip() if soup.title and soup.title.string else None
        info["title"] = title
    return info, resp


def discover_forums(html: str, base_url: str) -> list[dict]:
    """Find forum/subforum links typical of phpBB3."""
    soup = BeautifulSoup(html, "html.parser")
    forums: list[dict] = []
    seen: set[str] = set()
    # phpBB3 forum links generally look like viewforum.php?f=<n>
    for a in soup.find_all("a", href=True):
        href = a["href"]
        if "viewforum.php" not in href:
            continue
        absolute = urljoin(base_url, href)
        parsed = urlparse(absolute)
        f_id = parse_qs(parsed.query).get("f", [None])[0]
        if not f_id:
            continue
        key = f"f={f_id}"
        if key in seen:
            continue
        seen.add(key)
        title = a.get_text(strip=True) or None
        forums.append({"f": f_id, "title": title, "url": absolute})
    return forums


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument(
        "--cookies",
        default="roms_extracted/forumdownloads/cookies.txt",
        help="Path to Netscape cookies.txt (relative to this script's directory).",
    )
    p.add_argument("--delay", type=float, default=3.0, help="Seconds between requests.")
    args = p.parse_args()

    here = Path(__file__).resolve().parent
    cookies_path = (here / args.cookies).resolve()
    if not cookies_path.exists():
        print(f"cookies file not found: {cookies_path}", file=sys.stderr)
        return 2

    jar = load_cookies(cookies_path)
    print(f"Loaded {len(jar)} cookies from {cookies_path}")
    for c in jar:
        print(f"  {c.domain}\t{c.name}\t(expires={c.expires})")

    for ua_label, ua in (("chrome", UA_CHROME), ("firefox", UA_FIREFOX)):
        print(f"\n=== Probing with User-Agent: {ua_label} ===")
        s = requests.Session()
        s.cookies = jar  # type: ignore[assignment]
        s.headers.update(
            {
                "User-Agent": ua,
                "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
                "Accept-Language": "en-US,en;q=0.5",
                "Accept-Encoding": "gzip, deflate",
                "DNT": "1",
                "Connection": "keep-alive",
                "Upgrade-Insecure-Requests": "1",
            }
        )

        # 1. Root
        info, resp = probe(s, BASE, ua_label)
        print(f"root: {info}")
        if info["classification"] in ("cf_block", "cf_challenge"):
            print("  Cloudflare blocked/challenged. Trying next UA.")
            time.sleep(args.delay)
            continue

        time.sleep(args.delay)

        # 2. Try forum locations
        forum_html = None
        forum_base_url = None
        for f in FORUM_CANDIDATES:
            url = urljoin(BASE, f)
            info, resp = probe(s, url, ua_label)
            print(f"{f}: status={info['status']} cls={info['classification']} title={info.get('title')!r}")
            if info["classification"] == "html_ok":
                forum_html = resp.text
                forum_base_url = resp.url
                break
            time.sleep(args.delay)

        if not forum_html:
            print("  Could not reach forum index. Trying next UA.")
            continue

        # 3. Discover subforums
        forums = discover_forums(forum_html, forum_base_url)
        print(f"\nDiscovered {len(forums)} subforum links:")
        for fm in forums:
            print(f"  f={fm['f']:>4}  {fm['title']}")

        # 4. Highlight subforums whose names hint at ROMs/ECU/calibrations
        keywords = ("rom", "ecu", "calib", "definition", "logger", "tune", "subaru")
        relevant = [
            fm for fm in forums
            if fm["title"] and any(k in fm["title"].lower() for k in keywords)
        ]
        if relevant:
            print(f"\nLikely relevant ({len(relevant)}):")
            for fm in relevant:
                print(f"  f={fm['f']:>4}  {fm['title']}")

        # Persist the discovery so we don't have to re-probe
        out = here / "romraider_recon_forums.txt"
        with out.open("w", encoding="utf-8") as fh:
            fh.write(f"# Source: {forum_base_url}\n")
            fh.write(f"# User-Agent: {ua_label}\n\n")
            for fm in forums:
                fh.write(f"f={fm['f']}\t{fm['title']}\t{fm['url']}\n")
        print(f"\nWrote {out}")

        return 0

    print("\nAll UAs failed. cookies may be expired or IP-bound.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
