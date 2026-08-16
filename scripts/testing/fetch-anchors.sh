#!/bin/bash
# Fetch and build the rating anchors used by gauntlet-anchors.sh.
#
#   fetch-anchors.sh [--force] [engine ...]
#
# With no engine names, does all five. Existing binaries are left alone unless
# --force is given, so re-running is cheap and safe.
#
# This exists because the anchors were lost once. They had been built by hand
# into a path that did not survive a reboot, and four of the five had no
# recorded recipe anywhere in the repository, so every absolute rating this
# project has published rested on binaries that could not be reproduced. The
# versions are pinned because a different version is a different engine with a
# different rating and is worthless as an anchor.
#
# Deliberately NOT sourcing common.sh: that file requires cutechess-cli and the
# opening book to exist, neither of which is needed to build an engine.
#
#   REFENGINES  where to install (default: ~/code/refengines)
#
# Builds are CPU-heavy. Do not run this while a match is in progress -- SPRTs
# are timing measurements and competing load corrupts them.

set -u

REFENGINES="${REFENGINES:-$HOME/code/refengines}"
FORCE=0
JOBS="$(command -v nproc >/dev/null 2>&1 && nproc || echo 4)"

ARGS=()
for a in "$@"; do
    case "$a" in
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
        *) ARGS+=("$a") ;;
    esac
done

WANT=("${ARGS[@]:-}")
[ -z "${WANT[0]:-}" ] && WANT=(fruit glaurung arasan phalanx zurichess)

mkdir -p "$REFENGINES"
LOG="$REFENGINES/build.log"
: > "$LOG"

RESULTS=()

want() {
    local e
    for e in "${WANT[@]}"; do [ "$e" = "$1" ] && return 0; done
    return 1
}

# Build steps are noisy and only interesting when they fail, so they go to a
# log and are dumped only on error.
run() {
    if ! "$@" >>"$LOG" 2>&1; then
        echo "    command failed: $*" >&2
        echo "    see $LOG" >&2
        return 1
    fi
}

# A binary that builds but does not speak its protocol is worse than one that
# is missing, because the gauntlet will silently score it as losing every game.
verify_uci() {
    local bin="$1" expect="$2"
    printf 'uci\nquit\n' | timeout 10 "$bin" 2>/dev/null | grep -qi "$expect"
}

verify_xboard() {
    local bin="$1"
    printf 'xboard\nprotover 2\nquit\n' | timeout 10 "$bin" 2>/dev/null | grep -qi "feature\|done="
}

record() { RESULTS+=("$1"); echo "  $1"; }

# --------------------------------------------------------------------------
# Fruit 2.1 -- CCRL 40/40 2694. Original host (wbec-ridderkerk.nl) is dead;
# this is a mirror of the unmodified 2005 distribution.
# --------------------------------------------------------------------------
build_fruit() {
    local d="$REFENGINES/fruit-2.1" bin="$REFENGINES/fruit-2.1/src/fruit"
    [ -x "$bin" ] && [ "$FORCE" = 0 ] && { record "fruit      skip (already built)"; return; }
    echo "fruit 2.1..."
    [ -d "$d" ] || run git clone --quiet https://github.com/rwbc/Fruit-2.1.git "$d" || {
        record "fruit      FAILED (clone)"; return; }
    make -C "$d/src" clean >/dev/null 2>&1
    # No -std= in the shipped Makefile, so GCC picks gnu++17 and rejects this
    # 2005 code. Inject the dialect through CXX rather than CXXFLAGS: a
    # command-line CXXFLAGS= would replace the Makefile's own flags, not add
    # to them, silently dropping its -I and -D options.
    if ! run make -C "$d/src" -j"$JOBS" CXX="g++ -std=c++14"; then
        record "fruit      FAILED (build)"; return
    fi
    verify_uci "$bin" "fruit" && record "fruit      OK" || record "fruit      FAILED (no uciok)"
}

# --------------------------------------------------------------------------
# Glaurung 2.2 -- CCRL 40/40 2793. Stockfish's direct predecessor.
# --------------------------------------------------------------------------
build_glaurung() {
    local d="$REFENGINES/glaurung-2.2" bin="$REFENGINES/glaurung-2.2/src/glaurung"
    [ -x "$bin" ] && [ "$FORCE" = 0 ] && { record "glaurung   skip (already built)"; return; }
    echo "glaurung 2.2..."
    [ -d "$d" ] || run git clone --quiet https://github.com/phenri/glaurung.git "$d" || {
        record "glaurung   FAILED (clone)"; return; }
    # value.h uses std::string without including <string>; it survived on the
    # transitive includes libstdc++ used to provide and GCC 13 removed.
    if [ -f "$d/src/value.h" ] && ! grep -q '#include <string>' "$d/src/value.h"; then
        sed -i '1i #include <string>' "$d/src/value.h"
    fi
    make -C "$d/src" clean >/dev/null 2>&1
    if ! run make -C "$d/src" -j"$JOBS" CXX="g++ -std=c++14"; then
        record "glaurung   FAILED (build)"; return
    fi
    verify_uci "$bin" "glaurung" && record "glaurung   OK" || record "glaurung   FAILED (no uciok)"
}

# --------------------------------------------------------------------------
# Arasan 12.2 -- CCRL 40/40 2505. Author's own site, live since the 1990s.
# Note the binary reads arasan.rc from its working directory, which is why
# gauntlet-anchors.sh must pass dir= alongside cmd=.
# --------------------------------------------------------------------------
build_arasan() {
    local d="$REFENGINES/arasan-12.2.0" bin="$REFENGINES/arasan-12.2.0/export/arasanx"
    [ -x "$bin" ] && [ "$FORCE" = 0 ] && { record "arasan     skip (already built)"; return; }
    echo "arasan 12.2..."
    if [ ! -d "$d" ]; then
        run wget -q -O "$REFENGINES/arasan-12.2.0.tar.gz" https://arasanchess.org/arasan-12.2.0.tar.gz || {
            record "arasan     FAILED (download)"; return; }
        run tar xzf "$REFENGINES/arasan-12.2.0.tar.gz" -C "$REFENGINES" || {
            record "arasan     FAILED (extract)"; return; }
    fi
    [ -d "$d/src" ] || { record "arasan     FAILED (unexpected layout)"; return; }
    # 2012 code: std::auto_ptr and friends are gone in C++17.
    if ! run make -C "$d/src" -j"$JOBS" CXX="g++ -std=c++14"; then
        record "arasan     FAILED (build)"; return
    fi
    [ -x "$bin" ] || { record "arasan     FAILED (no binary at export/arasanx)"; return; }
    ( cd "$d/export" && verify_uci "./arasanx" "arasan" ) \
        && record "arasan     OK" || record "arasan     FAILED (no uciok)"
}

# --------------------------------------------------------------------------
# Phalanx XXIV -- CCRL 40/40 2521. xboard/CECP only, not UCI.
# Debian ships XXII (bullseye) and XXV (bookworm/trixie), never XXIV, so the
# distro package is the wrong engine. Use the SourceForge tarball.
# --------------------------------------------------------------------------
build_phalanx() {
    local d="$REFENGINES/phalanx-XXIV" bin="$REFENGINES/phalanx-XXIV/phalanx"
    [ -x "$bin" ] && [ "$FORCE" = 0 ] && { record "phalanx    skip (already built)"; return; }
    echo "phalanx XXIV..."
    if [ ! -d "$d" ]; then
        run wget -q -O "$REFENGINES/phalanx-XXIV.tgz" \
            "https://sourceforge.net/projects/phalanx/files/Version%20XXIV/phalanx-XXIV-source.tgz/download" || {
            record "phalanx    FAILED (download)"; return; }
        mkdir -p "$d"
        run tar xzf "$REFENGINES/phalanx-XXIV.tgz" -C "$d" --strip-components=1 || {
            record "phalanx    FAILED (extract)"; return; }
    fi
    # 1990s C: gets() was removed in C11 and implicit declarations are errors
    # from GCC 14. Both are diagnostics about style, not correctness, here.
    if ! run make -C "$d" -j"$JOBS" CC="gcc -std=gnu99 -Wno-implicit-function-declaration -Wno-implicit-int"; then
        record "phalanx    FAILED (build)"; return
    fi
    [ -x "$bin" ] || { record "phalanx    FAILED (no binary)"; return; }
    ( cd "$d" && verify_xboard "./phalanx" ) \
        && record "phalanx    OK" || record "phalanx    FAILED (no xboard response)"
}

# --------------------------------------------------------------------------
# Zurichess Fribourg -- CCRL 40/40 2412. The only anchor BELOW haVoc, so the
# one that turns the rating fit from extrapolation into interpolation.
#
# The upstream Bitbucket Mercurial repo was deleted in June 2020 and was never
# archived by Software Heritage; the author's site is now parked. This git
# conversion was rescued two months before the deletion and carries the full
# history, so the release is recoverable by commit even though no tag survived.
# c2bcb164 is the last commit of the Fribourg release day; the two commits
# before it touch only the readme and the release script, and the last
# functional change is two weeks earlier. main.go there has
# buildVersion = "fribourg", which the binary prints.
# --------------------------------------------------------------------------
ZURICHESS_COMMIT=c2bcb164a817

build_zurichess() {
    local d="$REFENGINES/zurichess-fribourg" bin="$REFENGINES/zurichess-fribourg/zurichess-bin"
    [ -x "$bin" ] && [ "$FORCE" = 0 ] && { record "zurichess  skip (already built)"; return; }
    echo "zurichess fribourg..."

    local GO
    GO="$(command -v go || true)"
    if [ -z "$GO" ]; then
        local gdir="$REFENGINES/.toolchain"
        GO="$gdir/go/bin/go"
        if [ ! -x "$GO" ]; then
            echo "  installing a local Go toolchain (no system go found)..."
            mkdir -p "$gdir"
            run wget -q -O "$gdir/go.tar.gz" https://go.dev/dl/go1.23.4.linux-amd64.tar.gz || {
                record "zurichess  FAILED (go download)"; return; }
            run tar xzf "$gdir/go.tar.gz" -C "$gdir" || {
                record "zurichess  FAILED (go extract)"; return; }
        fi
    fi

    if [ ! -d "$d" ]; then
        run git clone --quiet https://github.com/easychessanimations/zurichess.git "$d" || {
            record "zurichess  FAILED (clone)"; return; }
    fi
    run git -C "$d" checkout --quiet "$ZURICHESS_COMMIT" || {
        record "zurichess  FAILED (checkout $ZURICHESS_COMMIT)"; return; }

    # Pre-modules GOPATH-era source: synthesise the module under its original
    # import path so the internal imports still resolve.
    [ -f "$d/go.mod" ] || ( cd "$d" && run "$GO" mod init bitbucket.org/zurichess/zurichess )
    if ! ( cd "$d" && GOFLAGS=-mod=mod run "$GO" build -o zurichess-bin ./zurichess ); then
        record "zurichess  FAILED (build)"; return
    fi
    verify_uci "$bin" "zurichess" && record "zurichess  OK" || record "zurichess  FAILED (no uciok)"
}

echo "installing anchors into $REFENGINES"
echo

want fruit     && build_fruit
want glaurung  && build_glaurung
want arasan    && build_arasan
want phalanx   && build_phalanx
want zurichess && build_zurichess

echo
echo "summary:"
printf '  %s\n' "${RESULTS[@]}"

if printf '%s\n' "${RESULTS[@]}" | grep -q FAILED; then
    echo
    echo "at least one anchor failed; build log: $LOG" >&2
    exit 1
fi
