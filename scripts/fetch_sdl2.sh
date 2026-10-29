#!/bin/sh
set -e
version=${1:-2.32.10}
here=$(cd "$(dirname "$0")/.." && pwd)
target="$here/third_party/sdl2"
mkdir -p "$target"
(cd "$target" && curl -L -o sdl2.zip "https://github.com/libsdl-org/SDL/releases/download/release-$version/SDL2-devel-$version-mingw.zip")
# curl from Git for Windows and tar.exe read their command line in the ANSI code page and cannot open a folder with letters
# outside it (Chinese on a Western Windows), so both run inside the folder with a plain file name. MinGit has no unzip.
if command -v unzip > /dev/null; then unzip -o -q "$target/sdl2.zip" -d "$target"; else (cd "$target" && tar -xf sdl2.zip); fi
cp -R "$target/SDL2-$version/x86_64-w64-mingw32/." "$target"
rm -rf "$target/SDL2-$version" "$target/sdl2.zip"
echo "SDL2 $version in $target"
