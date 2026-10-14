#!/bin/bash
# Builds the FFmpeg DLLs that Syna ships, for Windows x64, from pinned sources.
#
# Runs on Ubuntu 24.04 with the tools from setup-ubuntu.sh, as any user. Downloads the llvm-mingw
# toolchain and the source archives listed below, checks their SHA-256, builds the libraries as static
# libraries and FFmpeg as DLLs, and writes to the output folder (first argument, default
# ./ffmpeg-build):
#   ffmpeg-syna-<recipe>-win64.zip  bin\ (DLLs, ffmpeg.exe, ffprobe.exe), lib\ (.def and import
#                                   libraries), include\, licenses\ (every library's license and
#                                   notice files, from its archive) and BUILDINFO.txt
#   source\                         the corresponding source: every archive below, unchanged, the
#                                   build recipe (this folder, as syna-ffmpeg-recipe-<recipe>.tar.gz)
#                                   and their SHA-256 list (ffmpeg-sources.sha256)
# <recipe> is the first 12 hex digits of the SHA-256 of this file: any change to the pins or the
# configuration below gives a new package name. docs/FFMPEG.md explains the choices.
set -euo pipefail

# --- Pinned inputs: name, archive, URL, SHA-256 ---------------------------------------------------
TOOLCHAIN=(llvm-mingw llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz
    https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-ubuntu-22.04-x86_64.tar.xz
    5f9c6ed95b2d4bdb2869a488c5fd5857fbdabcf288a0aa3eb1da43f6a08d8ab4)
SOURCES=(
    "ffmpeg FFmpeg-330caae0c1acccd2222edc52a05940c574561ce5.tar.gz https://github.com/FFmpeg/FFmpeg/archive/330caae0c1acccd2222edc52a05940c574561ce5.tar.gz 67b5876ee973a26f267280b2c1eb851a0ac7a502382d20e41b7a819111886af9"
    "zlib zlib-1.3.2.tar.xz https://github.com/madler/zlib/releases/download/v1.3.2/zlib-1.3.2.tar.xz d7a0654783a4da529d1bb793b7ad9c3318020af77667bcae35f95d0e42a792f3"
    "dav1d dav1d-1.5.4.tar.xz https://downloads.videolan.org/pub/videolan/dav1d/1.5.4/dav1d-1.5.4.tar.xz 686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5"
    "opus opus-1.6.1.tar.gz https://downloads.xiph.org/releases/opus/opus-1.6.1.tar.gz 6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
    "lame lame-3.100.tar.gz https://downloads.sourceforge.net/project/lame/lame/3.100/lame-3.100.tar.gz ddfe36cab873794038ae2c1210557ad34857a4b6bdc515785d1da9e175b1da1e"
    "openh264 openh264-2.6.0.tar.gz https://github.com/cisco/openh264/archive/refs/tags/v2.6.0.tar.gz 558544ad358283a7ab2930d69a9ceddf913f4a51ee9bf1bfb9e377322af81a69"
    "kvazaar kvazaar-2.3.2.tar.xz https://github.com/ultravideo/kvazaar/releases/download/v2.3.2/kvazaar-2.3.2.tar.xz 35e506a6d2be9ae343f17787eb80fa214492b276e9c88b6e93749050ae380070"
    "svtav1 SVT-AV1-4.2.0.tar.gz https://github.com/AOMediaCodec/SVT-AV1/archive/refs/tags/v4.2.0.tar.gz d9cb8d5fccbed565d038ae304cb00c5610f1f62388868102ca22132e7844921b"
    "libvpx libvpx-1.17.0.tar.gz https://github.com/webmproject/libvpx/archive/refs/tags/v1.17.0.tar.gz 1020f184046187baa2985dbde38e0691f49c44088bca7a1842b0236c6081dc0a"
    "libvpl libvpl-2.17.0.tar.gz https://github.com/intel/libvpl/archive/refs/tags/v2.17.0.tar.gz 4de3e2faf1e8307fb282e4a43f443191810f6a6b0a484fffa7995ba1c814c6ec"
    "ffnvcodec nv-codec-headers-13.0.19.1.tar.gz https://github.com/FFmpeg/nv-codec-headers/releases/download/n13.0.19.1/nv-codec-headers-13.0.19.1.tar.gz eab9d02d461035a4baded23d4f2e2834e5b17934820b79cea45baaf0c85383eb"
    "amf AMF-headers-v1.5.3.tar.gz https://github.com/GPUOpen-LibrariesAndSDKs/AMF/releases/download/v1.5.3/AMF-headers-v1.5.3.tar.gz 570c8f6593b8c24a6e47a0a9c59b216a7198138e660ba4011590ae913343d5c3"
)

# --- FFmpeg configuration ---------------------------------------------------------------------------
# All of FFmpeg's own decoders, demuxers, muxers, parsers, filters and protocols stay in. External
# code is limited to what Syna uses, each enabled explicitly (--disable-autodetect): see
# docs/FFMPEG.md. No --enable-version3: none of these libraries needs it, so FFmpeg stays
# LGPL-2.1-or-later.
FFMPEG_OPTIONS=(
    --disable-autodetect
    --enable-shared --disable-static
    --disable-debug --disable-doc --disable-ffplay
    --enable-w32threads
    --enable-d3d11va --enable-d3d12va --enable-dxva2 --enable-mediafoundation --enable-schannel
    --enable-ffnvcodec --enable-cuda --enable-cuvid --enable-nvdec --enable-nvenc
    --enable-amf --enable-libvpl
    --enable-zlib
    --enable-libdav1d
    --enable-libopus --enable-libmp3lame
    --enable-libopenh264 --enable-libkvazaar --enable-libsvtav1 --enable-libvpx
    # Their sources (perlin.c, qp_table.c) carry GPL-2.0-or-later notices, but configure builds them
    # without --enable-gpl. Syna uses neither filter.
    --disable-filter=perlin --disable-filter=codecview
)
# ---------------------------------------------------------------------------------------------------

SELF="$(readlink -f "${BASH_SOURCE[0]}")"
RECIPE_DIR="$(dirname "$SELF")"
RECIPE="$(sha256sum "$SELF" | cut -c1-12)"
OUT="$(readlink -m "${1:-ffmpeg-build}")"
# A fixed work path keeps build paths, which end up in the configuration string FFmpeg reports, the
# same on every machine.
WORK=/tmp/syna-ffmpeg-build
PREFIX=$WORK/prefix
HOST=x86_64-w64-mingw32
JOBS=$(nproc)

log() { printf '\n=== %s\n' "$*"; }

fetch() {  # archive url sha256 destdir
    local file="$1" url="$2" sha="$3" dir="$4"
    if [[ ! -f "$dir/$file" ]] || ! echo "$sha  $dir/$file" | sha256sum -c --status; then
        curl -fsSL --retry 3 -o "$dir/$file.part" "$url"
        mv "$dir/$file.part" "$dir/$file"
    fi
    echo "$sha  $dir/$file" | sha256sum -c --quiet || { echo "SHA-256 mismatch: $file" >&2; exit 1; }
}

enter() {  # name: unpacks that source into $WORK/src/<name> and changes into it
    local name="$1" spec n file
    for spec in "${SOURCES[@]}"; do
        read -r n file _ _ <<<"$spec"
        if [[ $n == "$name" ]]; then
            rm -rf "$WORK/src/$name"; mkdir -p "$WORK/src/$name"
            tar -xf "$OUT/source/$file" -C "$WORK/src/$name" --strip-components=1
            cd "$WORK/src/$name"
            return
        fi
    done
    echo "unknown source $name" >&2; exit 1
}

rm -rf "$WORK"
mkdir -p "$WORK/src" "$PREFIX" "$OUT/source" "$OUT/toolchain"
rm -f "$OUT"/ffmpeg-syna-*-win64.zip

log "Downloading the toolchain and sources"
fetch "${TOOLCHAIN[1]}" "${TOOLCHAIN[2]}" "${TOOLCHAIN[3]}" "$OUT/toolchain"
keep=()
for spec in "${SOURCES[@]}"; do
    read -r _ file url sha <<<"$spec"
    fetch "$file" "$url" "$sha" "$OUT/source"
    keep+=("$file")
done
# The source folder is what a release attaches: nothing but this recipe's archives may be in it.
for f in "$OUT"/source/*; do
    [[ " ${keep[*]} " == *" $(basename "$f") "* ]] || rm -f "$f"
done
tar -xf "$OUT/toolchain/${TOOLCHAIN[1]}" -C "$WORK"
TC="$WORK/${TOOLCHAIN[1]%.tar.xz}"
export PATH="$TC/bin:$PATH"

# Release build, no build paths in the binaries, stack protection; every library static, the
# toolchain's runtime libraries too, so the DLLs depend on Windows system libraries only.
export CC=$HOST-clang CXX=$HOST-clang++ AR=$HOST-ar RANLIB=$HOST-ranlib NM=$HOST-nm STRIP=$HOST-strip
export CFLAGS="-O2 -ffile-prefix-map=$WORK=. -fstack-protector-strong -D_FORTIFY_SOURCE=2"
export CXXFLAGS="$CFLAGS"
export LDFLAGS="-static -fstack-protector-strong"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig" PKG_CONFIG_PATH=""
# SVT-AV1 compiles __DATE__ and __TIME__ into its version text; clang takes them from here instead,
# so two builds of this recipe give the same bytes.
export SOURCE_DATE_EPOCH=946684800

cat > "$WORK/cross.meson" <<EOF
[binaries]
c = '$HOST-clang'
cpp = '$HOST-clang++'
ar = '$HOST-ar'
strip = '$HOST-strip'
windres = '$HOST-windres'
pkg-config = 'pkg-config'
nasm = 'nasm'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF
cat > "$WORK/cross.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)
set(CMAKE_C_COMPILER $HOST-clang)
set(CMAKE_CXX_COMPILER $HOST-clang++)
set(CMAKE_RC_COMPILER $HOST-windres)
set(CMAKE_ASM_NASM_COMPILER nasm)
set(CMAKE_FIND_ROOT_PATH $PREFIX $TC/$HOST)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF
CMAKE=(cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE="$WORK/cross.cmake" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=OFF)
AUTOCONF=(--host=$HOST --prefix="$PREFIX" --disable-shared --enable-static)

log "zlib"
enter zlib
CHOST=$HOST ./configure --prefix="$PREFIX" --static
make -j"$JOBS" install

log "dav1d"
enter dav1d
meson setup build --cross-file="$WORK/cross.meson" --prefix="$PREFIX" --libdir=lib --buildtype=release \
    --default-library=static -Denable_tools=false -Denable_tests=false -Denable_examples=false
ninja -C build -j"$JOBS" install

log "Opus"
enter opus
./configure "${AUTOCONF[@]}" --disable-extra-programs --disable-doc
make -j"$JOBS" install

log "LAME"
enter lame
CFLAGS="$CFLAGS -DNDEBUG" ./configure "${AUTOCONF[@]}" --disable-frontend --disable-decoder --disable-gtktest
make -j"$JOBS" install

log "OpenH264"
enter openh264
make -j"$JOBS" OS=mingw_nt ARCH=x86_64 CC="$CC" CXX="$CXX" AR="$AR" PREFIX="$PREFIX" \
    INCLUDE_PREFIX="$PREFIX/include/wels" LIBDIR_NAME=lib BUILDTYPE=Release DEBUGSYMBOLS=False install-static
sed -i 's/-lstdc++/-lc++/' "$PREFIX/lib/pkgconfig/openh264.pc"

log "Kvazaar"
enter kvazaar
./configure "${AUTOCONF[@]}"
make -j"$JOBS" install
printf 'Cflags.private: -DKVZ_STATIC_LIB\nLibs.private: -lpthread\n' >> "$PREFIX/lib/pkgconfig/kvazaar.pc"

log "SVT-AV1"
enter svtav1
"${CMAKE[@]}" -B build -DBUILD_TESTING=OFF -DBUILD_APPS=OFF -DENABLE_AVX512=ON -DSVT_AV1_LTO=OFF
ninja -C build -j"$JOBS" install

log "libvpx"
enter libvpx
CROSS=$HOST- ./configure --target=x86_64-win64-gcc --prefix="$PREFIX" --as=nasm --disable-shared --enable-static \
    --enable-pic --enable-vp9-highbitdepth --disable-examples --disable-tools --disable-docs --disable-unit-tests
make -j"$JOBS" install

log "libvpl"
enter libvpl
# Without MSVC, _MSC_VER is undefined and this test wrongly picks the pre-2005 MSVC branch.
sed -i 's/^#if _MSC_VER < 1400$/#if defined(_MSC_VER) \&\& _MSC_VER < 1400/' libvpl/src/windows/mfx_dispatcher_defs.h
"${CMAKE[@]}" -B build -DBUILD_DISPATCHER=ON -DBUILD_DEV=ON -DBUILD_PREVIEW=OFF -DBUILD_TOOLS=OFF \
    -DBUILD_TOOLS_ONEVPL_EXPERIMENTAL=OFF -DINSTALL_EXAMPLE_CODE=OFF -DBUILD_TESTS=OFF
ninja -C build -j"$JOBS" install
echo 'Libs.private: -lc++' >> "$PREFIX/lib/pkgconfig/vpl.pc"

log "nv-codec-headers and AMF headers"
enter ffnvcodec
make PREFIX="$PREFIX" install
enter amf
mkdir -p "$PREFIX/include" && cp -r AMF "$PREFIX/include/"

log "FFmpeg"
enter ffmpeg
# Built from an archive, FFmpeg only knows its release number: add the commit, which names the source.
read -r _ archive _ _ <<<"${SOURCES[0]}"
commit=${archive#FFmpeg-}
./configure --prefix=/ffmpeg --target-os=mingw32 --arch=x86_64 --cross-prefix=$HOST- --cc="$CC" --cxx="$CXX" \
    --pkg-config=pkg-config --pkg-config-flags=--static \
    --extra-cflags="-I$PREFIX/include $CFLAGS" --extra-ldflags="-L$PREFIX/lib $LDFLAGS -Wl,--no-insert-timestamp" \
    --extra-version="g${commit:0:10}-syna" "${FFMPEG_OPTIONS[@]}" || { tail -50 ffbuild/config.log; exit 1; }
make -j"$JOBS"
make install DESTDIR="$WORK/install"

log "Checking that no GPL-licensed source went into the DLLs"
# FFmpeg's tree holds GPL-only files that configure leaves out of LGPL builds, but not all of them
# (see --disable-filter above). Every object FFmpeg compiled is checked against the license notice of
# its source file and of the headers it included (its .d file); the libraries' trees may not hold
# GPL-licensed source files at all.
gpl_files() {  # directory: source files whose notice is the GPL, not the LGPL
    find "$1" -type f \( -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.cc' -o -name '*.asm' \
        -o -name '*.S' -o -name '*.inc' \) -print0 |
        xargs -0 -r grep -lIE 'GNU General Public License|GPL-2\.0|GPL-3\.0|GPLv[23]' |
        xargs -r -d '\n' grep -LIE 'Lesser General Public|Library General Public|LGPL' || true
}
gpl_found=()
for spec in "${SOURCES[@]}"; do
    read -r name _ _ _ <<<"$spec"
    [[ $name == ffmpeg ]] && continue
    while IFS= read -r f; do [[ -n $f ]] && gpl_found+=("${f#$WORK/src/}"); done < <(gpl_files "$WORK/src/$name")
done
mapfile -t ffmpeg_gpl < <(gpl_files "$WORK/src/ffmpeg")
for f in "${ffmpeg_gpl[@]}"; do
    rel=${f#$WORK/src/ffmpeg/}
    if [[ -f ${f%.*}.o ]] || grep -rqF --include='*.d' -- "$rel" "$WORK/src/ffmpeg"; then gpl_found+=("ffmpeg/$rel"); fi
done
if (( ${#gpl_found[@]} )); then
    printf 'GPL-licensed source compiled into the DLLs: %s\n' "${gpl_found[@]}" >&2
    exit 1
fi
GPL_CHECK="GPL source check: passed (FFmpeg's ${#ffmpeg_gpl[@]} GPL-licensed files not compiled; none in the libraries)"
echo "$GPL_CHECK"

log "Packaging"
PKG="$WORK/package"
mkdir -p "$PKG/bin" "$PKG/lib" "$PKG/licenses"
cp "$WORK"/install/ffmpeg/bin/*.dll "$WORK"/install/ffmpeg/bin/ffmpeg.exe "$WORK"/install/ffmpeg/bin/ffprobe.exe "$PKG/bin/"
cp "$WORK"/install/ffmpeg/lib/*.def "$WORK"/install/ffmpeg/lib/*.dll.a "$PKG/lib/"
cp -r "$WORK/install/ffmpeg/include" "$PKG/"

# License texts, notices and patent statements, as each project ships them. The NVIDIA and AMD
# headers carry their MIT license only in their source files: it is copied from there.
L="$PKG/licenses"; S="$WORK/src"; M="$TC/$HOST/share/mingw32"
cp "$S/ffmpeg/COPYING.LGPLv2.1" "$PKG/LICENSE.txt"
cp "$S/ffmpeg/COPYING.LGPLv2.1" "$L/FFmpeg-COPYING.LGPLv2.1.txt"; cp "$S/ffmpeg/LICENSE.md" "$L/FFmpeg-LICENSE.md"
cp "$S/zlib/LICENSE" "$L/zlib-LICENSE.txt"
cp "$S/dav1d/COPYING" "$L/dav1d-COPYING.txt"; cp "$S/dav1d/doc/PATENTS" "$L/dav1d-PATENTS.txt"
cp "$S/opus/COPYING" "$L/opus-COPYING.txt"
cp "$S/lame/COPYING" "$L/LAME-COPYING.txt"; cp "$S/lame/LICENSE" "$L/LAME-LICENSE.txt"
cp "$S/openh264/LICENSE" "$L/OpenH264-LICENSE.txt"
cp "$S/kvazaar/LICENSE" "$L/Kvazaar-LICENSE.txt"
cp "$S/svtav1/LICENSE.md" "$L/SVT-AV1-LICENSE.md"; cp "$S/svtav1/PATENTS.md" "$L/SVT-AV1-PATENTS.md"
cp "$S/libvpx/LICENSE" "$L/libvpx-LICENSE.txt"; cp "$S/libvpx/PATENTS" "$L/libvpx-PATENTS.txt"; cp "$S/libvpx/AUTHORS" "$L/libvpx-AUTHORS.txt"
cp "$S/libvpl/LICENSE" "$L/libvpl-LICENSE.txt"
sed -n '/^ \* Copyright/,/^ \*\//p' "$S/ffnvcodec/include/ffnvcodec/nvEncodeAPI.h" > "$L/nv-codec-headers-LICENSE.txt"
sed -n '/^\/\/ Copyright/,/^\/\/ THE SOFTWARE\./p' "$S/amf/AMF/core/Factory.h" > "$L/AMF-LICENSE.txt"
cp "$M/COPYING.MinGW-w64-runtime.txt" "$L/mingw-w64-runtime-COPYING.txt"
cp "$M/COPYING.winpthreads.txt" "$L/winpthreads-COPYING.txt"
cp "$TC/LICENSE.TXT" "$L/LLVM-runtime-LICENSE.txt"
for f in "$L"/*; do [[ -s $f ]] || { echo "Empty license file: $f" >&2; exit 1; }; done

# The recipe: this folder as it was used, so the source archives and these instructions together are
# the complete corresponding source.
( cd "$(dirname "$RECIPE_DIR")" && find "$(basename "$RECIPE_DIR")" -type f | LC_ALL=C sort |
    tar --no-recursion --mode=a=r,u+w --mtime=@0 --owner=0 --group=0 --numeric-owner -cf - -T - ) |
    gzip -9n > "$OUT/source/syna-ffmpeg-recipe-$RECIPE.tar.gz"

{
    echo "Syna FFmpeg build, recipe $RECIPE (scripts/ffmpeg/build.sh)"
    echo
    echo "FFmpeg $(sed -n 's/^#define FFMPEG_VERSION "\(.*\)"$/\1/p' "$S/ffmpeg/libavutil/ffversion.h")"
    echo "Toolchain: ${TOOLCHAIN[1]} (SHA-256 ${TOOLCHAIN[3]}), $("$CC" --version | head -1)"
    echo "Build tools: $(nasm -v), $(cmake --version | head -1), meson $(meson --version), ninja $(ninja --version)"
    echo "$GPL_CHECK"
    echo
    echo "Configuration:"
    sed -n 's/^#define FFMPEG_CONFIGURATION "\(.*\)"$/\1/p' "$S/ffmpeg/config.h"
    echo
    echo "Sources (SHA-256):"
    for spec in "${SOURCES[@]}"; do read -r _ file url sha <<<"$spec"; echo "    $sha  $file  $url"; done
} > "$PKG/BUILDINFO.txt"

# Fixed file times and order, so the same build gives the same archive.
find "$PKG" -exec touch -h -d 2000-01-01T00:00:00Z {} +
( cd "$PKG" && find . -type f | LC_ALL=C sort | TZ=UTC zip -X -q -D "$OUT/ffmpeg-syna-$RECIPE-win64.zip" -@ )
( cd "$OUT/source" && sha256sum -- *.tar.* > ffmpeg-sources.sha256 )

log "Done"
ls -l "$OUT/ffmpeg-syna-$RECIPE-win64.zip"
cat "$OUT/source/ffmpeg-sources.sha256"
