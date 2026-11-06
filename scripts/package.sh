#!/bin/sh
#
# Builds the release of the C++ port of the Claude Code CLI.
#
# The archive holds the binary, the man page and the README, so that the
# install script can put the program where the reference puts it.
#
# Usage:
#   scripts/package.sh [--release <version>] [--out <dir>]

set -eu

root="$(cd "$(dirname "$0")/.." && pwd)"
# The version is the one of the source, so that the name of the archive is
# never out of step with the program.
version="$(sed -n 's/.*version() { return "\([^"]*\)";.*/\1/p' "${root}/src/core/version.cpp" | head -n 1)"
version="${version:-0.1.0}"
out="${root}/dist"

while [ $# -gt 0 ]; do
    case "$1" in
        --release)
            version="$2"
            shift 2
            ;;
        --release=*)
            version="${1#--release=}"
            shift
            ;;
        --out)
            out="$2"
            shift 2
            ;;
        --out=*)
            out="${1#--out=}"
            shift
            ;;
        -h|--help)
            echo "Usage: scripts/package.sh [--release <version>] [--out <dir>]"
            exit 0
            ;;
        *)
            echo "package.sh: unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

arch="$(uname -m)"
case "$arch" in
    arm64) triple="aarch64-apple-darwin" ;;
    x86_64) triple="x86_64-apple-darwin" ;;
    *) triple="$arch-unknown-linux-gnu" ;;
esac

build="${root}/build-release"

echo "building ${version} (${triple})"
# The dependencies that the build folder has already fetched are reused, so
# that the release does not fetch them a second time.
if [ -d "${root}/build/_deps" ]; then
    fetch="-DFETCHCONTENT_BASE_DIR=${root}/build/_deps"
else
    fetch=""
fi
cmake -B "$build" -DCMAKE_BUILD_TYPE=Release ${fetch} >/dev/null
# Only the program is built: the tests are not in the archive, and building
# them takes as long as the program itself.
cmake --build "$build" --target claude -j"$(sysctl -n hw.ncpu 2>/dev/null || echo 4)" >/dev/null

stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

cp "${build}/src/cc++" "${stage}/cc++"
chmod 755 "${stage}/cc++"

# The changelog of the port goes with it, in the folder of the data of the
# program, so that the notes of the release are the ones of the port.
if [ -f "${root}/CHANGELOG.md" ]; then
    mkdir -p "${stage}/share/claude"
    cp "${root}/CHANGELOG.md" "${stage}/share/claude/CHANGELOG.md"
fi

if [ -f "${root}/docs/cc++.1" ]; then
    mkdir -p "${stage}/share/man/man1"
    cp "${root}/docs/cc++.1" "${stage}/share/man/man1/cc++.1"
fi

cp "${root}/README.md" "${stage}/README.md"
if [ -f "${root}/LICENSE" ]; then
    cp "${root}/LICENSE" "${stage}/LICENSE"
fi

mkdir -p "$out"
name="claude-code-cpp-${version}-${triple}"
tarball="${out}/${name}.tar.gz"

tar -czf "$tarball" -C "$stage" .

echo ""
echo "built ${tarball}"
ls -lh "$tarball" | awk '{print "  " $5 "\t" $9}'
