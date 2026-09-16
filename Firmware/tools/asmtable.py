"""Writes a table into the source tree as assembler text.

WHY NOT A .bin AND .incbin. The assembler resolves an .incbin path
against the WORKING DIRECTORY, and that is not something the source tree
can pin down: PlatformIO runs from the project root, while the Arduino
IDE starts its build service once, before any sketch is opened, so its
working directory can never be the sketch folder. The C preprocessor
resolves #include against the directory of the INCLUDING FILE, which is
exactly what is needed, so the tables go out as text and the .S includes
them from next door.

It costs 2.66 times the size in the source tree and 0.06 s per table to
assemble, both measured.
"""
import io
import os
import re

NL = chr(10)


def write_table(path, data):
    """The bytes as .4byte lines, little endian, padded to a whole word.

    Padding is safe because the length that the firmware works with is the
    count in the generated _data.h and never the size of the symbol.
    """
    data = bytes(data)
    data += bytes((-len(data)) % 4)
    words = [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]
    lines = []
    for i in range(0, len(words), 8):
        lines.append(".4byte " + ",".join("%#x" % w for w in words[i:i + 8]))
    with io.open(path, "w", encoding="ascii", newline=NL) as fh:
        fh.write("// GENERATED -- do not edit by hand." + NL)
        fh.write("// One .4byte per four bytes of the table, little endian." + NL)
        fh.write(NL.join(lines) + NL)
    return os.path.getsize(path)


def read_table(path):
    """The bytes back out of a .inc.h, for the tools that look at what was
    generated.

    The padding to a whole word comes along. That is harmless: every
    caller works from the counts in the generated _data.h, the same way
    the firmware does, and never from the length of the table itself.
    """
    out = bytearray()
    text = io.open(path, encoding="ascii").read()
    for m in re.finditer(r"0x([0-9A-Fa-f]+)", text):
        out += int(m.group(1), 16).to_bytes(4, "little")
    return bytes(out)
