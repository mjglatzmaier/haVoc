#!/usr/bin/env python3
"""Place haVoc on an absolute scale from a cutechess gauntlet log.

Why this exists
---------------
cutechess prints a ranking table whose Elo column is normalised across the whole
pool. In a *gauntlet* the anchors never play each other, so that column is not
pairwise-consistent and the gaps in it must not be subtracted. Doing exactly
that once produced a 420-point gap between haVoc and Fruit when the head-to-head
result said 203 -- an error of over 200 Elo, which made a healthy engine look
like it had regressed by two classes.

This script ignores the ranking table and works only from the raw game results,
one opponent at a time. Each anchor yields an independent estimate; spread
between those estimates is reported, because anchor disagreement is the signal
that the single-strength-scale assumption is breaking down.

Usage:
    anchor_rating.py <gauntlet.log> [more.log ...] [--engine havoc]
"""

import argparse
import math
import os
import re
import sys

# CCRL 40/40, "all engines" list. Rating, number of games behind it.
# Only anchors with a large, long-scrutinised sample belong here.
ANCHORS = {
    "zurichess": (2412, 449),
    "arasan122": (2505, 825),
    "phalanx24": (2521, 887),
    "fruit":     (2694, 505),
    "glaurung":  (2793, 1396),
}

# Deliberately excluded, with the reason, so nobody re-adds them.
REJECTED = {
    "sungorus":    "implied 2192 where Fruit/Glaurung implied 2429/2397 in the same run",
    "gophercheck": "implied 2284 where Rusty-Rival implied 2507 for the same candidate",
    "rustyrival":  "implied 2507 where GopherCheck implied 2284 for the same candidate",
}

RESULT = re.compile(
    r"Finished game \d+ \(([^ ]+) vs ([^)]+)\): (1-0|0-1|1/2-1/2)"
)
SCORE = {"1-0": 1.0, "0-1": 0.0, "1/2-1/2": 0.5}


def elo_diff(p):
    """Elo difference implied by an expected score."""
    if p <= 0.0 or p >= 1.0:
        return None
    return -400.0 * math.log10(1.0 / p - 1.0)


def elo_stderr(p, n):
    """Standard error of that difference, by delta method on a binomial score.

    Undefined at p of 0 or 1, and untrustworthy near them: this is why anchors
    that bracket the engine are worth far more per game than distant ones.
    """
    if n == 0 or p <= 0.0 or p >= 1.0:
        return None
    var = p * (1.0 - p) / n
    return 400.0 / math.log(10) * math.sqrt(var) / (p * (1.0 - p))


def collect(paths, me):
    tally = {}
    for path in paths:
        try:
            text = open(path, errors="ignore").read()
        except OSError as exc:
            print(f"warning: {exc}", file=sys.stderr)
            continue
        for white, black, res in RESULT.findall(text):
            for side, opp, sc in (
                (white, black, SCORE[res]),
                (black, white, 1.0 - SCORE[res]),
            ):
                if side != me:
                    continue
                rec = tally.setdefault(opp, [0, 0.0])
                rec[0] += 1
                rec[1] += sc
    return tally


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logs", nargs="+")
    ap.add_argument("--engine", default="havoc")
    ap.add_argument("--min-games", type=int, default=30,
                    help="ignore opponents with fewer games (default 30)")
    args = ap.parse_args()

    tally = collect(args.logs, args.engine)
    if not tally:
        print(f"no games found for engine '{args.engine}'", file=sys.stderr)
        return 1

    print(f"{args.engine}: head-to-head, computed from game results only\n")
    header = f"{'opponent':14s} {'n':>5s} {'score':>7s} {'diff':>9s} {'+/-':>6s} {'implied':>9s}"
    print(header)
    print("-" * len(header))

    estimates = []
    for opp in sorted(tally, key=lambda o: -tally[o][0]):
        n, s = tally[opp]
        p = s / n
        d = elo_diff(p)
        e = elo_stderr(p, n)
        ds = f"{d:+9.1f}" if d is not None else f"{'n/a':>9s}"
        es = f"{e:6.0f}" if e is not None else f"{'-':>6s}"
        implied = ""
        if opp in ANCHORS and d is not None:
            r = ANCHORS[opp][0] + d
            implied = f"{r:9.0f}"
            if n >= args.min_games and e:
                estimates.append((opp, r, e, n))
        elif opp in REJECTED:
            implied = "  rejected"
        print(f"{opp:14s} {n:5d} {p:6.1%} {ds} {es} {implied}")

    for opp in tally:
        if opp in REJECTED:
            print(f"\nnote: '{opp}' excluded as an anchor -- {REJECTED[opp]}")

    if not estimates:
        print("\nno anchor met the game threshold; no absolute estimate.")
        return 0

    # Inverse-variance weighted mean. Statistical error only: it does not
    # include the systematic offset from measuring at blitz against ratings
    # established at 40/40.
    wsum = sum(1.0 / e**2 for _, _, e, _ in estimates)
    mean = sum(r / e**2 for _, r, e, _ in estimates) / wsum
    sem = math.sqrt(1.0 / wsum)

    print(f"\npooled estimate: {mean:.0f} +/- {sem:.0f} (statistical only)")

    lo = min(r for _, r, _, _ in estimates)
    hi = max(r for _, r, _, _ in estimates)
    spread = hi - lo
    print(f"anchor spread  : {spread:.0f} ({lo:.0f} to {hi:.0f}) across "
          f"{len(estimates)} anchors")

    # A spread much larger than the individual error bars means the anchors do
    # not agree on what the engine is worth, and no weighted mean can repair it.
    typical = sum(e for _, _, e, _ in estimates) / len(estimates)
    if len(estimates) > 1 and spread > 2.5 * typical:
        print(f"WARNING: spread exceeds 2.5x the typical error bar ({typical:.0f}).")
        print("         The anchors disagree. Suspect non-transitive results or a")
        print("         mis-stated published rating before trusting the pooled value.")

    if all(r > mean for r in (ANCHORS[o][0] for o, _, _, _ in estimates)):
        print("NOTE: every anchor is stronger than the engine; the fit is")
        print("      extrapolating. Add an anchor at or below the engine's level.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
