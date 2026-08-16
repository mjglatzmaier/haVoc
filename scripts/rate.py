#!/usr/bin/env python3
"""Estimate an absolute rating for haVoc from a gauntlet PGN.

Opponent ratings are held fixed at their published CCRL values and haVoc's
rating is the maximum likelihood fit of the standard logistic (Elo) model:
solve  sum_i N_i * E(r - R_i)  =  sum_i S_i  for r, where
E(d) = 1 / (1 + 10^(-d/400)).

Draws count a half point, which is what the Elo model assumes; no separate
draw parameter is fitted.
"""
import math
import re
import sys
from collections import defaultdict

# CCRL 40/40 ratings ("all engines" list), single-CPU 64-bit entries, list
# dated 7 August 2026, from computerchess.org.uk/ccrl/4040/rating_list_all.html.
#
# Only fruit and glaurung should normally be used as anchors -- pass
# `--only fruit,glaurung`. Both have very large CCRL sample sizes, so their
# published ratings carry small error bars. The three engines above them in
# this table are kept for reference but are unreliable: gophercheck and
# rustyrival once produced implied ratings of 2284 and 2507 for the *same*
# candidate binary, a 223-point contradiction, which is what a lumpy
# evaluation surface and a thinly-played published rating do to a fit that
# assumes a single strength scale.
#
# Fairy-Max is deliberately absent: it does not appear on the 40/40 list at
# all, so the 1975 figure used in earlier sessions was never sourced. It can
# still be played for interest, it just cannot anchor anything.
ANCHORS = {
    "gophercheck": 2137,
    "sungorus": 2268,
    "rustyrival": 2360,
    "fruit": 2694,
    "glaurung": 2793,
}

SUBJECT = "havoc"


def norm(name):
    """Engine names vary in case and version suffix between match scripts."""
    n = name.strip().lower()
    for key in list(ANCHORS) + [SUBJECT, "fairymax"]:
        if n.startswith(key):
            return key
    return n


def parse(path):
    """Return {opponent: [score_for_subject, games]} from a PGN."""
    text = open(path, errors="replace").read()
    games = defaultdict(lambda: [0.0, 0])
    tags = re.findall(
        r'\[White "([^"]*)"\]\s*\n\[Black "([^"]*)"\]', text
    )
    results = re.findall(r'\[Result "([^"]*)"\]', text)
    if len(tags) != len(results):
        # Fall back to scanning game by game when the tag order differs.
        blocks = re.split(r"\n(?=\[Event )", text)
        tags, results = [], []
        for b in blocks:
            w = re.search(r'\[White "([^"]*)"\]', b)
            bl = re.search(r'\[Black "([^"]*)"\]', b)
            r = re.search(r'\[Result "([^"]*)"\]', b)
            if w and bl and r:
                tags.append((w.group(1), bl.group(1)))
                results.append(r.group(1))
    for (w, b), res in zip(tags, results):
        w, b = norm(w), norm(b)
        if SUBJECT not in (w, b):
            continue
        opp = b if w == SUBJECT else w
        if res == "1/2-1/2":
            s = 0.5
        elif res == "1-0":
            s = 1.0 if w == SUBJECT else 0.0
        elif res == "0-1":
            s = 1.0 if b == SUBJECT else 0.0
        else:
            continue  # unfinished
        games[opp][0] += s
        games[opp][1] += 1
    return games


def expected(diff):
    return 1.0 / (1.0 + 10 ** (-diff / 400.0))


def solve(games):
    total = sum(v[0] for v in games.values())
    lo, hi = 0.0, 4000.0
    for _ in range(200):
        mid = (lo + hi) / 2
        pred = sum(n * expected(mid - ANCHORS[o]) for o, (s, n) in games.items()
                   if o in ANCHORS)
        if pred < total:
            lo = mid
        else:
            hi = mid
    return (lo + hi) / 2


def main():
    argv = sys.argv[1:]
    # --only fruit,glaurung  restricts the fit to a chosen set of anchors.
    # Obscure engines can have both an unreliable published rating and a lumpy
    # eval surface that makes the matchup non-transitive, either of which
    # poisons a maximum-likelihood fit that assumes a single strength scale.
    only = None
    if "--only" in argv:
        i = argv.index("--only")
        only = {x.strip() for x in argv[i + 1].split(",")}
        del argv[i:i + 2]
    paths = argv or ["gauntlet.pgn"]
    # Several gauntlets may be pooled: the same candidate binary at the same
    # time control on the same hardware, just against different opponents.
    games = defaultdict(lambda: [0.0, 0])
    for path in paths:
        for opp, (sc, n) in parse(path).items():
            games[opp][0] += sc
            games[opp][1] += n
    games = dict(games)
    if only:
        games = {o: v for o, v in games.items() if o in only}
    games = {o: v for o, v in games.items() if o in ANCHORS and v[1] > 0}
    if not games:
        print("no completed games against a rated opponent yet")
        return

    print(f"{'opponent':<12}{'anchor':>8}{'games':>8}{'score':>9}"
          f"{'pct':>8}{'implied':>10}")
    n_tot = s_tot = 0
    for o in sorted(games, key=lambda x: ANCHORS[x]):
        s, n = games[o]
        pct = s / n
        if 0 < pct < 1:
            implied = ANCHORS[o] - 400 * math.log10(1 / pct - 1)
            imp = f"{implied:.0f}"
        else:
            imp = "n/a"
        print(f"{o:<12}{ANCHORS[o]:>8}{n:>8}{s:>9.1f}{pct*100:>7.1f}%{imp:>10}")
        n_tot += n
        s_tot += s

    r = solve(games)
    # Standard error of the total score, converted to Elo through the local
    # slope of the logistic curve at the fitted rating.
    var = 0.0
    for o, (s, n) in games.items():
        e = expected(r - ANCHORS[o])
        var += n * e * (1 - e)
    se_score = math.sqrt(var) if var > 0 else float("inf")
    slope = sum(n * expected(r - ANCHORS[o]) * (1 - expected(r - ANCHORS[o]))
                * math.log(10) / 400 for o, (s, n) in games.items())
    se_elo = se_score / slope if slope > 0 else float("inf")
    print(f"\ntotal {s_tot:.1f} / {n_tot}")
    print(f"haVoc estimated rating: {r:.0f}  (+/- {1.96*se_elo:.0f}, 95%)")
    print("anchored on CCRL 40/40; same hardware and time control for all "
          "engines")


if __name__ == "__main__":
    main()
