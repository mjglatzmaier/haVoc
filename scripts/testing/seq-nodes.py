#!/usr/bin/env python3
"""Compare two builds for behavioural identity across a sequence of positions.

Why this exists
---------------
`bench` is the usual identity check, and it is not sufficient. It searches a
fixed list of positions at fixed depth, and it carries no state between them:
every position gets a fresh table and fresh history. So `bench` is blind to any
change that only shows up once the engine has accumulated state -- move-ordering
history, correction history, transposition entries surviving into the next
search. Two builds can print the same bench number and search different trees in
an actual game.

This drives one `ucinewgame` and then walks a real game, feeding successive
`position startpos moves ...` and searching each. State carries forward exactly
as it does in play, and the per-move node counts are recorded.

Identical per-move vectors across two builds is strong evidence they are the
same engine. It is not proof -- node counts can collide -- but a change that
alters search behaviour and leaves this vector untouched is rare enough that a
match is worth trusting. A *mismatch* is conclusive: they differ.

This was written to verify that a reimplementation of an accidental LMR
extension actually reproduced the original before spending 3000 games measuring
it. That check is the difference between a match that answers the question asked
and one that answers a different question silently.

Usage:
    seq-nodes.py <binary> [<binary> ...] [--depth N]

Exits non-zero if the binaries disagree, so it can gate a match.
"""

import argparse
import re
import subprocess
import sys

# A quiet Ruy Lopez, chosen because nothing forcing happens: the engine has to
# actually search rather than following a forced sequence, so history and
# ordering state get exercised.
MOVES = ["e2e4", "e7e5", "g1f3", "b8c6", "f1b5", "a7a6", "b5a4", "g8f6",
         "e1g1", "f8e7", "f1e1", "b7b5", "a4b3", "d7d6", "c2c3", "e8g8"]


def node_vector(binary, depth):
    """Search every second position of the game, returning per-move node counts."""
    p = subprocess.Popen([binary], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, text=True)

    def send(line):
        p.stdin.write(line + "\n")
        p.stdin.flush()

    def search():
        send(f"go depth {depth}")
        nodes = 0
        while True:
            line = p.stdout.readline()
            if not line:
                raise RuntimeError(f"{binary} exited mid-search")
            m = re.search(r" nodes (\d+)", line)
            if m:
                nodes = int(m.group(1))
            if line.startswith("bestmove"):
                return nodes

    send("uci")
    # One thread: helper threads make node counts nondeterministic, which would
    # destroy the comparison this script exists to make.
    send("setoption name Threads value 1")
    send("isready")
    while not p.stdout.readline().startswith("readyok"):
        pass
    send("ucinewgame")

    counts = []
    for i in range(0, len(MOVES) + 1, 2):
        played = " ".join(MOVES[:i])
        send(f"position startpos moves {played}" if i else "position startpos")
        counts.append(search())

    send("quit")
    p.wait(timeout=10)
    return counts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binaries", nargs="+")
    ap.add_argument("--depth", type=int, default=11)
    args = ap.parse_args()

    results = {}
    for b in args.binaries:
        counts = node_vector(b, args.depth)
        results[b] = counts
        print(f"{b:40s} total={sum(counts):>9d}  {counts}")

    if len(results) < 2:
        return 0

    vectors = list(results.values())
    if all(v == vectors[0] for v in vectors):
        print("\nidentical: same per-move node vector on every build")
        return 0

    print("\nDIFFER: these builds do not search the same tree")
    return 1


if __name__ == "__main__":
    sys.exit(main())
