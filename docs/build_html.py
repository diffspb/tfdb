#!/usr/bin/env python3
"""Build and verify the committed offline HTML mirror of TFDB documentation."""

from __future__ import annotations

import argparse
import hashlib
import html
import os
import re
import sys
import unicodedata
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote, urlsplit

try:
    import markdown_it
    from markdown_it import MarkdownIt
except ImportError:
    sys.stderr.write(
        "docs/build_html.py requires markdown-it-py "
        "(Debian/Ubuntu package: python3-markdown-it)\n"
    )
    raise SystemExit(2)


EXPECTED_MARKDOWN_IT_VERSION = "3.0.0"
DOCS_DIR = Path(__file__).resolve().parent
ROOT = DOCS_DIR.parent
OUTPUT_DIR = DOCS_DIR / "html"

PAGES = (
    (ROOT / "README.md", "index.html", "Overview"),
    (DOCS_DIR / "tutorial.md", "tutorial.html", "Tutorial"),
    (DOCS_DIR / "api.md", "api.html", "C++ API"),
    (DOCS_DIR / "architecture.md", "architecture.html", "Architecture"),
    (DOCS_DIR / "format-v1.md", "format-v1.html", "Media format v1"),
    (DOCS_DIR / "decisions.md", "decisions.html", "Decisions"),
    (DOCS_DIR / "research.md", "research.html", "Research"),
    (DOCS_DIR / "testing.md", "testing.html", "Verification"),
    (DOCS_DIR / "sizing.md", "sizing.html", "Sizing and endurance"),
    (DOCS_DIR / "evidence.md", "evidence.html", "Evidence"),
    (DOCS_DIR / "logs.md", "logs.html", "Application logs"),
)

STYLE = r"""/* Generated documentation asset; source: docs/build_html.py. */
:root {
  color-scheme: light dark;
  --bg: #f7f8fa;
  --panel: #ffffff;
  --text: #172033;
  --muted: #5c667a;
  --line: #d9dee8;
  --accent: #1769aa;
  --accent-soft: #e7f2fb;
  --code: #eef1f5;
  --shadow: 0 1px 3px rgb(19 33 68 / 9%);
}

@media (prefers-color-scheme: dark) {
  :root {
    --bg: #10141d;
    --panel: #181e29;
    --text: #e8edf5;
    --muted: #aab3c2;
    --line: #30394a;
    --accent: #78bdf2;
    --accent-soft: #172d40;
    --code: #111722;
    --shadow: none;
  }
}

* { box-sizing: border-box; }
html { scroll-behavior: smooth; }
@media (prefers-reduced-motion: reduce) {
  html { scroll-behavior: auto; }
}
body {
  margin: 0;
  background: var(--bg);
  color: var(--text);
  font: 16px/1.62 system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
}
a { color: var(--accent); text-underline-offset: 0.16em; }
a:hover { text-decoration-thickness: 2px; }
.skip-link {
  position: fixed;
  left: 1rem;
  top: -5rem;
  z-index: 10;
  padding: .55rem .8rem;
  background: var(--panel);
  border: 1px solid var(--line);
}
.skip-link:focus { top: 1rem; }
.site-header {
  position: sticky;
  top: 0;
  z-index: 5;
  display: flex;
  align-items: baseline;
  justify-content: space-between;
  gap: 1rem;
  min-height: 3.7rem;
  padding: .8rem clamp(1rem, 3vw, 2.5rem);
  border-bottom: 1px solid var(--line);
  background: var(--panel);
  background: color-mix(in srgb, var(--panel) 94%, transparent);
  backdrop-filter: blur(8px);
}
.site-title { color: var(--text); font-size: 1.15rem; font-weight: 750; text-decoration: none; }
.site-status { color: var(--muted); font-size: .85rem; }
.layout {
  display: grid;
  grid-template-columns: minmax(12rem, 17rem) minmax(0, 58rem);
  justify-content: center;
  gap: clamp(1.5rem, 4vw, 4rem);
  padding: 2rem clamp(1rem, 3vw, 2.5rem) 4rem;
}
.sidebar { align-self: start; position: sticky; top: 5.5rem; }
.sidebar-title {
  margin: 0 0 .65rem;
  color: var(--muted);
  font-size: .75rem;
  letter-spacing: .09em;
  text-transform: uppercase;
}
.sidebar ul { margin: 0; padding: 0; list-style: none; }
.sidebar a {
  display: block;
  margin: .12rem 0;
  padding: .42rem .62rem;
  border-radius: .35rem;
  color: var(--muted);
  text-decoration: none;
}
.sidebar a:hover { color: var(--text); background: var(--accent-soft); }
.sidebar a[aria-current="page"] { color: var(--accent); background: var(--accent-soft); font-weight: 700; }
main {
  min-width: 0;
  padding: clamp(1.25rem, 4vw, 3rem);
  border: 1px solid var(--line);
  border-radius: .65rem;
  background: var(--panel);
  box-shadow: var(--shadow);
}
.mirror-note {
  margin: 0 0 2rem;
  padding: .65rem .8rem;
  border-left: .25rem solid var(--accent);
  background: var(--accent-soft);
  color: var(--muted);
  font-size: .9rem;
}
h1, h2, h3, h4 { line-height: 1.22; scroll-margin-top: 5rem; }
h1 { margin-top: 0; font-size: clamp(2rem, 5vw, 3rem); letter-spacing: -.035em; }
h2 { margin-top: 2.4rem; padding-top: .4rem; border-top: 1px solid var(--line); font-size: 1.55rem; }
h3 { margin-top: 1.8rem; font-size: 1.2rem; }
p, ul, ol, blockquote { max-width: 78ch; }
li + li { margin-top: .24rem; }
code {
  padding: .12em .3em;
  border-radius: .25rem;
  background: var(--code);
  font: .9em/1.45 ui-monospace, SFMono-Regular, Consolas, monospace;
}
pre {
  max-width: 100%;
  overflow: auto;
  padding: 1rem;
  border: 1px solid var(--line);
  border-radius: .45rem;
  background: var(--code);
}
pre code { padding: 0; background: transparent; font-size: .88rem; }
.table-wrap { max-width: 100%; overflow-x: auto; margin: 1.25rem 0; }
table { width: 100%; border-collapse: collapse; font-size: .91rem; }
th, td { padding: .52rem .65rem; border: 1px solid var(--line); text-align: left; vertical-align: top; }
th { background: var(--code); }
blockquote { margin-left: 0; padding-left: 1rem; border-left: .25rem solid var(--line); color: var(--muted); }
.page-footer { margin-top: 3rem; padding-top: 1rem; border-top: 1px solid var(--line); color: var(--muted); font-size: .82rem; }
.source-hash { font-family: ui-monospace, SFMono-Regular, Consolas, monospace; overflow-wrap: anywhere; }

@media (max-width: 780px) {
  .site-header { position: static; }
  .layout { display: block; padding: 1rem; }
  .sidebar { position: static; margin-bottom: 1rem; }
  .sidebar ul { display: flex; gap: .25rem; overflow-x: auto; padding-bottom: .4rem; }
  .sidebar li { flex: 0 0 auto; }
  main { padding: 1.2rem; }
}

@media print {
  :root { color-scheme: light; --bg: white; --panel: white; --text: black; --muted: #444; --line: #bbb; --code: #f3f3f3; }
  .site-header, .sidebar, .skip-link, .mirror-note { display: none; }
  .layout { display: block; padding: 0; }
  main { border: 0; box-shadow: none; padding: 0; }
  a { color: inherit; }
  pre, table { break-inside: avoid; }
}
"""


def slugify(value: str) -> str:
    normalized = unicodedata.normalize("NFKD", value).encode("ascii", "ignore").decode()
    slug = re.sub(r"[^a-zA-Z0-9]+", "-", normalized).strip("-").lower()
    return slug or "section"


def rewrite_href(href: str, source_path: Path) -> str:
    if href.startswith(("https://", "http://", "mailto:")) or href.startswith("#"):
        return href
    path, marker, fragment = href.partition("#")
    target = (source_path.parent / unquote(path)).resolve()
    mapped = next(
        (filename for source, filename, _ in PAGES if source.resolve() == target),
        None,
    )
    rewritten = mapped or os.path.relpath(target, OUTPUT_DIR).replace(os.sep, "/")
    return rewritten + (marker + fragment if marker else "")


def walk_tokens(tokens):
    for token in tokens:
        yield token
        if token.children:
            yield from walk_tokens(token.children)


def render_markdown(source: str, source_path: Path) -> tuple[str, str]:
    parser = MarkdownIt("commonmark", {"html": False}).enable("table")
    tokens = parser.parse(source)
    seen: dict[str, int] = {}
    title = "TFDB documentation"
    for index, token in enumerate(tokens):
        if token.type != "heading_open" or index + 1 >= len(tokens):
            continue
        heading = tokens[index + 1].content
        if token.tag == "h1":
            title = heading
        base = slugify(heading)
        duplicate = seen.get(base, 0)
        seen[base] = duplicate + 1
        token.attrSet("id", base if duplicate == 0 else f"{base}-{duplicate + 1}")
    for token in walk_tokens(tokens):
        if token.type == "link_open":
            href = token.attrGet("href")
            if href:
                token.attrSet("href", rewrite_href(href, source_path))
                if href.startswith(("https://", "http://")):
                    token.attrSet("rel", "noopener noreferrer")
        if token.type == "table_open":
            token.attrSet("class", "data-table")
        if token.type == "th_open":
            token.attrSet("scope", "col")
    body = parser.renderer.render(tokens, parser.options, {})
    body = body.replace('<table class="data-table">', '<div class="table-wrap"><table class="data-table">')
    body = body.replace("</table>", "</table></div>")
    return title, body


def navigation(active: str) -> str:
    items = []
    for _, filename, label in PAGES:
        current = ' aria-current="page"' if filename == active else ""
        items.append(
            f'<li><a href="{html.escape(filename)}"{current}>{html.escape(label)}</a></li>'
        )
    return "\n".join(items)


def page_html(source_path: Path, filename: str, label: str) -> str:
    source = source_path.read_text(encoding="utf-8")
    title, body = render_markdown(source, source_path)
    digest = hashlib.sha256(source.encode("utf-8")).hexdigest()
    source_link = "../../README.md" if source_path == ROOT / "README.md" else f"../{source_path.name}"
    return f"""<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta name="tfdb-source-sha256" content="{digest}">
  <title>{html.escape(title)} — TFDB</title>
  <link rel="stylesheet" href="style.css">
</head>
<body>
  <a class="skip-link" href="#content">Skip to content</a>
  <header class="site-header">
    <a class="site-title" href="index.html">TFDB documentation</a>
    <span class="site-status">C++14 software candidate</span>
  </header>
  <div class="layout">
    <nav class="sidebar" aria-label="Documentation">
      <p class="sidebar-title">Contents</p>
      <ul>
        {navigation(filename)}
      </ul>
    </nav>
    <main id="content">
      <p class="mirror-note">Offline HTML mirror of <a href="{html.escape(source_link)}">{html.escape(source_path.relative_to(ROOT).as_posix())}</a>. The Markdown file is canonical.</p>
      {body}
      <footer class="page-footer">
        <p>Generated deterministically by <code>python3 docs/build_html.py</code>.</p>
        <p>Source SHA-256: <span class="source-hash">{digest}</span></p>
      </footer>
    </main>
  </div>
</body>
</html>
"""


class LinkParser(HTMLParser):
    def __init__(self) -> None:
        super().__init__()
        self.links: list[str] = []
        self.ids: set[str] = set()
        self.main_count = 0
        self.nav_count = 0
        self.h1_count = 0

    def handle_starttag(self, tag: str, attrs) -> None:
        values = dict(attrs)
        if "id" in values:
            self.ids.add(values["id"])
        if tag == "a" and "href" in values:
            self.links.append(values["href"])
        if tag == "main":
            self.main_count += 1
        elif tag == "nav":
            self.nav_count += 1
        elif tag == "h1":
            self.h1_count += 1


def verify_links(artifacts: dict[str, str]) -> list[str]:
    errors: list[str] = []
    parsed: dict[str, LinkParser] = {}
    for name, content in artifacts.items():
        if not name.endswith(".html"):
            continue
        parser = LinkParser()
        parser.feed(content)
        parsed[name] = parser
        if parser.main_count != 1 or parser.nav_count != 1 or parser.h1_count != 1:
            errors.append(
                f"{name}: expected one main/nav/h1, got "
                f"{parser.main_count}/{parser.nav_count}/{parser.h1_count}"
            )
    for name, parser in parsed.items():
        for href in parser.links:
            split = urlsplit(href)
            if split.scheme or split.netloc or href.startswith("mailto:"):
                continue
            target_path = unquote(split.path)
            if not target_path:
                target_name = name
                exists = True
            else:
                target = (OUTPUT_DIR / name).parent / target_path
                exists = target.exists() or target.name in artifacts
                target_name = target.name
            if not exists:
                errors.append(f"{name}: broken local link {href}")
                continue
            if split.fragment and target_name.endswith(".html"):
                target_parser = parsed.get(target_name)
                if target_parser and split.fragment not in target_parser.ids:
                    errors.append(f"{name}: missing anchor {href}")
    return errors


def expected_artifacts() -> dict[str, str]:
    artifacts = {"style.css": STYLE}
    for source, filename, label in PAGES:
        artifacts[filename] = page_html(source, filename, label)
    return artifacts


def dependency_errors() -> list[str]:
    actual = getattr(markdown_it, "__version__", "unknown")
    if actual == EXPECTED_MARKDOWN_IT_VERSION:
        return []
    return [
        "unsupported markdown-it-py version: "
        f"expected {EXPECTED_MARKDOWN_IT_VERSION}, found {actual}"
    ]


def source_inventory_errors() -> list[str]:
    declared = {source.resolve() for source, _, _ in PAGES}
    discovered = {ROOT / "README.md", *DOCS_DIR.glob("*.md")}
    discovered = {source.resolve() for source in discovered}
    errors = [
        f"documentation source has no HTML mapping: {source.relative_to(ROOT)}"
        for source in sorted(discovered - declared)
    ]
    errors.extend(
        f"mapped documentation source is missing: {source.relative_to(ROOT)}"
        for source in sorted(declared - discovered)
    )
    return errors


def unexpected_output_entries(artifacts: dict[str, str]) -> list[Path]:
    if not OUTPUT_DIR.exists():
        return []
    return sorted(path for path in OUTPUT_DIR.iterdir() if path.name not in artifacts)


def display_path(path: Path) -> str:
    try:
        return path.relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def main() -> int:
    argument_parser = argparse.ArgumentParser(description=__doc__)
    argument_parser.add_argument(
        "--check", action="store_true", help="verify committed HTML without rewriting it"
    )
    args = argument_parser.parse_args()
    preflight_failures = dependency_errors() + source_inventory_errors()
    if preflight_failures:
        for failure in preflight_failures:
            sys.stderr.write(f"HTML docs check: {failure}\n")
        return 1
    artifacts = expected_artifacts()

    if args.check:
        failures = []
        for name, expected in artifacts.items():
            path = OUTPUT_DIR / name
            if not path.exists():
                failures.append(f"missing generated file: {path.relative_to(ROOT)}")
            elif path.read_text(encoding="utf-8") != expected:
                failures.append(f"stale generated file: {path.relative_to(ROOT)}")
        unexpected = unexpected_output_entries(artifacts)
        failures.extend(
            f"unexpected generated entry: {display_path(path)}" for path in unexpected
        )
        failures.extend(verify_links(artifacts))
        if failures:
            for failure in failures:
                sys.stderr.write(f"HTML docs check: {failure}\n")
            return 1
        print(f"HTML docs check: {len(PAGES)} pages and local links are current")
        return 0

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    unexpected = unexpected_output_entries(artifacts)
    if unexpected:
        for path in unexpected:
            sys.stderr.write(
                f"HTML docs check: unexpected generated entry: {display_path(path)}\n"
            )
        return 1
    for name, content in artifacts.items():
        path = OUTPUT_DIR / name
        if not path.exists() or path.read_text(encoding="utf-8") != content:
            path.write_text(content, encoding="utf-8")
            print(f"generated {path.relative_to(ROOT)}")
    failures = verify_links(artifacts)
    if failures:
        for failure in failures:
            sys.stderr.write(f"HTML docs check: {failure}\n")
        return 1
    print(f"generated HTML mirror: {len(PAGES)} pages")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
