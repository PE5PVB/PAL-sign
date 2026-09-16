"""The run form the contest lettering is stored in.

A byte of coverage per pixel reads fine but draws badly: a row of the
counter band is hundreds of flash bytes, the band is 148 rows deep, and
the ring buffer bridges 4 ms; a burst of rows that each cost more than
their budget therefore drains it to black. Almost every covered pixel
is FULL coverage though, with only a few antialiased bytes at each
edge, so the rows are stored as runs: the solid core becomes one fill
token and only the edges stay as bytes. The flash traffic per row drops
from the glyph width to a handful of bytes.

One row is a sequence of tokens, each:

    skip  uint8   columns to advance before the run
    op    uint8   bit 7 set: solid run of (op & 0x7F) pixels, no data
                  bit 7 clear: op coverage bytes follow, one per pixel

Rows carry no terminator; a per row offset table brackets them. A skip
never exceeds the glyph width, which stays well under 255, and runs
longer than 127 continue with a second token of skip 0.
"""


def rle_rows(data, w, h):
    """data: w*h coverage bytes -> list of h token strings."""
    rows = []
    for r in range(h):
        row = data[r * w:(r + 1) * w]
        toks = bytearray()
        x = 0
        pend = 0                       # skip waiting for the next token
        while x < w:
            while x < w and row[x] == 0:
                x += 1
                pend += 1
            if x >= w:
                break
            if row[x] == 255:
                j = x
                while j < w and row[j] == 255:
                    j += 1
                n = j - x
                while n > 0:
                    take = min(n, 127)
                    toks.append(pend)
                    toks.append(0x80 | take)
                    pend = 0
                    n -= take
                x = j
            else:
                j = x
                while j < w and 0 < row[j] < 255:
                    j += 1
                n = j - x
                k = x
                while n > 0:
                    take = min(n, 127)
                    toks.append(pend)
                    toks.append(take)
                    toks.extend(row[k:k + take])
                    pend = 0
                    n -= take
                    k += take
                x = j
        rows.append(bytes(toks))
    return rows


def emit_rle(f, name, data, w, h):
    """Writes NAME_R (the tokens) and NAME_O (h+1 row offsets)."""
    rows = rle_rows(data, w, h)
    stream = b"".join(rows)
    assert len(stream) < 65536, f"{name}: stream too long for uint16 offsets"
    f.write(f"static const uint8_t {name}_R[] = {{\n")
    for i in range(0, max(1, len(stream)), 20):
        chunk = stream[i:i + 20]
        if chunk:
            f.write("    " + ",".join(str(v) for v in chunk) + ",\n")
    if not stream:
        f.write("    0,\n")            # an empty array is not valid C
    f.write("};\n")
    f.write(f"static const uint16_t {name}_O[] = {{")
    at = 0
    offs = [0]
    for r in rows:
        at += len(r)
        offs.append(at)
    f.write(",".join(str(v) for v in offs))
    f.write("};\n")
    return len(stream)
