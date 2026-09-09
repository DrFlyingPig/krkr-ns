#!/usr/bin/env python3
"""Find which XP3 TJS scripts define/reference a symbol.

Scans each entry's raw bytes for the UTF-16LE form of the symbol, which is how
TJS2 stores member names in its bytecode constant tables.  Diagnostics only.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import xp3_index as X  # noqa: E402


def main():
    archive = Path(sys.argv[1])
    symbol = sys.argv[2]
    needle = symbol.encode("utf-16-le")
    hits = []
    for name, original_size, archived_size, segments in X.entries(archive):
        if not name.lower().endswith(".tjs"):
            continue
        try:
            data = X.extract(archive, segments)
        except Exception:
            continue
        if needle in data:
            hits.append((name, original_size, data.count(needle)))
    for name, size, count in hits:
        print(f"{count:3d}x  {size:8d}  {name}")
    print(f"-- {len(hits)} file(s) reference {symbol!r}")


if __name__ == "__main__":
    main()
