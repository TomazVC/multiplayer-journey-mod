#!/usr/bin/env sh
# Builds dist/journey-coop.zip. Needs zig (https://ziglang.org) on PATH or in $ZIG.
set -eu
ZIG="${ZIG:-zig}"
cd "$(dirname "$0")"
rm -rf build dist && mkdir -p build dist/journey-coop
"$ZIG" cc -target x86_64-windows-gnu -shared -O2 -Wall -Wextra -s \
    -o build/steam_api64.dll src/proxy.c src/steam_api64.def
cp build/steam_api64.dll scripts/*.ps1 scripts/*.cmd dist/journey-coop/
cp docs/LEIA-ME.txt dist/journey-coop/
(cd dist && (command -v zip >/dev/null && zip -qr journey-coop.zip journey-coop || powershell -NoProfile -Command "Compress-Archive -Force journey-coop journey-coop.zip"))
echo "built dist/journey-coop.zip"
