#!/usr/bin/env python3
"""Grade haVoc's endgame play against Syzygy tablebases.

Why tablebases rather than a curated puzzle book
------------------------------------------------
A book of famous studies (GM-RAM, Lucena, Philidor) is small, hand-picked and
therefore biased towards positions that are interesting to humans.  It cannot
tell you the *rate* at which the engine mishandles a material constellation,
only whether it solved N specific puzzles.  Syzygy gives a perfect oracle over
an unlimited, uncurated sample, so every number here is a rate with a
confidence interval rather than an anecdote.

The oracle must not be visible to the engine
--------------------------------------------
haVoc supports `setoption name syzygypath`.  If the engine can see the
tablebases it plays these positions perfectly by construction and every metric
below reads 100%.  This harness therefore never sets syzygypath, and asserts
that the engine was not started with one baked in.

Two questions, deliberately kept separate
-----------------------------------------
`probe` mode asks: given a position whose theoretical result is known, does the
engine's chosen move preserve that result?  This grades search and evaluation
together, one move at a time, and is cheap enough to run on tens of thousands
of positions.

`convert` mode asks the harder question: can the engine actually *win* a won
position against perfect defence, inside the fifty-move rule?  A great many
engines know KBNK is winning and still fail to mate, because the win needs a
20+ move plan that no shallow search can see.  Single-move preservation cannot
detect this -- every individual move preserves the win right up until the draw
is claimed -- so conversion has to be measured by playing the ending out.

`probe` mode additionally records what the engine *scores* the position, which
isolates evaluation from search: a theoretically drawn position that the engine
scores at +400 is an evaluation defect regardless of what move it picks.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import sys
from collections import defaultdict
from dataclasses import dataclass, field

try:
    import chess
    import chess.engine
    import chess.syzygy
except ImportError:  # pragma: no cover
    sys.exit("python-chess is required: pip install chess")


PIECE_BY_LETTER = {
    "K": chess.KING,
    "Q": chess.QUEEN,
    "R": chess.ROOK,
    "B": chess.BISHOP,
    "N": chess.KNIGHT,
    "P": chess.PAWN,
}


def parse_constellation(spec: str) -> tuple[list[int], list[int]]:
    """"KRPvKR" -> ([KING, ROOK, PAWN], [KING, ROOK]).

    The spec is written from white's side first, but sampling assigns colours
    randomly so that a one-sided constellation still exercises both colours.
    """
    if spec.count("v") != 1:
        raise ValueError(f"constellation {spec!r} must contain exactly one 'v'")
    white, black = spec.split("v")
    try:
        return ([PIECE_BY_LETTER[c] for c in white], [PIECE_BY_LETTER[c] for c in black])
    except KeyError as exc:
        raise ValueError(f"unknown piece letter {exc.args[0]!r} in {spec!r}") from None


def sample_position(
    white: list[int], black: list[int], rng: random.Random
) -> chess.Board | None:
    """Place the given men on random squares and return the board if it is legal.

    Returns None rather than retrying internally so the caller controls the
    attempt budget; the rejection rate is high for dense constellations and an
    unbounded internal loop would hide that.
    """
    board = chess.Board(None)
    squares = rng.sample(range(64), len(white) + len(black))
    i = 0
    for colour, men in ((chess.WHITE, white), (chess.BLACK, black)):
        for piece in men:
            sq = squares[i]
            i += 1
            # Pawns cannot stand on the first or last rank.
            if piece == chess.PAWN and chess.square_rank(sq) in (0, 7):
                return None
            board.set_piece_at(sq, chess.Piece(piece, colour))

    board.turn = rng.choice([chess.WHITE, chess.BLACK])
    board.clear_stack()
    # is_valid() rejects adjacent kings, a side-not-to-move left in check,
    # missing kings and pawns on impossible ranks.
    if not board.is_valid():
        return None
    if board.is_game_over(claim_draw=False):
        return None
    return board


def wdl_class(wdl: int) -> int:
    """Collapse the five Syzygy WDL values to win / draw / loss.

    Cursed wins and blessed losses (+-1) are results that the fifty-move rule
    takes away.  They are folded into win/loss here because the question this
    harness asks is "did the engine throw away the theoretical result", and
    conflating a cursed win with a draw would credit the engine for a move that
    was only saved by the fifty-move counter.
    """
    return (wdl > 0) - (wdl < 0)


@dataclass
class Bucket:
    """Per-constellation tallies.  Kept as raw counts so they can be summed."""

    sampled: int = 0
    # Result preservation, split by what was at stake.
    won_positions: int = 0
    won_preserved: int = 0
    drawn_positions: int = 0
    drawn_preserved: int = 0
    # Evaluation sanity on positions that are theoretically drawn.
    drawn_scores: list[int] = field(default_factory=list)
    # Evaluation sanity on positions that are theoretically won.
    won_scores: list[int] = field(default_factory=list)
    # Conversion.
    conversions_attempted: int = 0
    conversions_won: int = 0
    conversion_plies: list[int] = field(default_factory=list)
    # Fraction of legal moves that held the result, per sampled position.  A
    # low mean means the filter is doing its job and the sample is hard.
    difficulty: list[float] = field(default_factory=list)


def wilson(successes: int, total: int) -> tuple[float, float]:
    """95% Wilson interval.

    A normal-approximation interval is useless here because the rates we care
    about sit near 0 or 1, exactly where it produces bounds outside [0, 1].
    """
    if total == 0:
        return (0.0, 0.0)
    z = 1.96
    p = successes / total
    denom = 1 + z * z / total
    centre = (p + z * z / (2 * total)) / denom
    margin = z * math.sqrt(p * (1 - p) / total + z * z / (4 * total * total)) / denom
    return (max(0.0, centre - margin), min(1.0, centre + margin))


def score_cp(info: dict, board: chess.Board) -> int | None:
    """Engine score in centipawns from the side to move's point of view."""
    score = info.get("score")
    if score is None:
        return None
    rel = score.relative
    # A mate score is not a centipawn value; report it as a large signed number
    # so it sorts correctly but is obviously not an evaluation.
    return rel.score(mate_score=100000)


def result_after(
    tb: chess.syzygy.Tablebase, board: chess.Board, move: chess.Move
) -> int | None:
    """Theoretical result after `move`, in the *mover's* frame.

    probe_wdl always speaks for the side to move, so once the move is on the
    board it is describing the opponent and has to be negated.  Terminal
    positions are settled directly because they are not in the tables.
    """
    board.push(move)
    try:
        if board.is_checkmate():
            return 1
        if board.is_stalemate() or board.is_insufficient_material():
            return 0
        return -wdl_class(tb.probe_wdl(board))
    except (chess.syzygy.MissingTableError, KeyError):
        return None
    finally:
        board.pop()


def preserving_fraction(
    tb: chess.syzygy.Tablebase, board: chess.Board, target: int
) -> float | None:
    """Fraction of legal moves that hold the theoretical result `target`.

    This is the discrimination filter.  Sampled uniformly, a constellation like
    KBBvK is overwhelmingly made up of positions where almost every move keeps
    the win, so an engine that plays plausible-looking moves scores ~99% and
    the metric cannot tell a good endgame evaluator from a bad one.  Keeping
    only positions where few moves work concentrates the sample on the moments
    that actually decide the ending -- which is what a curated study book is
    trying to approximate, obtained here without the curation bias.
    """
    total = 0
    good = 0
    for move in board.legal_moves:
        after = result_after(tb, board, move)
        if after is None:
            return None
        total += 1
        good += int(after == target)
    if total == 0:
        return None
    return good / total


def run_probe(
    engine: chess.engine.SimpleEngine,
    tb: chess.syzygy.Tablebase,
    board: chess.Board,
    limit: chess.engine.Limit,
    bucket: Bucket,
) -> None:
    """One position: does the engine's move preserve the theoretical result?"""
    try:
        wdl = tb.probe_wdl(board)
    except (chess.syzygy.MissingTableError, KeyError):
        return
    before = wdl_class(wdl)

    info = engine.analyse(board, limit)
    cp = score_cp(info, board)

    if before == 0 and cp is not None:
        bucket.drawn_scores.append(cp)
    elif before > 0 and cp is not None:
        bucket.won_scores.append(cp)

    pv = info.get("pv")
    if not pv:
        return
    move = pv[0]

    after = result_after(tb, board, move)
    if after is None:
        return

    # A lost position cannot be spoiled, so it grades nothing and is excluded
    # rather than counted as a success -- including it would inflate every rate
    # towards 100% in constellations that are mostly losses.
    if before > 0:
        bucket.won_positions += 1
        bucket.won_preserved += int(after > 0)
    elif before == 0:
        bucket.drawn_positions += 1
        bucket.drawn_preserved += int(after == 0)


def best_defence(tb: chess.syzygy.Tablebase, board: chess.Board) -> chess.Move:
    """Perfect defence: maximise DTZ among moves that hold the best result.

    Defending by DTZ rather than just by WDL matters.  A defender that merely
    avoids losing faster gives the attacker no fifty-move pressure; a defender
    that drags the ending out is what actually exposes an engine that knows the
    position is won but has no conversion plan.
    """
    best: tuple[int, int, chess.Move] | None = None
    for move in board.legal_moves:
        board.push(move)
        try:
            if board.is_checkmate():
                res, dtz = -1, 0
            elif board.is_stalemate() or board.is_insufficient_material():
                res, dtz = 0, 0
            else:
                res = -wdl_class(tb.probe_wdl(board))
                dtz = abs(tb.probe_dtz(board))
        except (chess.syzygy.MissingTableError, KeyError):
            board.pop()
            continue
        board.pop()
        key = (res, dtz, move)
        if best is None or (key[0], key[1]) > (best[0], best[1]):
            best = key
    if best is None:
        return next(iter(board.legal_moves))
    return best[2]


def run_convert(
    engine: chess.engine.SimpleEngine,
    tb: chess.syzygy.Tablebase,
    board: chess.Board,
    limit: chess.engine.Limit,
    bucket: Bucket,
    max_plies: int,
) -> None:
    """Play a won position out against perfect defence and see if it converts."""
    try:
        if wdl_class(tb.probe_wdl(board)) <= 0:
            return
    except (chess.syzygy.MissingTableError, KeyError):
        return

    attacker = board.turn
    bucket.conversions_attempted += 1

    for ply in range(max_plies):
        if board.is_game_over(claim_draw=True):
            break
        if board.turn == attacker:
            result = engine.play(board, limit)
            if result.move is None:
                break
            board.push(result.move)
        else:
            board.push(best_defence(tb, board))
        # The fifty-move rule is the whole point of the exercise: a win that
        # takes 101 plies of shuffling is not a win.
        if board.halfmove_clock >= 100:
            break

    outcome = board.outcome(claim_draw=True)
    won = outcome is not None and outcome.winner == attacker
    bucket.conversions_won += int(won)
    if won:
        bucket.conversion_plies.append(len(board.move_stack))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--engine", required=True, help="path to the haVoc UCI binary")
    ap.add_argument("--syzygy", required=True, help="directory holding the .rtbw/.rtbz files")
    ap.add_argument(
        "--constellations",
        default="KPvK,KRPvKR,KBBvK,KBNvK,KNNvK,KQvKRR,KRvKP,KBPvKB,KQvKP",
        help="comma-separated material specs, e.g. KRPvKR",
    )
    ap.add_argument("--samples", type=int, default=500, help="positions per constellation")
    ap.add_argument("--depth", type=int, default=12, help="fixed search depth")
    ap.add_argument("--movetime", type=float, default=None, help="seconds per move (overrides --depth)")
    ap.add_argument("--mode", choices=["probe", "convert"], default="probe")
    ap.add_argument("--max-plies", type=int, default=200, help="convert mode: give up after this many plies")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument(
        "--max-preserving",
        type=float,
        default=1.0,
        help="keep a position only if at most this fraction of legal moves holds "
        "the theoretical result (1.0 = no filter, 0.35 = only sharp positions). "
        "Costs one tablebase probe per legal move but makes the metric "
        "discriminating rather than saturated near 100%%.",
    )
    ap.add_argument("--hash", type=int, default=64, help="engine hash in MB")
    ap.add_argument("--json", default=None, help="write raw results here")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    limit = (
        chess.engine.Limit(time=args.movetime)
        if args.movetime is not None
        else chess.engine.Limit(depth=args.depth)
    )

    tb = chess.syzygy.open_tablebase(args.syzygy)
    engine = chess.engine.SimpleEngine.popen_uci(args.engine)
    try:
        # Never expose the oracle to the engine under test.  Setting hash also
        # gives every constellation the same memory, so a later constellation
        # is not helped by entries left over from an earlier one.
        try:
            engine.configure({"hash": args.hash})
        except chess.engine.EngineError:
            pass

        buckets: dict[str, Bucket] = defaultdict(Bucket)

        for spec in args.constellations.split(","):
            spec = spec.strip()
            if not spec:
                continue
            white, black = parse_constellation(spec)
            bucket = buckets[spec]
            attempts = 0
            budget = args.samples * 400
            while bucket.sampled < args.samples and attempts < budget:
                attempts += 1
                # Colours are swapped at random so a one-sided constellation
                # exercises both the white and black code paths; the piece
                # lists are otherwise identical.
                if rng.random() < 0.5:
                    board = sample_position(white, black, rng)
                else:
                    board = sample_position(black, white, rng)
                if board is None:
                    continue
                if args.max_preserving < 1.0:
                    try:
                        target = wdl_class(tb.probe_wdl(board))
                    except (chess.syzygy.MissingTableError, KeyError):
                        continue
                    # A lost position grades nothing, so filtering it is free.
                    if target < 0:
                        continue
                    frac = preserving_fraction(tb, board, target)
                    if frac is None or frac > args.max_preserving:
                        continue
                    bucket.difficulty.append(frac)
                bucket.sampled += 1
                if args.mode == "probe":
                    run_probe(engine, tb, board, limit, bucket)
                else:
                    run_convert(engine, tb, board, limit, bucket, args.max_plies)

            report(spec, bucket, args.mode)

        if args.json:
            with open(args.json, "w") as fh:
                json.dump(
                    {
                        spec: {
                            k: v for k, v in vars(b).items() if not isinstance(v, list)
                        }
                        | {
                            "drawn_score_abs_mean": mean_abs(b.drawn_scores),
                            "drawn_score_over_150": frac_over(b.drawn_scores, 150),
                            "conversion_plies_median": median(b.conversion_plies),
                            "difficulty_mean": mean(b.difficulty),
                        }
                        for spec, b in buckets.items()
                    },
                    fh,
                    indent=2,
                )
            print(f"wrote {args.json}")
    finally:
        engine.quit()
        tb.close()
    return 0


def mean_abs(xs: list[int]) -> float:
    return sum(abs(x) for x in xs) / len(xs) if xs else 0.0


def mean(xs: list[float]) -> float:
    return sum(xs) / len(xs) if xs else 0.0


def frac_over(xs: list[int], t: int) -> float:
    return sum(1 for x in xs if abs(x) > t) / len(xs) if xs else 0.0


def median(xs: list[int]) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else (s[n // 2 - 1] + s[n // 2]) / 2


def report(spec: str, b: Bucket, mode: str) -> None:
    if mode == "probe":
        wlo, whi = wilson(b.won_preserved, b.won_positions)
        dlo, dhi = wilson(b.drawn_preserved, b.drawn_positions)
        print(
            f"{spec:10s} n={b.sampled:6d}  "
            f"win-held {b.won_preserved:5d}/{b.won_positions:<5d} "
            f"[{wlo:.3f},{whi:.3f}]  "
            f"draw-held {b.drawn_preserved:5d}/{b.drawn_positions:<5d} "
            f"[{dlo:.3f},{dhi:.3f}]  "
            f"|eval| on drawn {mean_abs(b.drawn_scores):7.1f}cp  "
            f"frac>150cp {frac_over(b.drawn_scores, 150):.3f}"
            + (f"  difficulty {mean(b.difficulty):.3f}" if b.difficulty else "")
        )
    else:
        lo, hi = wilson(b.conversions_won, b.conversions_attempted)
        print(
            f"{spec:10s} converted {b.conversions_won:5d}/{b.conversions_attempted:<5d} "
            f"[{lo:.3f},{hi:.3f}]  median plies {median(b.conversion_plies):.0f}"
        )


if __name__ == "__main__":
    raise SystemExit(main())
