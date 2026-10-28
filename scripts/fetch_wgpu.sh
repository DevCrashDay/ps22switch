#!/bin/sh
set -e
version=${1:-v29.0.1.1}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/wgpu-native"
mkdir -p "$target"
os=$(uname -s | sed "s/Darwin/macos/;s/Linux/linux/;s/MINGW.*/windows/;s/MSYS.*/windows/")
arch=$(uname -m | sed "s/arm64/aarch64/")
[ "$os" = windows ] && arch=$arch-gnu
(cd "$target" && curl -L -o wgpu.zip "https://github.com/gfx-rs/wgpu-native/releases/download/$version/wgpu-$os-$arch-release.zip")
# curl from Git for Windows and tar.exe read their command line in the ANSI code page and cannot open a folder with letters
# outside it (Chinese on a Western Windows), so both run inside the folder with a plain file name. MinGit has no unzip.
if command -v unzip > /dev/null; then unzip -o -q "$target/wgpu.zip" -d "$target"; else (cd "$target" && tar -xf wgpu.zip); fi
rm "$target/wgpu.zip"
echo "wgpu-native $version in $target"
