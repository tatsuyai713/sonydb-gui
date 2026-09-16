#!/usr/bin/env bash
set -euo pipefail

# Debian packages conventionally install shared directories as 0755 even when
# the developer account uses a group-writable umask.
umask 022

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
preset=${1:-release}
if [[ $# -gt 0 ]]; then
  shift
fi
cmake_args=("$@")

case "$preset" in
  release|import-reference) ;;
  *)
    echo "Usage: $0 [release|import-reference] [CMake configure arguments...]" >&2
    exit 2
    ;;
esac

for command_name in cmake cpack dpkg-deb dpkg-shlibdeps; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required Debian packaging command not found: $command_name" >&2
    echo "Install the packaging tools with: sudo apt install cmake dpkg-dev" >&2
    exit 1
  fi
done

build_dir="$project_dir/build/$preset"
dist_dir="$project_dir/dist"

cmake --preset "$preset" -S "$project_dir" "${cmake_args[@]}"
cmake --build --preset "$preset" --parallel
cmake -E make_directory "$dist_dir"

cpack --config "$build_dir/CPackConfig.cmake" \
  -G DEB \
  -B "$dist_dir"

package=$(find "$dist_dir" -maxdepth 1 -type f -name 'sonydb-gui_*.deb' \
  -printf '%T@ %p\n' | sort -nr | head -n 1 | cut -d' ' -f2-)
if [[ -z "$package" ]]; then
  echo "CPack completed but no sonydb-gui Debian package was found." >&2
  exit 1
fi

echo "Debian package created: $package"
