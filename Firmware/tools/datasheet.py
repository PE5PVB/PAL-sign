"""
Search the datasheets and render pages.

    python tools/datasheet.py search "chroma filter"
    python tools/datasheet.py search 0x84 --context 800
    python tools/datasheet.py figures notch
    python tools/datasheet.py page 19 20

Why this exists: part of what you have to know about this chip is NOT in
the text but in the graphs. The response curves of all the luma and
chroma filters for instance are in figures 27 to 44, and the text layer
of the PDF only gives the captions there. Whoever searches on words
alone then concludes something from the register names, and that goes
wrong. With `page` you render the page as a PNG, so that you can really
read the curve off.

The PDFs are in datasheets/ and are not in the repository; see
datasheets/README.md.
"""

import argparse
import io
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DS_DIR = os.path.join(os.path.dirname(ROOT), "datasheets")
DEFAULT = "ADV7390_7391_7392_7393.pdf"


def pdf_path(name):
    p = os.path.join(DS_DIR, name)
    if not os.path.exists(p):
        raise SystemExit("not found: %s\nSee datasheets/README.md for where to get it." % p)
    return p


def pages(name):
    """Text per page; cached next to the PDF so that searching is fast."""
    pdf = pdf_path(name)
    cache = pdf[:-4] + ".txt"
    if not os.path.exists(cache) or os.path.getmtime(cache) < os.path.getmtime(pdf):
        try:
            import pypdf
        except ImportError:
            raise SystemExit("pip install pypdf")
        r = pypdf.PdfReader(pdf)
        text = []
        for p in r.pages:
            try:
                text.append(p.extract_text() or "")
            except Exception:
                text.append("")
        io.open(cache, "w", encoding="utf-8", newline=chr(10)).write(chr(12).join(text))
    return io.open(cache, encoding="utf-8").read().split(chr(12))


def cmd_search(a):
    pp = pages(a.pdf)
    n = 0
    for i, text in enumerate(pp):
        for m in re.finditer(re.escape(a.term), text, re.IGNORECASE):
            lo = max(0, m.start() - a.context // 2)
            print("===== page %d =====" % (i + 1))
            print(text[lo:lo + a.context].replace(chr(10) * 3, chr(10)))
            print()
            n += 1
            if n >= a.max:
                print("(stopped after %d hits)" % a.max)
                return
            break
    if n == 0:
        print("nothing found for %r" % a.term)


def cmd_figures(a):
    pp = pages(a.pdf)
    for i, text in enumerate(pp):
        for m in re.finditer(r"Figure \d+\.[^\n]{0,90}", text):
            if not a.term or a.term.lower() in m.group(0).lower():
                print("page %3d: %s" % (i + 1, m.group(0).strip()))


def cmd_page(a):
    try:
        import pymupdf
    except ImportError:
        raise SystemExit("pip install pymupdf")
    d = pymupdf.open(pdf_path(a.pdf))
    out = a.out or os.environ.get("TEMP", ".")
    for n in a.numbers:
        pix = d[n - 1].get_pixmap(dpi=a.dpi)
        path = os.path.join(out, "ds_p%d.png" % n)
        pix.save(path)
        print("%s  (%dx%d)" % (path, pix.width, pix.height))


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--pdf", default=DEFAULT, help="file name in datasheets/")
    sub = p.add_subparsers(dest="cmd", required=True)

    z = sub.add_parser("search", help="search text with context")
    z.add_argument("term")
    z.add_argument("--context", type=int, default=600, help="number of characters of context")
    z.add_argument("--max", type=int, default=8, help="at most this many hits")
    z.set_defaults(func=cmd_search)

    f = sub.add_parser("figures", help="list the captions of figures")
    f.add_argument("term", nargs="?", default="")
    f.set_defaults(func=cmd_figures)

    r = sub.add_parser("page", help="render pages as PNG to read graphs")
    r.add_argument("numbers", type=int, nargs="+")
    r.add_argument("--dpi", type=int, default=170)
    r.add_argument("--out", default=None)
    r.set_defaults(func=cmd_page)

    a = p.parse_args()

    # The text layer of the datasheet holds characters the Windows console
    # codepage does not have, U+2212 MINUS SIGN among them, and print()
    # then raises UnicodeEncodeError halfway through the hits. Replace what
    # does not fit rather than stopping.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="replace")

    return a.func(a)


if __name__ == "__main__":
    sys.exit(main())
