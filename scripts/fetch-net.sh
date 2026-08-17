#!/usr/bin/env bash
# Fetch the default network and verify it against the hash this checkout expects.
#
# Networks are ~20 MB binaries and are not kept in git: a repository that
# accumulates one blob per training iteration becomes expensive to clone
# forever, and git-lfs moves that cost to bandwidth billing rather than
# removing it. What is committed is the hash and the provenance, which is what
# makes a network reproducible; the bytes are served from the release page.
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
spec="$root/nets/default.txt"
dest_dir="${HAVOC_NET_DIR:-$root/nets}"

[ -f "$spec" ] || { echo "fetch-net: missing $spec" >&2; exit 1; }

name=$(grep -E '^name=' "$spec" | cut -d= -f2-)
sha=$(grep -E '^sha256=' "$spec" | cut -d= -f2-)
url=$(grep -E '^url=' "$spec" | cut -d= -f2-)

[ -n "$name" ] && [ -n "$sha" ] && [ -n "$url" ] || {
    echo "fetch-net: $spec must set name=, sha256= and url=" >&2; exit 1; }

mkdir -p "$dest_dir"
out="$dest_dir/$name"

verify() { # A network that fails its hash is not usable, so say so and stop
           # rather than leaving a plausible-looking file in place.
    local got
    got=$(sha256sum "$1" | cut -d' ' -f1)
    [ "$got" = "$sha" ] || { echo "fetch-net: $1 has sha256 $got, expected $sha" >&2; return 1; }
}

if [ -f "$out" ] && verify "$out" 2>/dev/null; then
    echo "fetch-net: $out already present and verified"
else
    echo "fetch-net: downloading $name"
    curl -fL --retry 3 -o "$out.part" "$url"
    mv "$out.part" "$out"
    verify "$out" || { rm -f "$out"; exit 1; }
    echo "fetch-net: verified $out"
fi

echo
echo "Load it with:  setoption name EvalFile value $out"
