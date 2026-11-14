#!/usr/bin/env python3
"""Repository documentation checks.

Runs in the `docs` CI job with no third-party dependencies. Verifies that:

* every long-lived document has an English and a Chinese counterpart;
* the GitHub Pages landing pages exist, contain no nested <a> elements, and only
  reference files that are actually published under docs/;
* relative Markdown links point at files that exist.

Exits non-zero on the first category of problems found, listing every issue.
"""

from __future__ import annotations

import re
import sys
from html.parser import HTMLParser
from pathlib import Path
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
DOCS = ROOT / "docs"

# Documents that must stay bilingual. The English file is canonical.
BILINGUAL = [
    "README.md",
    "CHANGELOG.md",
    "CONTRIBUTING.md",
    "CODE_OF_CONDUCT.md",
    "SECURITY.md",
    "THIRD_PARTY_NOTICES.md",
    "docs/architecture.md",
    "docs/PROTOCOL-NOTES.md",
    "docs/projection-backend.md",
]

SKIP_PREFIXES = ("http://", "https://", "mailto:", "//", "data:", "javascript:")


def local_target(base: Path, href: str) -> Path | None:
    href = href.strip()
    if not href or href.startswith("#") or href.lower().startswith(SKIP_PREFIXES):
        return None
    path = unquote(href.split("#", 1)[0].split("?", 1)[0])
    if not path:
        return None
    return (base / path).resolve()


class LandingPageParser(HTMLParser):
    def __init__(self, page: Path) -> None:
        super().__init__(convert_charrefs=True)
        self.page = page
        self.anchor_depth = 0
        self.nested_anchors: list[int] = []
        self.links: list[tuple[str, str]] = []
        self._line = 1

    def handle_starttag(self, tag: str, attrs: list[tuple[str, str | None]]) -> None:
        values = dict(attrs)
        if tag == "a":
            if self.anchor_depth:
                self.nested_anchors.append(self.getpos()[0])
            self.anchor_depth += 1
        for attr in ("href", "src"):
            value = values.get(attr)
            if value:
                self.links.append((attr, value))

    def handle_endtag(self, tag: str) -> None:
        if tag == "a" and self.anchor_depth:
            self.anchor_depth -= 1


def check_bilingual(errors: list[str]) -> None:
    for english in BILINGUAL:
        path = ROOT / english
        if not path.is_file():
            errors.append(f"missing document: {english}")
            continue
        chinese = path.with_name(path.stem + ".zh-CN.md")
        if not chinese.is_file():
            errors.append(f"missing Chinese counterpart: {chinese.relative_to(ROOT)}")
        switch = path.read_text(encoding="utf-8", errors="replace").splitlines()[:8]
        if not any("zh-CN" in line for line in switch):
            errors.append(f"{english}: no link to the Chinese version near the top")


def check_landing_pages(errors: list[str]) -> None:
    pages = [DOCS / "index.html", DOCS / "index.zh-CN.html"]
    if not (DOCS / ".nojekyll").is_file():
        errors.append("docs/.nojekyll is missing")
    for page in pages:
        if not page.is_file():
            errors.append(f"missing landing page: {page.relative_to(ROOT)}")
            continue
        parser = LandingPageParser(page)
        parser.feed(page.read_text(encoding="utf-8"))
        for line in parser.nested_anchors:
            errors.append(f"{page.relative_to(ROOT)}:{line}: nested <a> element")
        for attr, value in parser.links:
            if value.startswith("#") or value.lower().startswith(SKIP_PREFIXES):
                continue
            target = local_target(DOCS, value)
            if target is None:
                continue
            if not target.is_file():
                errors.append(f"{page.relative_to(ROOT)}: {attr}={value!r} does not resolve")
            elif DOCS.resolve() not in target.parents and target != DOCS.resolve():
                errors.append(
                    f"{page.relative_to(ROOT)}: {attr}={value!r} points outside the published docs/ tree"
                )


LINK_RE = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)")


def check_markdown_links(errors: list[str]) -> None:
    files = [ROOT / name for name in BILINGUAL]
    files += sorted(DOCS.glob("*.md"))
    for path in files:
        if not path.is_file():
            continue
        for line_no, line in enumerate(path.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
            for href in LINK_RE.findall(line):
                target = local_target(path.parent, href)
                if target is None:
                    continue
                if not target.exists():
                    errors.append(f"{path.relative_to(ROOT)}:{line_no}: broken link {href!r}")


def main() -> int:
    errors: list[str] = []
    check_bilingual(errors)
    check_landing_pages(errors)
    check_markdown_links(errors)
    if errors:
        for error in errors:
            print(f"error: {error}")
        print(f"\n{len(errors)} documentation problem(s)")
        return 1
    print("docs checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
