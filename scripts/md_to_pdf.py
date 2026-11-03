#!/usr/bin/env python3
"""Convert the project's Markdown notes into printable A4 PDFs.

Usage:  scripts/md_to_pdf.py INPUT.md OUTPUT.pdf [--title "Header text"] [--break-h2]
        --break-h2  start every "##" section on a new page

Handles what the notes use: # to #### headings, paragraphs, nested "-" and "1." lists, tables,
horizontal rules, fenced code, and inline **bold**, *italic* and `code`. Needs reportlab:
    python3 -m venv .venv && .venv/bin/pip install reportlab
    .venv/bin/python scripts/md_to_pdf.py docs/LEARNING_NOTES.md docs/print/LEARNING_NOTES.pdf --break-h2
"""
import argparse
import re
import sys
from pathlib import Path

from reportlab.lib import colors
from reportlab.lib.pagesizes import A4
from reportlab.lib.styles import ParagraphStyle
from reportlab.lib.units import cm
from reportlab.platypus import (BaseDocTemplate, Frame, HRFlowable, PageBreak, PageTemplate,
                                Paragraph, Preformatted, Spacer, Table, TableStyle)

# The built-in PDF fonts only cover Windows-1252; map the few other characters we use.
REPLACEMENTS = {
    "→": "->", "←": "<-", "≥": ">=", "≤": "<=", "≈": "~", "≠": "!=",
    "✓": "(ok)", "‑": "-", " ": " ", "​": "", "−": "-", "↔": "<->",
}
NAVY = colors.HexColor("#1f2d4d")
GREY = colors.HexColor("#555555")

BODY = ParagraphStyle("body", fontName="Helvetica", fontSize=9.5, leading=13, spaceAfter=4)
H1 = ParagraphStyle("h1", parent=BODY, fontName="Helvetica-Bold", fontSize=18, leading=22,
                    textColor=NAVY, spaceBefore=4, spaceAfter=8)
H2 = ParagraphStyle("h2", parent=BODY, fontName="Helvetica-Bold", fontSize=13.5, leading=17,
                    textColor=NAVY, spaceBefore=12, spaceAfter=5, keepWithNext=1)
H3 = ParagraphStyle("h3", parent=BODY, fontName="Helvetica-Bold", fontSize=11, leading=14,
                    textColor=NAVY, spaceBefore=9, spaceAfter=3, keepWithNext=1)
H4 = ParagraphStyle("h4", parent=BODY, fontName="Helvetica-BoldOblique", fontSize=10, leading=13,
                    spaceBefore=6, spaceAfter=2, keepWithNext=1)
CELL = ParagraphStyle("cell", parent=BODY, fontSize=8, leading=10.2, spaceAfter=0)
CELL_HEAD = ParagraphStyle("cellhead", parent=CELL, fontName="Helvetica-Bold")
CODE = ParagraphStyle("code", fontName="Courier", fontSize=8, leading=10, leftIndent=8,
                      backColor=colors.HexColor("#f3f3f3"), spaceAfter=6, spaceBefore=2)
HEADINGS = {1: H1, 2: H2, 3: H3, 4: H4}
warnings = []


class Heading(Paragraph):
    """Paragraph that remembers its level and text so the PDF outline can list it."""

    def __init__(self, text, style, level, plain):
        super().__init__(text, style)
        self.level, self.plain = level, plain


def sanitize(text):
    for a, b in REPLACEMENTS.items():
        text = text.replace(a, b)
    out = []
    for ch in text:
        try:
            ch.encode("cp1252")
            out.append(ch)
        except UnicodeEncodeError:
            warnings.append(f"unsupported character U+{ord(ch):04X} replaced by '?'")
            out.append("?")
    return "".join(out)


def esc(text):
    return text.replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def inline(text, code_size=8.8):
    """Markdown inline markup -> ReportLab paragraph markup."""
    text = sanitize(text)
    codes = []

    def stash(m):
        codes.append(m.group(1))
        return f"\x00{len(codes) - 1}\x00"

    text = re.sub(r"`([^`]+)`", stash, text)
    text = esc(text)
    text = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", text)
    text = re.sub(r"(?<![\w*])\*(?!\s)(.+?)(?<!\s)\*(?![\w*])", r"<i>\1</i>", text)
    return re.sub(r"\x00(\d+)\x00",
                  lambda m: f'<font face="Courier" size="{code_size}">{esc(codes[int(m.group(1))])}</font>',
                  text)


def plain(text):
    return re.sub(r"[*`]", "", sanitize(text))


def split_row(line):
    cells, cur, in_code = [], "", False
    i = 0
    while i < len(line):
        c = line[i]
        if c == "\\" and i + 1 < len(line) and line[i + 1] == "|":
            cur += "|"
            i += 2
            continue
        if c == "`":
            in_code = not in_code
        if c == "|" and not in_code:
            cells.append(cur.strip())
            cur = ""
        else:
            cur += c
        i += 1
    cells.append(cur.strip())
    if cells and cells[0] == "":
        cells = cells[1:]
    if cells and cells[-1] == "":
        cells = cells[:-1]
    return cells


def make_table(rows, width):
    header, body = rows[0], rows[1:]
    ncols = len(header)
    body = [r + [""] * (ncols - len(r)) if len(r) < ncols else r[:ncols] for r in body]
    lens = [max(len(plain(r[c])) for r in [header] + body) for c in range(ncols)]
    weights = [min(max(l, 6), 60) for l in lens]
    colw = [width * w / sum(weights) for w in weights]
    data = [[Paragraph(inline(c, 7.5), CELL_HEAD) for c in header]]
    data += [[Paragraph(inline(c, 7.5), CELL) for c in r] for r in body]
    t = Table(data, colWidths=colw, repeatRows=1)
    t.setStyle(TableStyle([
        ("GRID", (0, 0), (-1, -1), 0.4, colors.HexColor("#999999")),
        ("BACKGROUND", (0, 0), (-1, 0), colors.HexColor("#e6e6e6")),
        ("VALIGN", (0, 0), (-1, -1), "TOP"),
        ("LEFTPADDING", (0, 0), (-1, -1), 3), ("RIGHTPADDING", (0, 0), (-1, -1), 3),
        ("TOPPADDING", (0, 0), (-1, -1), 2), ("BOTTOMPADDING", (0, 0), (-1, -1), 2),
    ]))
    return t


LIST_RE = re.compile(r"^(\s*)([-*]|\d+\.)\s+(.*)$")


def parse(md, width, break_h2):
    story, lines, i = [], md.splitlines(), 0
    para = []

    def flush_para():
        if para:
            story.append(Paragraph(inline(" ".join(s.strip() for s in para)), BODY))
            para.clear()

    first_h2 = True
    while i < len(lines):
        line = lines[i]
        if line.strip().startswith("```"):
            flush_para()
            i += 1
            block = []
            while i < len(lines) and not lines[i].strip().startswith("```"):
                block.append(sanitize(lines[i]))
                i += 1
            story.append(Preformatted("\n".join(block), CODE))
            i += 1
            continue
        if not line.strip():
            flush_para()
            i += 1
            continue
        m = re.match(r"^(#{1,4})\s+(.*)$", line)
        if m:
            flush_para()
            level, text = len(m.group(1)), m.group(2)
            if level == 2 and break_h2 and not first_h2:
                story.append(PageBreak())
            if level == 2:
                first_h2 = False
            story.append(Heading(inline(text, 10), HEADINGS[level], level, plain(text)))
            i += 1
            continue
        if re.match(r"^---+\s*$", line):
            flush_para()
            story.append(HRFlowable(width="100%", thickness=0.5, color=colors.HexColor("#aaaaaa"),
                                    spaceBefore=3, spaceAfter=5))
            i += 1
            continue
        if line.lstrip().startswith("|"):
            flush_para()
            rows = []
            while i < len(lines) and lines[i].lstrip().startswith("|"):
                cells = split_row(lines[i].strip())
                if not all(re.fullmatch(r":?-{2,}:?", c) for c in cells if c):
                    rows.append(cells)
                i += 1
            if len(rows) >= 1:
                story.append(make_table(rows, width))
                story.append(Spacer(1, 6))
            continue
        m = LIST_RE.match(line)
        if m:
            flush_para()
            indent, marker, text = len(m.group(1).expandtabs(2)), m.group(2), m.group(3)
            i += 1
            while i < len(lines) and lines[i].strip() and not LIST_RE.match(lines[i]) \
                    and lines[i].startswith("  ") and not lines[i].lstrip().startswith(("|", "#")):
                text += " " + lines[i].strip()
                i += 1
            level = indent // 2
            style = ParagraphStyle(f"li{level}", parent=BODY, leftIndent=14 + 14 * level,
                                   bulletIndent=4 + 14 * level, spaceAfter=2.5)
            bullet = "•" if marker in "-*" else marker
            story.append(Paragraph(inline(text), style, bulletText=bullet))
            continue
        para.append(line)
        i += 1
    flush_para()
    return story


def build(src, dst, title, break_h2):
    width = A4[0] - 3.6 * cm

    def footer(canvas, doc):
        canvas.saveState()
        canvas.setFont("Helvetica", 8)
        canvas.setFillColor(GREY)
        canvas.drawString(1.8 * cm, 1.0 * cm, title)
        canvas.drawRightString(A4[0] - 1.8 * cm, 1.0 * cm, f"Page {doc.page}")
        canvas.setStrokeColor(colors.HexColor("#cccccc"))
        canvas.line(1.8 * cm, 1.4 * cm, A4[0] - 1.8 * cm, 1.4 * cm)
        canvas.restoreState()

    class Doc(BaseDocTemplate):
        def afterFlowable(self, flowable):
            if isinstance(flowable, Heading) and flowable.level <= 3:
                key = f"h{id(flowable)}"
                self.canv.bookmarkPage(key)
                self.canv.addOutlineEntry(flowable.plain, key, level=flowable.level - 1, closed=False)

    doc = Doc(str(dst), pagesize=A4, leftMargin=1.8 * cm, rightMargin=1.8 * cm, topMargin=1.7 * cm,
              bottomMargin=1.9 * cm, title=title, author="Ratul Mukherjee (notes written with Claude)")
    doc.addPageTemplates([PageTemplate(id="p", frames=[Frame(
        doc.leftMargin, doc.bottomMargin, width, A4[1] - doc.topMargin - doc.bottomMargin,
        leftPadding=0, rightPadding=0, topPadding=0, bottomPadding=0)], onPage=footer)])
    doc.build(parse(Path(src).read_text(encoding="utf-8"), width, break_h2))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("dst")
    ap.add_argument("--title", default=None)
    ap.add_argument("--break-h2", action="store_true")
    a = ap.parse_args()
    Path(a.dst).parent.mkdir(parents=True, exist_ok=True)
    build(a.src, a.dst, a.title or Path(a.src).stem.replace("_", " ").title(), a.break_h2)
    for w in sorted(set(warnings)):
        print("warning:", w, file=sys.stderr)
    print(f"wrote {a.dst}")


if __name__ == "__main__":
    main()
