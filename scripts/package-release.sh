#!/bin/sh
# Builds, tests, and packages a release of wt for the current platform:
#
#   scripts/package-release.sh [build-dir]    (default: build-release)
#
# Produces <build-dir>/wt-<version>-<os>-<arch>.tar.gz, containing
# wt-<version>-<os>-<arch>/bin/wt, and prints its path. The release workflow
# (.github/workflows/release.yml) runs this same script.
set -eu

build_dir=${1:-build-release}
source_dir=$(cd "$(dirname "$0")/.." && pwd)

case $(uname -s) in
  Linux) os=linux ;;
  Darwin) os=macos ;;
  *) echo "package-release: unsupported platform $(uname -s); wt supports Linux and macOS" >&2; exit 1 ;;
esac
arch=$(uname -m)

# The same configure, build, and test commands as README.md, in Release mode.
cmake -S "$source_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build_dir" -j
ctest --test-dir "$build_dir" --output-on-failure -j 4

# Name the package after the version the binary itself reports.
version=$("$build_dir/wt" --version)
version=${version#wt }
name="wt-$version-$os-$arch"

# Stage through the install rules so the package holds exactly what
# `cmake --install` installs. DESTDIR also suppresses the PATH hint.
stage=$(mktemp -d)
unpacked=$(mktemp -d)
manifest="$build_dir/install_manifest.txt"
# WARNING: a DESTDIR install records files at their final paths (/bin/wt) in
# the manifest, where the uninstall target would remove them from the real
# system, so the manifest must never outlive the stage.
trap 'rm -rf "$stage" "$unpacked"; rm -f "$manifest"' EXIT
DESTDIR="$stage/$name" cmake --install "$build_dir" --prefix / --strip

# Refuse to ship anything the install rules didn't intend to ship.
files=$(cd "$stage" && find . ! -type d | sort)
if [ "$files" != "./$name/bin/wt" ]; then
  echo "package-release: unexpected files staged for packaging:" >&2
  echo "$files" >&2
  exit 1
fi

archive="$build_dir/$name.tar.gz"
# COPYFILE_DISABLE keeps macOS tar from adding AppleDouble metadata files.
COPYFILE_DISABLE=1 tar -czf "$archive" -C "$stage" "$name"

# The archive holds a stripped copy, so check that copy runs, not the build's.
tar -xzf "$archive" -C "$unpacked"
if [ "$("$unpacked/$name/bin/wt" --version)" != "wt $version" ]; then
  echo "package-release: the packaged wt does not report version $version" >&2
  exit 1
fi
echo "$archive"
