#!/usr/bin/env python3
"""Generate stdlib/core/case_data.eco from the Unicode Character Database.

The walk in case.eco searches integer arrays. This script is the only thing that
knows a codepoint's mapping. Re-run when UNICODE_VERSION moves; the generated
file is committed. Then re-emit stdlib/build/stdlib_embedded.h.

    tools/gen_unicode_case.py            # write case_data.eco
    tools/gen_unicode_case.py --check    # refuse if the committed file is stale

UCD text is fetched into tools/ucd/<version>/ (gitignored). A version bump
changes the directory, so a cached 17.0.0 file cannot silently seed 18.0.0.
"""

from __future__ import annotations

import argparse
import pathlib
import sys
import tempfile
import urllib.request

UNICODE_VERSION = "17.0.0"
UCD_BASE = f"https://www.unicode.org/Public/{UNICODE_VERSION}/ucd"

FILES = (
    "UnicodeData.txt",
    "SpecialCasing.txt",
    "DerivedCoreProperties.txt",
)

# SpecialCasing Final_Sigma is hard-coded in case.eco: it is the only
# language-independent conditional row
FINAL_SIGMA = 0x03A3

ASCII = 128


def default_ucd_dir() -> pathlib.Path:
    return pathlib.Path(__file__).resolve().parent / "ucd" / UNICODE_VERSION


def fetch(name: str, dest: pathlib.Path) -> pathlib.Path:
    path = dest / name
    if path.exists():
        return path

    dest.mkdir(parents=True, exist_ok=True)
    url = f"{UCD_BASE}/{name}"
    print(f"fetching {url}", file=sys.stderr)
    with urllib.request.urlopen(url) as response:
        path.write_bytes(response.read())

    return path


def parse_codepoints(field: str) -> tuple[int, ...]:
    field = field.strip()
    if not field:
        return ()

    return tuple(int(part, 16) for part in field.split())


def parse_unicode_data(path: pathlib.Path) -> dict[int, dict[str, object]]:
    rows: dict[int, dict[str, object]] = {}
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            parts = line.rstrip("\n").split(";")
            cp = int(parts[0], 16)
            rows[cp] = {
                "category": parts[2],
                "upper": parse_codepoints(parts[12]),
                "lower": parse_codepoints(parts[13]),
                "title": parse_codepoints(parts[14]),
            }

    return rows


def parse_special_casing(path: pathlib.Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    with path.open(encoding="utf-8") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue

            parts = [part.strip() for part in line.split(";")]
            # unconditional rows have 4 or 5 fields, the last empty
            condition = parts[4] if len(parts) > 4 and parts[4] else ""
            rows.append(
                {
                    "cp": int(parts[0], 16),
                    "lower": parse_codepoints(parts[1]),
                    "title": parse_codepoints(parts[2]),
                    "upper": parse_codepoints(parts[3]),
                    "condition": condition,
                }
            )

    return rows


def parse_derived_ranges(path: pathlib.Path, property_name: str) -> list[tuple[int, int]]:
    ranges: list[tuple[int, int]] = []
    prefix = f"; {property_name}"
    with path.open(encoding="utf-8") as handle:
        for raw in handle:
            line = raw.split("#", 1)[0].strip()
            if not line.endswith(prefix) and f"{prefix} " not in line:
                if not line.endswith(prefix):
                    continue

            left = line.split(";", 1)[0].strip()
            if ".." in left:
                start_s, end_s = left.split("..")
                start, end = int(start_s, 16), int(end_s, 16)
            else:
                start = end = int(left, 16)

            ranges.append((start, end))

    ranges.sort()
    return ranges


def clip_ascii(ranges: list[tuple[int, int]]) -> list[tuple[int, int]]:
    """Drop the A-Z / a-z ranges; case.eco answers those without a table."""
    clipped: list[tuple[int, int]] = []
    for start, end in ranges:
        if end < ASCII:
            continue
        if start < ASCII:
            start = ASCII
        clipped.append((start, end))

    return clipped


def build_maps(
    unicode_data: dict[int, dict[str, object]],
    special: list[dict[str, object]],
) -> dict[str, object]:
    simple_upper: dict[int, int] = {}
    simple_lower: dict[int, int] = {}
    simple_title: dict[int, int] = {}
    special_upper: dict[int, tuple[int, ...]] = {}
    special_lower: dict[int, tuple[int, ...]] = {}
    special_title: dict[int, tuple[int, ...]] = {}

    for cp, row in unicode_data.items():
        if cp < ASCII:
            continue

        upper = row["upper"]
        lower = row["lower"]
        title = row["title"]
        if upper and upper != (cp,):
            simple_upper[cp] = upper[0]
        if lower and lower != (cp,):
            simple_lower[cp] = lower[0]
        if title and title != (cp,):
            simple_title[cp] = title[0]
        elif upper and upper != (cp,):
            # UnicodeData title defaults to upper when absent
            simple_title[cp] = upper[0]

    for row in special:
        if row["condition"]:
            continue

        cp = row["cp"]
        if cp == FINAL_SIGMA:
            # case.eco hard-codes Final Sigma; the unconditional fallback is σ
            # and lives in the simple table already
            continue

        if cp < ASCII:
            continue

        for kind, dest_simple, dest_special in (
            ("upper", simple_upper, special_upper),
            ("lower", simple_lower, special_lower),
            ("title", simple_title, special_title),
        ):
            mapping = row[kind]
            if len(mapping) >= 2:
                dest_special[cp] = mapping
                dest_simple.pop(cp, None)
            elif len(mapping) == 1 and mapping[0] != cp:
                dest_simple[cp] = mapping[0]
                dest_special.pop(cp, None)

    # title that equals the upper mapping is not an exception
    title_exceptions = {
        cp: mapped
        for cp, mapped in simple_title.items()
        if simple_upper.get(cp, cp) != mapped
    }
    title_special = {
        cp: mapping
        for cp, mapping in special_title.items()
        if special_upper.get(cp) != mapping
    }

    return {
        "simple_upper": simple_upper,
        "simple_lower": simple_lower,
        "title_exceptions": title_exceptions,
        "special_upper": special_upper,
        "special_lower": special_lower,
        "title_special": title_special,
    }


def hex8(value: int) -> str:
    return f"0x{value & 0xFFFFFFFF:08X}"


def dump_u32(values: list[int]) -> str:
    if not values:
        return "array<uint32>()"

    lines: list[str] = []
    row: list[str] = []
    for value in values:
        row.append(hex8(value))
        if len(row) == 8:
            lines.append("        " + ", ".join(row) + ",")
            row = []

    if row:
        lines.append("        " + ", ".join(row) + ",")

    return "[\n" + "\n".join(lines) + "\n    ]"


def dump_pairs(pairs: dict[int, int]) -> tuple[str, str]:
    keys = sorted(pairs)
    return dump_u32(keys), dump_u32([pairs[k] for k in keys])


def dump_special(rows: dict[int, tuple[int, ...]]) -> str:
    values: list[int] = []
    for src, mapping in sorted(rows.items()):
        if len(mapping) > 3:
            raise SystemExit(f"U+{src:04X} maps to {len(mapping)} scalars; table holds 3")

        padded = list(mapping) + [0] * (3 - len(mapping))
        values.extend((src, len(mapping), padded[0], padded[1], padded[2]))

    return dump_u32(values)


def dump_ranges(ranges: list[tuple[int, int]]) -> tuple[str, str]:
    return dump_u32([start for start, _ in ranges]), dump_u32([end for _, end in ranges])


def render_fn(name: str, body: str) -> str:
    if body == "array<uint32>()":
        return (
            f"internal function {name}(array<uint32>& $into) : void\n"
            "{\n"
            "}\n"
        )

    return (
        f"internal function {name}(array<uint32>& $into) : void\n"
        "{\n"
        f"    array<uint32> $src = {body};\n"
        "    $into->extend($src);\n"
        "}\n"
    )


def render(maps: dict[str, object], cased: list[tuple[int, int]], ignorable: list[tuple[int, int]]) -> str:
    simple_upper = maps["simple_upper"]
    simple_lower = maps["simple_lower"]
    title_exceptions = maps["title_exceptions"]
    special_upper = maps["special_upper"]
    special_lower = maps["special_lower"]
    title_special = maps["title_special"]

    up_from, up_to = dump_pairs(simple_upper)
    lo_from, lo_to = dump_pairs(simple_lower)
    ti_from, ti_to = dump_pairs(title_exceptions)
    cased_lo, cased_hi = dump_ranges(cased)
    ign_lo, ign_hi = dump_ranges(ignorable)

    return f"""/**
 * Generated by tools/gen_unicode_case.py from Unicode {UNICODE_VERSION}.
 * Do not edit. Re-run the generator when the Unicode version moves, then
 * re-emit stdlib/build/stdlib_embedded.h.
 *
 * Each function fills the integer array case.eco searches. A literal cannot
 * be returned and an owning array cannot be assigned into a static field, so
 * the walk passes the field in and this extends it. ASCII is not in the
 * simple tables or the cased/ignorable ranges; the walk answers those
 * itself. Special rows are five words: from, n, a, b, c.
 */

namespace str;

{render_fn("case_upper_from", up_from)}

{render_fn("case_upper_to", up_to)}

{render_fn("case_lower_from", lo_from)}

{render_fn("case_lower_to", lo_to)}

{render_fn("case_title_from", ti_from)}

{render_fn("case_title_to", ti_to)}

{render_fn("case_upper_special", dump_special(special_upper))}

{render_fn("case_lower_special", dump_special(special_lower))}

{render_fn("case_title_special", dump_special(title_special))}

{render_fn("case_cased_lo", cased_lo)}

{render_fn("case_cased_hi", cased_hi)}

{render_fn("case_ignorable_lo", ign_lo)}

{render_fn("case_ignorable_hi", ign_hi)}
"""


def generate(ucd: pathlib.Path) -> tuple[str, dict[str, object], list, list]:
    for name in FILES:
        fetch(name, ucd)

    unicode_data = parse_unicode_data(ucd / "UnicodeData.txt")
    special = parse_special_casing(ucd / "SpecialCasing.txt")
    cased = clip_ascii(parse_derived_ranges(ucd / "DerivedCoreProperties.txt", "Cased"))
    ignorable = clip_ascii(
        parse_derived_ranges(ucd / "DerivedCoreProperties.txt", "Case_Ignorable")
    )
    maps = build_maps(unicode_data, special)
    return render(maps, cased, ignorable), maps, cased, ignorable


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--ucd",
        type=pathlib.Path,
        default=default_ucd_dir(),
        help="directory holding (or receiving) this Unicode version's UCD text",
    )
    parser.add_argument(
        "--out",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parent.parent
        / "stdlib"
        / "core"
        / "case_data.eco",
        help="path to write case_data.eco",
    )
    parser.add_argument(
        "--check",
        action="store_true",
        help="write to a temp path and refuse if --out differs",
    )
    args = parser.parse_args()

    text, maps, cased, ignorable = generate(args.ucd)
    summary = (
        f"upper {len(maps['simple_upper'])}+{len(maps['special_upper'])}, "
        f"lower {len(maps['simple_lower'])}+{len(maps['special_lower'])}, "
        f"title {len(maps['title_exceptions'])}+{len(maps['title_special'])}, "
        f"cased {len(cased)}, ignorable {len(ignorable)}"
    )

    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if current == text:
            print(f"{args.out} matches Unicode {UNICODE_VERSION} ({summary})", file=sys.stderr)
            return 0

        with tempfile.NamedTemporaryFile(
            "w", encoding="utf-8", suffix=".eco", delete=False
        ) as handle:
            handle.write(text)
            tmp = handle.name

        print(
            f"{args.out} is stale against Unicode {UNICODE_VERSION}; "
            f"re-run tools/gen_unicode_case.py (fresh emit at {tmp})",
            file=sys.stderr,
        )
        return 1

    args.out.write_text(text, encoding="utf-8")
    print(f"wrote {args.out} ({summary})", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
