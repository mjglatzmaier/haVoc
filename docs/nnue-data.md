# Training data: where it lives and how to trust it

## Where

Corpora are **not** in git -- they run to gigabytes. They live in:

```
~/havoc-data/
  MANIFEST.md                 rendered index of everything below
  <file>.meta.json            provenance sidecar, one per artefact
  selfplay-d8-s20240815.epd   the original self-play corpus
  stage0-hce.hbin             3.6M positions, HCE-labelled (Stage 0 mimic set)
  val-games.hbin              67k independent game positions
  stage2/                     the label-quality study
```

`MANIFEST.md` is generated, not written by hand:

```sh
python3 scripts/nnue/provenance.py manifest ~/havoc-data
```

## Why a sidecar exists at all

A corpus is only as trustworthy as the engine that labelled it, and that engine
moves: the evaluation changes, the search changes, and a `.hbin` on disk carries
no memory of which version wrote it. "Why is this net worse than the old one" is
unanswerable six months later if the data cannot be traced to a commit.

So every artefact carries a `<file>.meta.json` naming the commit, the exact
command, the record count and a SHA-256. `provenance.py verify FILE` re-checks
the checksum, which catches the truncated-file and half-overwritten-file cases
that otherwise show up as a mysteriously worse network.

## Three grades of provenance, and the difference matters

| grade | meaning |
|---|---|
| `self-reported` | The tool that wrote the file stamped its own commit in. Trustworthy, and the default for anything generated from now on. |
| `verified` | The claim was **reproduced**: a sample was re-labelled under a build of the named commit and every score matched. |
| `attributed` | The commit is *inferred*, usually from the build timestamp of the binary. Good enough to navigate by, not good enough to bet on. |

Only deterministic labels can be `verified`. The HCE static evaluation is a pure
function of the position, so re-labelling either reproduces it exactly or the
attribution is wrong -- `stage0-hce.hbin` was checked this way, 50,000 of 50,000
identical. A **search** label cannot be verified like this: it depends on time
and thread count, so search corpora stay `attributed` unless the tool stamped
them itself.

Note also that the commit worth naming for an HCE label is the last commit that
changed *evaluation scoring*, not whatever was on `main` that day. `stage0-hce`
names `cd247aa` for that reason; every commit after it only moved the evaluation
behind the `IEvaluator` seam without changing a single score.

## Going forward

`havoc_datagen` writes its own `<output>.meta.json`, with the commit baked into
the binary at build time by `cmake/GitInfo.cmake`. That header is regenerated on
every build rather than at configure time, because a stale hash on a corpus is
worse than no hash: it looks authoritative. A build from a dirty tree sets
`engine_tree_dirty`, since such a binary corresponds to no commit at all.

## Resuming an interrupted run

A datagen run is hours long and holds the whole machine, so losing one to a
reboot or a full disk costs more than the run itself. `havoc_datagen --resume`
continues from where the run died:

```sh
havoc_datagen --games 200000 --depth 8 --threads 24 --seed 3001 -o iter4.epd
# ... machine dies at hour four ...
havoc_datagen --resume -o iter4.epd
```

The resumed corpus is **the same corpus**, not an approximation of it: killing a
400-game run partway and resuming it reproduces the uninterrupted run's records
exactly, with no duplicates.

Two design points make that true.

*Each game seeds its own RNG from its global game index*, rather than one stream
per thread. Skipping a finished game is then a matter of not playing it, where a
per-thread stream would have to be fast-forwarded by replaying every game before
it -- which is the entire cost the resume exists to avoid. Neighbouring `--seed`
values stay uncorrelated because the index is mixed through `seed_seq` the same
way the thread id used to be.

*The checkpoint records the EPD's byte size*, written under the same mutex that
guards the EPD write and immediately after it. On resume the file is truncated
back to that size, which discards the tail of whatever write was in flight when
the process died. Without it, a resumed run would append behind a partial record
and duplicate the games the checkpoint did not know were written.

What is lost is bounded: games buffered but not yet flushed, at most ten per
thread.

`--resume` reads the run's identity out of `<output>.progress` and adopts it, so
the options do not have to be retyped. Supplying one that contradicts the
checkpoint is refused rather than honoured -- including `--hash`, which is part
of a run's identity because at a fixed depth the transposition table changes
which moves the search finds. `--append` is the opposite operation (it adds a
new shard and replays no state) and the two cannot be combined.

The checkpoint is removed only after the run finishes and its metadata is
written. A run that fails on a full disk keeps it, so freeing space and rerunning
with `--resume` continues rather than restarts.
