#!/bin/sh
# Builds dinput8.dll (the mod) with clang/lld for x86_64 Windows, runs the unit
# tests and, with "package", creates the release zip.
#
#   ./build.sh            build build/dinput8.dll
#   ./build.sh test       build + run the unit tests (Linux/macOS host compiler)
#   ./build.sh package    build + test + dist/EnshroudedBuildingTools-<version>.zip
#                         and dist/EnshroudedBuildingTools-Shroudforge-<version>.zip
#
# Requirements: clang (C/C++ with x86_64-w64-mingw32 target), lld, and the
# MinGW-w64 headers and import libraries (kernel32, user32, gdi32).
#   Ubuntu/Debian:  sudo apt install clang lld mingw-w64-x86-64-dev libvulkan-dev zip
#   Windows:        MSYS2 "CLANG64" shell: pacman -S mingw-w64-clang-x86_64-toolchain mingw-w64-clang-x86_64-vulkan-headers zip
# The MinGW-w64 location is found automatically; override with WINSDK=/path
# (a directory with include/ and lib/).
set -e
cd "$(dirname "$0")"
mkdir -p build

if [ -z "$WINSDK" ]; then
    for d in /usr/x86_64-w64-mingw32 /usr/local/x86_64-w64-mingw32 "$HOME/winsdk"; do
        if [ -f "$d/include/windows.h" ]; then WINSDK=$d; break; fi
    done
fi
SDKINC=""; SDKLIB=""
if [ -n "$WINSDK" ]; then SDKINC="-isystem $WINSDK/include"; SDKLIB="-L $WINSDK/lib"; fi

# Vulkan headers (only the headers: the game loads Vulkan itself). They are taken from
# VKINC, the MinGW-w64 headers, or the host's /usr/include (Ubuntu: libvulkan-dev).
if [ -z "$VKINC" ] && [ ! -f "$WINSDK/include/vulkan/vulkan.h" ] && [ -f /usr/include/vulkan/vulkan.h ]; then
    mkdir -p build/vkinc
    ln -sfn /usr/include/vulkan build/vkinc/vulkan
    [ -d /usr/include/vk_video ] && ln -sfn /usr/include/vk_video build/vkinc/vk_video
    VKINC=build/vkinc
fi
if [ -n "$VKINC" ]; then SDKINC="$SDKINC -isystem $VKINC"; fi

TARGET="--target=x86_64-w64-mingw32"
CXXFLAGS="-O2 -std=c++17 -ffreestanding -fno-exceptions -fno-rtti -fno-threadsafe-statics \
  -ffp-contract=off -fno-stack-protector -Wall -Wno-unused-function -Wno-unused-variable \
  -DWIN32_LEAN_AND_MEAN -DUNICODE"

clang++ $TARGET $SDKINC $CXXFLAGS -c src/main.cpp -o build/main.o
clang $TARGET -c src/chkstk.S -o build/chkstk.o
clang $TARGET -c src/cave.S -o build/cave.o
# No C runtime: own entry point, memcpy/memset in main.cpp, ___chkstk_ms in chkstk.S.
clang++ $TARGET $SDKLIB -fuse-ld=lld -nostdlib -shared -o build/dinput8.dll \
    build/main.o build/chkstk.o build/cave.o src/dinput8.def \
    -lkernel32 -luser32 -lgdi32 -Wl,--entry=DllMainCRTStartup -Wl,--subsystem,windows -Wl,--no-insert-timestamp -s
echo "built build/dinput8.dll"

VERSION=$(sed -n 's/^#define MOD_VERSION "\(.*\)"/\1/p' src/main.cpp)

if [ "$1" = "test" ] || [ "$1" = "package" ]; then
    ${HOSTCXX:-c++} -std=c++17 -O1 -DSRC_DIR="\"$(pwd)/tests\"" -o build/test_core tests/test_core.cpp
    ./build/test_core ${ENSHROUDED_EXE:+"$ENSHROUDED_EXE"}
fi

if [ "$1" = "package" ]; then
    # Nexus Mods / manual install layout: the zip root is the game folder.
    rm -rf build/pkg && mkdir -p build/pkg dist
    cp build/dinput8.dll build/pkg/
    sed "s/^Enshrouded Building Tools [0-9.]*/Enshrouded Building Tools $VERSION/" package/BuildingTools-README.txt > build/pkg/BuildingTools-README.txt
    cp LICENSE build/pkg/BuildingTools-LICENSE.txt
    mkdir -p build/pkg/BuildingTools-Languages
    cp lang/*.txt build/pkg/BuildingTools-Languages/
    ZIP="dist/EnshroudedBuildingTools-$VERSION.zip"
    rm -f "$ZIP"
    (cd build/pkg && zip -q -9 "../../$ZIP" dinput8.dll BuildingTools-README.txt BuildingTools-LICENSE.txt BuildingTools-Languages/*.txt)
    echo "packaged $ZIP"

    # Shroudforge layout (second Nexus file): the same DLL as mods/building-tools/BuildingTools.dll,
    # loaded by Shroudforge through native-plugin.ini, so Shroudforge keeps its own dinput8.dll.
    # The ini, log, languages and blueprints stay next to enshrouded.exe as in the normal zip.
    SF=build/pkg-sf/mods/building-tools
    rm -rf build/pkg-sf && mkdir -p "$SF" build/pkg-sf/BuildingTools-Languages
    cp build/dinput8.dll "$SF/BuildingTools.dll"
    sed "s/@VERSION@/$VERSION/" package/shroudforge/mod.json > "$SF/mod.json"
    cp package/shroudforge/extended.mod.json package/shroudforge/native-plugin.ini "$SF/"
    cp build/pkg/BuildingTools-README.txt build/pkg/BuildingTools-LICENSE.txt build/pkg-sf/
    cp lang/*.txt build/pkg-sf/BuildingTools-Languages/
    SFZIP="dist/EnshroudedBuildingTools-Shroudforge-$VERSION.zip"
    rm -f "$SFZIP"
    (cd build/pkg-sf && zip -q -9 "../../$SFZIP" mods/building-tools/BuildingTools.dll mods/building-tools/mod.json \
        mods/building-tools/extended.mod.json mods/building-tools/native-plugin.ini \
        BuildingTools-README.txt BuildingTools-LICENSE.txt BuildingTools-Languages/*.txt)
    echo "packaged $SFZIP"
fi
