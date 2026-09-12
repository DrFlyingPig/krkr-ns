"""Compare two completed native menu fixtures and their serialized saves."""
import argparse
import re
import struct
import zlib
from pathlib import Path


def report(path, expected_checks):
    data = (path / "results.txt").read_bytes()
    text = data.decode("utf-16" if data.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig")
    completed = re.search(r"^COMPLETE checks=(\d+)\s*$", text, re.MULTILINE)
    assert completed and int(completed[1]) == expected_checks, f"Incomplete fixture: {path} (expected {expected_checks} checks)"
    return text


def payload(data):
    if data.startswith(b"\xfe\xfe\x02\xff\xfe"):
        compressed, uncompressed = struct.unpack_from("<QQ", data, 5)
        assert compressed == len(data) - 21
        result = zlib.decompress(data[21:])
        assert len(result) == uncompressed
        return result
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--before-checks", type=int, default=22151)
    parser.add_argument("--after-checks", type=int, default=22151)
    args = parser.parse_args()
    before, after = report(args.before, args.before_checks), report(args.after, args.after_checks)
    widths = r"font widths=([^\r\n]+)"
    assert re.search(widths, before)[1] == re.search(widths, after)[1]
    print(f"31 font widths match; native fixtures completed {args.before_checks} / {args.after_checks} checks")
    for i in range(6):
        left = (args.before / f"save-{i}.ksd").read_bytes()
        right = (args.after / f"save-{i}.ksd").read_bytes()
        assert payload(left) == payload(right), f"Save content differs in mode {i}"
        print(f"save-{i}: payload identical, byte identical={left == right}")
    print("Before:\n" + before + "After:\n" + after)


if __name__ == "__main__":
    main()
