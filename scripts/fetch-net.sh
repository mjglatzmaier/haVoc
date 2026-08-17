#!/usr/bin/env bash
# Fetch the default network and verify it against the hash this checkout expects.
#
# Networks are ~20 MB and are not kept in git: one blob per training iteration
# makes a repository permanently expensive to clone, and git-lfs moves that
# cost to bandwidth billing rather than removing it. What is committed is the
# hash and the provenance -- which is what makes a network reproducible -- and
# the bytes are served from the release page.
#
# Networks are named after the first 12 hex digits of their own sha256, so the
# filename identifies the contents. The full hash in nets/default.txt is what
# gets checked: a 48-bit prefix is a weaker promise than the whole digest.
#
#   fetch-net.sh                 fetch the network this checkout expects
#   fetch-net.sh --dir <path>    install somewhere other than nets/
#   fetch-net.sh --list          show every published network
set -euo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
spec="$root/nets/default.txt"
dest_dir="${HAVOC_NET_DIR:-$root/nets}"

die() { echo "fetch-net: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --dir)  dest_dir="${2:?--dir needs a directory}"; shift 2 ;;
        --list)
            command -v gh >/dev/null 2>&1 || die "--list needs the gh CLI"
            gh release list --repo "${HAVOC_REPO:-mjglatzmaier/haVoc}" --limit 100 \
                | grep -E '^net-' || echo "(none published yet)"
            exit 0 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1" ;;
    esac
done

[ -f "$spec" ] || die "missing $spec"
name=$(grep -E '^name=' "$spec" | cut -d= -f2-)
sha=$(grep -E '^sha256=' "$spec" | cut -d= -f2-)
url=$(grep -E '^url=' "$spec" | cut -d= -f2-)
[ -n "$name" ] && [ -n "$sha" ] && [ -n "$url" ] || \
    die "$spec must set name=, sha256= and url="

# macOS ships shasum rather than sha256sum, and this is the first script a new
# checkout runs.
sha256_of() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    elif command -v shasum   >/dev/null 2>&1; then shasum -a 256 "$1" | cut -d' ' -f1
    else die "need sha256sum or shasum to verify the download"; fi
}

mkdir -p "$dest_dir"
out="$dest_dir/$name"

if [ -f "$out" ]; then
    got=$(sha256_of "$out")
    if [ "$got" = "$sha" ]; then
        echo "fetch-net: $out already present and verified"
        exit 0
    fi
    echo "fetch-net: $out has sha256 $got, expected $sha -- refetching" >&2
fi

echo "fetch-net: downloading $name"
tmp=$(mktemp "$dest_dir/.$name.XXXXXX")
# Verify before the file appears under its real name: a partial download
# sitting where the engine looks is worse than no download at all.
trap 'rm -f "$tmp"' EXIT
curl -fL --retry 3 --progress-bar -o "$tmp" "$url" \
    || die "download failed; is $name published at $url ?"
got=$(sha256_of "$tmp")
[ "$got" = "$sha" ] || die "sha256 mismatch: got $got, expected $sha. Refusing to install."
mv "$tmp" "$out"
trap - EXIT
chmod 0644 "$out"
echo "fetch-net: verified $out"
