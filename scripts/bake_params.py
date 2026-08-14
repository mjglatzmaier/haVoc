#!/usr/bin/env python3
"""
Bake tuned parameters into parameters.hpp source code.

Reads a tuned parameter file (``key = value`` per line, as written by
``havoc_texel``) and updates the corresponding default initialisers in
include/havoc/parameters.hpp, so that the next build ships the fit.

Why this script is written the way it is
----------------------------------------
The previous version re-derived, by regular expression over the header, a
mapping from tuner parameter name to C++ member. That mapping already exists,
exactly, in ``parameters::all_params`` / ``every_param`` in src/parameters.cpp,
which is what the tuner itself uses. Re-deriving it guessed wrong in four
distinct ways -- ``_table``/``_bonus`` name suffixes, brace-initialisers
written without ``=``, two-dimensional arrays, and counting one "update" per
array rather than per value -- and, worst of all, said nothing when a name
failed to resolve. A 1093-parameter fit reported "Updated 82 values" while 199
changed parameters were silently dropped on the floor.

So the mapping is now parsed out of src/parameters.cpp, from the
``emplace_back`` registrations themselves, and any parameter that cannot be
resolved is a hard, non-zero-exit error. Silent partial bakes are the one
failure mode this script must not have.

Usage:
    python3 scripts/bake_params.py tuned_params.txt
    python3 scripts/bake_params.py tuned_params.txt --dry-run
"""

import argparse
import re
import sys
from pathlib import Path

# ── Registration forms in src/parameters.cpp ────────────────────────────────
# Scalar:  result.emplace_back("name", &member);
# 1-D:     result.emplace_back("prefix_" + std::to_string(i), &member[i]);
# 2-D:     result.emplace_back("prefix_" + std::to_string(i) + "_"
#                              + std::to_string(j), &member[i][j]);
# Whitespace and line breaks are arbitrary, so these are matched against the
# whole file with re.S rather than line by line.
RE_SCALAR = re.compile(
    r'emplace_back\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\)')
RE_1D = re.compile(
    r'emplace_back\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\+\s*std::to_string\(\s*\w+\s*\)\s*,'
    r'\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\[', re.S)
RE_2D = re.compile(
    r'emplace_back\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\+\s*std::to_string\(\s*\w+\s*\)'
    r'\s*\+\s*"_"\s*\+\s*std::to_string\(\s*\w+\s*\)\s*,'
    r'\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\[', re.S)
# Symbolic index: result.emplace_back("outpost_defended_knight",
#                                     &outpost_defended_bonus[knight]);
RE_SYMIDX = re.compile(
    r'emplace_back\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*,'
    r'\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*([A-Za-z_][A-Za-z0-9_]*|\d+)\s*\]\s*\)', re.S)
# Name-table row: the piece-square tables build their key from a literal
# prefix, a row name looked up in a table, and a square index:
#   emplace_back(std::string("pst_mg_") + piece_names[pc] + "_"
#                + std::to_string(sq), &pst_mg[pc][sq]);
RE_NAMED2D = re.compile(
    r'emplace_back\(\s*std::string\(\s*"([A-Za-z_][A-Za-z0-9_]*)"\s*\)'
    r'\s*\+\s*([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\w+\s*\]'
    r'\s*\+\s*"_"\s*\+\s*std::to_string\(\s*\w+\s*\)\s*,'
    r'\s*&([A-Za-z_][A-Za-z0-9_]*)\s*\[', re.S)
RE_NAMETABLE = re.compile(
    r'\b([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\d*\s*\]\s*=\s*\{([^}]*)\}')
RE_ENUM = re.compile(r'enum\s+\w+\s*\{([^}]*)\}')


class Mapping:
    """name -> (member, indices) resolution, derived from parameters.cpp."""

    def __init__(self, source: str, types_source: str = ""):
        self.scalars: dict[str, str] = {}   # tuner name -> member
        self.fixed: dict[str, tuple] = {}   # tuner name -> (member, [index])
        self.prefix1: dict[str, str] = {}   # "name_"     -> member (1-D)
        self.prefix2: dict[str, str] = {}   # "name_"     -> member (2-D)
        self.named: dict[str, tuple] = {}   # "pst_mg_"   -> (member, [row names])

        # Enumerators used as array subscripts, e.g. outpost_defended_bonus[knight].
        enums: dict[str, int] = {}
        for body in RE_ENUM.findall(types_source):
            nxt = 0
            for item in body.split(","):
                item = item.strip()
                if not item:
                    continue
                if "=" in item:
                    name, val = item.split("=", 1)
                    try:
                        nxt = int(val.strip(), 0)
                    except ValueError:
                        continue
                    enums[name.strip()] = nxt
                else:
                    enums[item] = nxt
                nxt += 1

        tables = {n: [v.strip().strip('"') for v in body.split(",") if v.strip()]
                  for n, body in RE_NAMETABLE.findall(source)}

        for prefix, table, member in RE_NAMED2D.findall(source):
            if table in tables:
                self.named[prefix] = (member, tables[table])
        for name, member in RE_2D.findall(source):
            self.prefix2[name] = member
        for name, member in RE_1D.findall(source):
            if name not in self.prefix2:
                self.prefix1[name] = member
        for name, member, sym in RE_SYMIDX.findall(source):
            if sym.isdigit():
                self.fixed[name] = (member, [int(sym)])
            elif sym in enums:
                self.fixed[name] = (member, [enums[sym]])
        for name, member in RE_SCALAR.findall(source):
            self.scalars[name] = member

    def resolve(self, key: str):
        """Return (member, [indices]) or None if the key is not registered."""
        if key in self.scalars:
            return self.scalars[key], []
        if key in self.fixed:
            return self.fixed[key]
        for prefix, (member, names) in self.named.items():
            if not key.startswith(prefix):
                continue
            tail = key[len(prefix):]
            row, _, col = tail.rpartition("_")
            if row in names and col.isdigit():
                return member, [names.index(row), int(col)]
        # Longest prefix wins, so that a 2-D "attack_combos_1_0" is never
        # mistaken for a 1-D "attack_combos_1".
        for table, ndim in ((self.prefix2, 2), (self.prefix1, 1)):
            for prefix, member in table.items():
                if not key.startswith(prefix):
                    continue
                tail = key[len(prefix):]
                parts = tail.split("_")
                if len(parts) != ndim or not all(p.isdigit() for p in parts):
                    continue
                return member, [int(p) for p in parts]
        return None


def strip_comments(text: str) -> str:
    """Blank out comments, preserving offsets so spans stay valid."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            for k in range(i, j):
                if out[k] != "\n":
                    out[k] = " "
            i = j
        else:
            i += 1
    return "".join(out)


class Sources:
    """The header files that hold the default values, searched as a set.

    More than one file is needed because parameters.hpp does not always own
    its own defaults: the piece-square tables are declared as
    ``std::array<...> pst_mg = kPieceSquareMiddlegame;``, an alias for a
    constant in squares.hpp. Baking a PST therefore has to follow the alias
    into another file, which the old script had no concept of.
    """

    def __init__(self, paths):
        self.paths = [Path(p) for p in paths]
        self.text = [p.read_text() for p in self.paths]
        self.clean = [strip_comments(t) for t in self.text]

    def find(self, member: str, _depth: int = 0):
        """Locate a member's initialiser, following aliases.

        Returns (file index, "scalar"|"aggregate", span).
        """
        if _depth > 4:
            raise LookupError(f"alias chain for '{member}' is too deep")
        decl = re.compile(r'\b' + re.escape(member) +
                          r'\b\s*(?:=\s*)?(\{|-?\d+|'
                          r'[A-Za-z_][A-Za-z0-9_]*\s*\([^;]*\)\s*;|'
                          r'[A-Za-z_][A-Za-z0-9_]*\s*;)')
        hits = [(i, m) for i, c in enumerate(self.clean) for m in decl.finditer(c)]
        if len(hits) != 1:
            where = ", ".join(str(p) for p in self.paths)
            raise LookupError(
                f"expected exactly one declaration of '{member}' in {where}, "
                f"found {len(hits)}")
        fi, m = hits[0]
        clean = self.clean[fi]
        tok = m.group(1)
        if tok == "{":
            open_brace = clean.index("{", m.start())
            depth, i = 0, open_brace
            while i < len(clean):
                if clean[i] == "{":
                    depth += 1
                elif clean[i] == "}":
                    depth -= 1
                    if depth == 0:
                        return fi, "aggregate", (open_brace, i)
                i += 1
            raise LookupError(f"unterminated initialiser for '{member}'")
        if tok[0].isdigit() or tok[0] == "-":
            num = re.compile(r'-?\d+').search(clean, m.start(1))
            return fi, "scalar", (num.start(), num.end())
        if "(" in tok:
            # A computed default, e.g. pst_eg = endgame_tables_as_seeded().
            # There is no literal to patch, so the whole call expression is
            # replaced by an explicit initialiser built from the fit. Editing
            # the function's source instead would be wrong: it derives the
            # endgame pawn row from the middlegame one, so a tuned endgame
            # pawn value written upstream would be silently overwritten.
            start = m.start(1)
            end = clean.index(";", start)
            return fi, "call", (start, end)
        return self.find(tok.rstrip("; \t\n"), _depth + 1)


def parse_tree(clean: str, span):
    """Parse an aggregate initialiser into a tree of integer-literal spans.

    The header nests braces inconsistently -- ``std::array<std::array<int,5>,5>``
    is written ``{{ {{..}}, {{..}} }}`` -- so a flat scan of the literals cannot
    recover row/column structure. Keeping the tree lets a sparsely registered
    table such as attack_combos (only i in [knight,queen] with j < i) be
    addressed by its true indices instead of by registration order.
    """
    open_brace, close_brace = span
    stack = [[]]
    for m in re.finditer(r'\{|\}|-?\d+', clean[open_brace:close_brace + 1]):
        tok = m.group(0)
        if tok == "{":
            stack.append([])
        elif tok == "}":
            node = stack.pop()
            stack[-1].append(node)
        else:
            stack[-1].append((open_brace + m.start(), open_brace + m.end()))
    return stack[0][0]


def descend(node, indices):
    """Follow indices through the tree, skipping redundant brace levels."""
    def unwrap(n):
        while isinstance(n, list) and len(n) == 1 and isinstance(n[0], list):
            n = n[0]
        return n
    for idx in indices:
        node = unwrap(node)
        if not isinstance(node, list):
            raise IndexError("scalar reached before indices were exhausted")
        node = node[idx]
    node = unwrap(node)
    if not isinstance(node, tuple):
        raise IndexError("indices did not reach a single value")
    return node


def materialise(items, member: str) -> str:
    """Render a complete set of values as an explicit brace initialiser.

    Used for members whose default is computed by a function rather than
    written as a literal. Every element must be present: a partial
    materialisation would silently invent values for the gaps, which is the
    class of failure this script exists to rule out.
    """
    ndim = len(items[0][0])
    if any(len(idx) != ndim for idx, _, _ in items):
        raise ValueError(f"'{member}' mixes index depths and cannot be materialised")
    have = {tuple(idx): val for idx, val, _ in items}
    if ndim == 1:
        n = max(i for (i,) in have) + 1
        missing = [i for i in range(n) if (i,) not in have]
        if missing:
            raise ValueError(f"'{member}' is missing {len(missing)} of {n} values")
        return "{" + ", ".join(str(have[(i,)]) for i in range(n)) + "}"
    if ndim != 2:
        raise ValueError(f"'{member}' has {ndim} dimensions, which is unsupported")
    rows = max(i for i, _ in have) + 1
    cols = max(j for _, j in have) + 1
    missing = [(i, j) for i in range(rows) for j in range(cols) if (i, j) not in have]
    if missing:
        raise ValueError(
            f"'{member}' is missing {len(missing)} of {rows * cols} values")
    out = ["{{"]
    for i in range(rows):
        vals = ", ".join(str(have[(i, j)]) for j in range(cols))
        out.append(f"        {{{{{vals}}}}},")
    out.append("    }}")
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description="Bake tuned params into source")
    ap.add_argument("param_file")
    ap.add_argument("--header", default="include/havoc/parameters.hpp")
    ap.add_argument("--extra-header", action="append",
                    default=["include/havoc/squares.hpp"],
                    help="additional headers that may hold aliased defaults")
    ap.add_argument("--registry", default="src/parameters.cpp")
    ap.add_argument("--types", default="include/havoc/types.hpp")
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would change without writing")
    args = ap.parse_args()

    params: dict[str, int] = {}
    for line in Path(args.param_file).read_text().splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        params[k.strip()] = int(v.strip())
    print(f"Read {len(params)} parameters from {args.param_file}")

    mapping = Mapping(Path(args.registry).read_text(), Path(args.types).read_text())
    sources = Sources([args.header] + list(args.extra_header))

    unresolved, edits = [], {}
    for key, val in params.items():
        r = mapping.resolve(key)
        if r is None:
            unresolved.append(key)
            continue
        member, idx = r
        edits.setdefault(member, []).append((idx, val, key))

    if unresolved:
        print(f"\nERROR: {len(unresolved)} parameters are not registered in "
              f"{args.registry} and cannot be baked:", file=sys.stderr)
        for k in sorted(unresolved)[:20]:
            print(f"  {k}", file=sys.stderr)
        if len(unresolved) > 20:
            print(f"  ... and {len(unresolved) - 20} more", file=sys.stderr)
        return 1

    patches, changed, failures, computed = [], 0, [], 0
    for member, items in edits.items():
        try:
            fi, kind, span = sources.find(member)
        except LookupError as e:
            failures.append(str(e))
            continue
        clean = sources.clean[fi]
        if kind == "scalar":
            idx, val, key = items[0]
            if idx:
                failures.append(f"'{key}' is indexed but '{member}' is a scalar")
                continue
            if clean[span[0]:span[1]] != str(val):
                patches.append((fi, span, str(val)))
                changed += 1
            continue
        if kind == "call":
            try:
                patches.append((fi, span, materialise(items, member)))
                changed += len(items)
                computed += 1
            except ValueError as e:
                failures.append(str(e))
            continue
        tree = parse_tree(clean, span)
        for idx, val, key in items:
            if not idx:
                failures.append(f"'{key}' is a scalar but '{member}' is an array")
                continue
            try:
                pos = descend(tree, idx)
            except (IndexError, TypeError):
                failures.append(f"'{key}' index {idx} is out of range for '{member}'")
                continue
            if clean[pos[0]:pos[1]] != str(val):
                patches.append((fi, pos, str(val)))
                changed += 1

    if failures:
        print(f"\nERROR: {len(failures)} parameters could not be written:", file=sys.stderr)
        for f in failures[:20]:
            print(f"  {f}", file=sys.stderr)
        if len(failures) > 20:
            print(f"  ... and {len(failures) - 20} more", file=sys.stderr)
        return 1

    print(f"Resolved all {len(params)} parameters to {len(edits)} members")
    if computed:
        print(f"{computed} member(s) had a computed default and were "
              f"materialised as explicit initialisers")
    print(f"{changed} values differ from the current source")

    if args.dry_run:
        print("Dry run: nothing written.")
        return 0

    touched = set()
    for fi in range(len(sources.paths)):
        mine = [(sp, tx) for f, sp, tx in patches if f == fi]
        if not mine:
            continue
        text = sources.text[fi]
        for (start, end), tx in sorted(mine, key=lambda p: -p[0][0]):
            text = text[:start] + tx + text[end:]
        sources.paths[fi].write_text(text)
        touched.add(str(sources.paths[fi]))
    print(f"Wrote {changed} values to {', '.join(sorted(touched))}")
    print("Rebuild the engine to use the new defaults.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
