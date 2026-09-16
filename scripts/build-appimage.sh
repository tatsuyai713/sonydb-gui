#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
preset=${1:-release}

case "$preset" in
  release) ;;
  *)
    echo "Usage: $0 [release]" >&2
    exit 2
    ;;
esac

case "$(uname -m)" in
  x86_64) appimage_arch=x86_64 ;;
  aarch64|arm64) appimage_arch=aarch64 ;;
  *)
    echo "Unsupported AppImage architecture: $(uname -m)" >&2
    exit 2
    ;;
esac

for command_name in cmake curl; do
  if ! command -v "$command_name" >/dev/null 2>&1; then
    echo "Required command not found: $command_name" >&2
    exit 1
  fi
done

build_dir="$project_dir/build/$preset"
package_dir="$project_dir/build/appimage/$preset"
appdir="$package_dir/AppDir"
tools_dir="$project_dir/build/appimage/tools"
dist_dir="$project_dir/dist"
linuxdeploy="$tools_dir/linuxdeploy-$appimage_arch.AppImage"
qt_plugin="$tools_dir/linuxdeploy-plugin-qt-$appimage_arch.AppImage"
output="$dist_dir/SonyDb-GUI-0.1.0-$appimage_arch.AppImage"

cmake --preset "$preset" -S "$project_dir"
cmake --build --preset "$preset" --parallel

cmake -E remove_directory "$appdir"
cmake -E make_directory "$appdir" "$tools_dir" "$dist_dir"
DESTDIR="$appdir" cmake --install "$build_dir" --prefix /usr --strip

# FFmpeg and FFprobe are runtime features of CD import, local transcoding, and
# Walkman compatibility conversion, so include them in the standalone image.
for media_tool in ffmpeg ffprobe; do
  media_path=$(command -v "$media_tool" || true)
  if [[ -z "$media_path" ]]; then
    echo "$media_tool is required to build the standalone AppImage." >&2
    echo "Install it with: sudo apt install ffmpeg" >&2
    exit 1
  fi
  cmake -E copy "$media_path" "$appdir/usr/bin/$media_tool"
done

download_tool() {
  local destination=$1
  local url=$2
  if [[ ! -f "$destination" ]]; then
    echo "Downloading $(basename "$destination")"
    curl --fail --location --retry 3 --output "$destination" "$url"
  fi
  chmod +x "$destination"
}

download_tool "$linuxdeploy" \
  "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-$appimage_arch.AppImage"
download_tool "$qt_plugin" \
  "https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-$appimage_arch.AppImage"

cmake -E rm -f "$output"
deploy_args=(
  --appdir "$appdir"
  --desktop-file "$appdir/usr/share/applications/sonydb-gui.desktop"
  --icon-file "$appdir/usr/share/icons/hicolor/512x512/apps/sonydb-gui.png"
  --executable "$appdir/usr/bin/sonydb-gui"
  --executable "$appdir/usr/bin/ffmpeg"
  --executable "$appdir/usr/bin/ffprobe"
  --plugin qt
  --output appimage
)

echo "Creating $output"
(
  cd "$project_dir"
  PATH="$tools_dir:$PATH" \
  APPIMAGE_EXTRACT_AND_RUN=1 \
  OUTPUT="$output" \
  "$linuxdeploy" "${deploy_args[@]}"
)

chmod +x "$output"
echo "AppImage created: $output"
