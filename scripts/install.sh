#!/bin/sh
#
# Installs the C++ port of the Claude Code CLI.
#
# The layout is the one the native installer of the reference uses, so that
# the two agree about where the program lives:
#
#   ~/.local/share/claude/versions/<version>   the binary itself
#   ~/.local/bin/cc++                         a symlink to the binary
#
# The folders of the cache and of the state are made as well, the way the
# reference makes them.
#
# Usage:
#   scripts/install.sh [--prefix <dir>] [--no-link] <binary|tarball>
#
# Without a prefix the program goes to ~/.local, and the symlink to
# ~/.local/bin. With a prefix the program goes to <prefix>/lib/claude and
# the symlink to <prefix>/bin.

set -eu

prefix=""
link=1
source_path=""

usage() {
    cat <<'EOF'
Usage: scripts/install.sh [options] <binary|tarball>

Options:
  --prefix <dir>   Install under <dir> instead of ~/.local
  --no-link        Do not make the symlink
  -h, --help       Print this help and exit
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --prefix)
            prefix="$2"
            shift 2
            ;;
        --prefix=*)
            prefix="${1#--prefix=}"
            shift
            ;;
        --no-link)
            link=0
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        -*)
            echo "install.sh: unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
        *)
            source_path="$1"
            shift
            ;;
    esac
done

if [ -z "$source_path" ]; then
    echo "install.sh: nothing to install" >&2
    usage >&2
    exit 2
fi

if [ ! -f "$source_path" ]; then
    echo "install.sh: no such file: $source_path" >&2
    exit 1
fi

if [ -z "$prefix" ]; then
    prefix="${HOME}/.local"
fi

version="0.1.0"
# The version is the one the program itself says, when it can be asked.
if [ -x "$source_path" ]; then
    reported="$("$source_path" --version 2>/dev/null | cut -d' ' -f1)"
    if [ -n "$reported" ]; then
        version="$reported"
    fi
fi

versions_dir="${prefix}/share/claude/versions"
install_path="${versions_dir}/${version}"
bin_dir="${prefix}/bin"
link_path="${bin_dir}/cc++"

mkdir -p "$versions_dir" "$bin_dir"
mkdir -p "${HOME}/.cache/claude/staging" 2>/dev/null || true
mkdir -p "${HOME}/.local/state/claude/locks" 2>/dev/null || true

staging="$(mktemp -d)"
trap 'rm -rf "$staging"' EXIT

case "$source_path" in
    *.tar.gz|*.tgz)
        tar -xzf "$source_path" -C "$staging"
        found="$(find "$staging" -type f -name 'cc++' | head -n 1)"
        if [ -z "$found" ]; then
            echo "install.sh: the archive holds no 'cc++' binary" >&2
            exit 1
        fi
        cp "$found" "$install_path"
        ;;
    *)
        cp "$source_path" "$install_path"
        ;;
esac

chmod 755 "$install_path"

# The changelog of the port goes with it, in the folder of the data of the
# program, so that the notes of the release are the ones of the port. The
# file is looked for beside the program, in the folder of the build, and in
# the root of the repository.
changelog=""
source_dir="$(dirname "$source_path")"
for candidate in "${source_dir}/CHANGELOG.md" "${source_dir}/../CHANGELOG.md" \
                 "${source_dir}/../../CHANGELOG.md"; do
    if [ -f "$candidate" ]; then
        changelog="$candidate"
        break
    fi
done
if [ -n "$changelog" ]; then
    mkdir -p "${prefix}/share/claude"
    cp "$changelog" "${prefix}/share/claude/CHANGELOG.md"
fi

if [ "$link" -eq 1 ]; then
    mkdir -p "$bin_dir"
    rm -f "$link_path"
    ln -s "$install_path" "$link_path"
fi

echo "Installed ${version} to ${install_path}"
if [ "$link" -eq 1 ]; then
    echo "Linked ${link_path} -> ${install_path}"
    case ":${PATH}:" in
        *":${bin_dir}:"*) ;;
        *)
            echo ""
            echo "${bin_dir} is not in your PATH. Add it:"
            echo "    echo 'export PATH=\"${bin_dir}:\$PATH\"' >> ~/.zshrc"
            ;;
    esac
fi
