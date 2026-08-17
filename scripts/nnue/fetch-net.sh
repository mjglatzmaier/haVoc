#!/usr/bin/env bash
# Fetch a haVoc evaluation network and verify it.
#
# Networks are too large and too numerous to keep in git history, and git-lfs
# puts a bandwidth meter on a public repository. They live instead as assets on
# a permanent GitHub release tagged `nets`, which is not tied to any engine
# version -- an old binary can fetch a new network and vice versa.
#
# Verification is intrinsic rather than bolted on: a network is named after the
# first 12 hex digits of its own sha256, so checking the file against its name
# is a complete integrity check. A truncated or corrupted download cannot keep
# the name it was asked for.
#
# Usage:
#   fetch-net.sh                     # fetch the network this checkout expects
#   fetch-net.sh nn-abc123456789.nnue
#   fetch-net.sh --dir ./nets        # install somewhere specific
#   fetch-net.sh --list              # show published networks
set -euo pipefail

REPO="${HAVOC_REPO:-mjglatzmaier/haVoc}"
NETS_TAG="${HAVOC_NETS_TAG:-nets}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

die() { echo "error: $*" >&2; exit 1; }

# The name the source tree expects, so `fetch-net.sh` with no arguments always
# fetches the network that matches the code you are about to build.
default_net() {
    sed -n 's/^set(HAVOC_DEFAULT_NET "\([^"]*\)".*/\1/p' "$ROOT/CMakeLists.txt" | head -1
}

default_dir() {
    if [[ -n "${XDG_DATA_HOME:-}" ]]; then echo "$XDG_DATA_HOME/havoc"
    else echo "$HOME/.local/share/havoc"; fi
}

sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum   >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else die "need sha256sum or shasum to verify the download"; fi
}

NET=""
DEST=""
LIST=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --dir)  DEST="${2:?--dir needs a directory}"; shift 2 ;;
        --list) LIST=1; shift ;;
        -h|--help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*)     die "unknown option $1" ;;
        *)      NET="$1"; shift ;;
    esac
done

if [[ $LIST -eq 1 ]]; then
    command -v gh >/dev/null 2>&1 || die "--list needs the gh CLI"
    gh release view "$NETS_TAG" --repo "$REPO" --json assets \
        --jq '.assets[] | "\(.name)  \(.size) bytes  \(.downloadCount) downloads"'
    exit 0
fi

[[ -n "$NET" ]] || NET="$(default_net)"
[[ -n "$NET" ]] || die "no network requested and none found in CMakeLists.txt"
[[ "$NET" =~ ^nn-[0-9a-f]{12}\.nnue$ ]] \
    || die "'$NET' is not a network name (expected nn-<12 hex digits>.nnue)"

[[ -n "$DEST" ]] || DEST="$(default_dir)"
mkdir -p "$DEST"
OUT="$DEST/$NET"

if [[ -f "$OUT" ]]; then
    have="$(sha256_of "$OUT")"
    if [[ "${have:0:12}" == "${NET:3:12}" ]]; then
        echo "already have $OUT (verified)"
        exit 0
    fi
    echo "warning: $OUT does not match its name, refetching" >&2
fi

URL="https://github.com/$REPO/releases/download/$NETS_TAG/$NET"
TMP="$(mktemp "$DEST/.$NET.XXXXXX")"
# Never leave a partial file where the engine might load it as a network.
trap 'rm -f "$TMP"' EXIT

echo "fetching $URL"
if command -v curl >/dev/null 2>&1; then
    curl -fL --retry 3 --progress-bar -o "$TMP" "$URL" \
        || die "download failed; is $NET published under the '$NETS_TAG' release?"
elif command -v wget >/dev/null 2>&1; then
    wget -q --show-progress -O "$TMP" "$URL" || die "download failed"
else
    die "need curl or wget"
fi

got="$(sha256_of "$TMP")"
[[ "${got:0:12}" == "${NET:3:12}" ]] \
    || die "checksum mismatch: got ${got:0:12}, expected ${NET:3:12}. Refusing to install."

mv "$TMP" "$OUT"
trap - EXIT
chmod 0644 "$OUT"
echo "installed $OUT"
echo "sha256 $got"
echo
echo "The engine finds this automatically. To use it from elsewhere, either"
echo "copy it next to the binary or set EvalFile in your GUI."
