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
