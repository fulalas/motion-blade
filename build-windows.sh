#!/bin/bash -e

root=$(cd "$(dirname "$0")" && pwd)
src_dir=$("$root/tools/mpv-src.sh")
[ -d "$root/../deps/bin" ] && PATH="$root/../deps/bin:$PATH"
work=$(readlink -f "${1:-$root/winbuild}")
TARGET=x86_64-w64-mingw32
toolchain=$(ls -d "$work"/llvm-mingw-*-ucrt-*/ 2>/dev/null | head -1)
[ -n "$toolchain" ] || { echo "no llvm-mingw-*-ucrt-* folder in $work" >&2; exit 1; }
export PATH="$toolchain/bin:$PATH"
prefix="$work/prefix"
mkdir -p "$prefix" "$work/src"
ln -snf . "$prefix/usr"
ln -snf . "$prefix/local"
cd "$work/src"

export CC=$TARGET-clang CXX=$TARGET-clang++ AR=$TARGET-ar RANLIB=$TARGET-ranlib
export CFLAGS="-O2 -pipe" CXXFLAGS="-O2 -pipe" LDFLAGS=""
export PKG_CONFIG_SYSROOT_DIR="$prefix"
export PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
jobs=$(nproc)

cat >"$prefix/crossfile" <<EOF
[built-in options]
buildtype = 'release'
default_library = 'static'
prefer_static = true
wrap_mode = 'nodownload'
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$TARGET-strip'
windres = '$TARGET-windres'
dlltool = '$TARGET-dlltool'
pkg-config = ['pkg-config', '--static']
nasm = 'nasm'
[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

cmake_args=(
    -Wno-dev -GNinja
    -DCMAKE_SYSTEM_NAME=Windows -DCMAKE_SYSTEM_PROCESSOR=x86_64
    -DCMAKE_C_COMPILER=$CC -DCMAKE_CXX_COMPILER=$CXX
    -DCMAKE_RC_COMPILER=$TARGET-windres
    -DCMAKE_FIND_ROOT_PATH="$prefix" -DCMAKE_BUILD_TYPE=Release
    -DCMAKE_INSTALL_PREFIX=/ -DBUILD_SHARED_LIBS=OFF
)

builddir() {
    rm -rf "$1/builddir"
    mkdir -p "$1/builddir"
    pushd "$1/builddir" >/dev/null
}

install_build() {
    if [ -f build.ninja ]; then
        ninja
        DESTDIR="$prefix" ninja install
    else
        make -j$jobs
        make DESTDIR="$prefix" install
    fi
}

gettar() {
    local name="${1##*/}"
    [ -d "${name%.tar.*}" ] && return 0
    curl -sfL -o "$name" "$1"
    echo "$2  $name" | sha256sum -c --quiet -
    tar -xaf "$name"
    rm "$name"
}

gitpin() {
    [ -d "$2" ] && return 0
    git init -q "$2"
    git -C "$2" fetch -q --depth=1 "$1" "$3"
    git -C "$2" checkout -q FETCH_HEAD
    git -C "$2" submodule update --init --recursive --depth=1
}

meson_build() {
    builddir "$1"
    meson setup .. --cross-file "$prefix/crossfile" --prefix=/ --libdir=lib "${@:2}"
    install_build
    popd >/dev/null
}

done_mark() { [ -e "$prefix/.done-$1" ]; }
mark() { touch "$prefix/.done-$1"; }

write_pc() {
    mkdir -p "$prefix/lib/pkgconfig"
    printf 'prefix=/\nlibdir=${prefix}/lib\nincludedir=${prefix}/include%s\nName: %s\nDescription: %s\nVersion: %s\nLibs: -L${libdir} %s\nCflags: -I${includedir}\n' \
        "$6" "$2" "$3" "$4" "$5" >"$prefix/lib/pkgconfig/$1.pc"
}

_zlib() {
    gettar https://zlib.net/fossils/zlib-1.3.1.tar.gz 9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    pushd zlib-1.3.1 >/dev/null
    make -fwin32/Makefile.gcc clean
    make -fwin32/Makefile.gcc PREFIX=$TARGET- CC=$CC libz.a -j$jobs
    install -Dm644 libz.a "$prefix/lib/libz.a"
    install -Dm644 zlib.h "$prefix/include/zlib.h"
    install -Dm644 zconf.h "$prefix/include/zconf.h"
    write_pc zlib zlib zlib 1.3.1 -lz
    popd >/dev/null
}

_iconv() {
    gettar https://ftpmirror.gnu.org/gnu/libiconv/libiconv-1.18.tar.gz 3b08f5f4f9b4eb82f151a7040bfd6fe6c6fb922efe4b1659c66ea933276965e8
    builddir libiconv-1.18
    ../configure --host=$TARGET --prefix=/ --disable-shared --enable-static --disable-nls
    install_build
    popd >/dev/null
    write_pc iconv iconv iconv 1.18 -liconv
}

_vulkan() {
    gitpin https://github.com/KhronosGroup/Vulkan-Headers Vulkan-Headers acbb9391c0a597272800cd3f4363661c00a27c77
    builddir Vulkan-Headers
    cmake .. "${cmake_args[@]}"
    install_build
    popd >/dev/null
    gitpin https://github.com/KhronosGroup/Vulkan-Loader Vulkan-Loader 6dc4f59df7af059f3a56a5df1704ab0130d33b95
    builddir Vulkan-Loader
    cmake .. "${cmake_args[@]}" -DBUILD_SHARED_LIBS=ON -DUSE_GAS=ON -DBUILD_TESTS=OFF \
        -DVULKAN_HEADERS_INSTALL_DIR="$prefix"
    install_build
    popd >/dev/null
}

_shaderc() {
    if [ ! -d shaderc ]; then
        gitpin https://github.com/google/shaderc.git shaderc 5bcaaaec711df4dfa31f38923d3d42b9b98d5250
        (cd shaderc && ./utils/git-sync-deps)
    fi
    builddir shaderc
    cmake .. "${cmake_args[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF
    install_build
    popd >/dev/null
    local v=$(sed -n 's/^Version: //p' "$prefix/lib/pkgconfig/shaderc_combined.pc")
    write_pc shaderc shaderc shaderc "$v" "-lshaderc_combined -lc++"
}

_spirv_cross() {
    gitpin https://github.com/KhronosGroup/SPIRV-Cross SPIRV-Cross 300681bbcb8365a8d1eacdacdbdd0249bbf1d6b1
    builddir SPIRV-Cross
    cmake .. "${cmake_args[@]}" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
        -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
    install_build
    popd >/dev/null
    local v=$(sed -n 's/^Version: //p' "$prefix/lib/pkgconfig/spirv-cross-c.pc")
    write_pc spirv-cross-c-shared spirv-cross-c-shared "SPIRV-Cross (static)" "$v" \
        "-lspirv-cross-c -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-cpp -lspirv-cross-core -lc++" /spirv_cross
}

_dav1d() {
    gitpin https://github.com/videolan/dav1d.git dav1d 9433da2daa61bf7029a52b84bafa5daf59cdcc9d
    meson_build dav1d -Denable_{tools,tests}=false
}

_lcms2() {
    gitpin https://github.com/mm2/Little-CMS.git lcms2 044b7bbc59383517854a797ef4d8291809d2ccd8
    meson_build lcms2 -Dtests=disabled -D{utils,versionedlibs}=false
}

_ffmpeg() {
    gitpin https://github.com/FFmpeg/FFmpeg.git ffmpeg 847acdff187e6f2f1b725644ac9eabdea7aed9f2
    builddir ffmpeg
    ../configure --prefix=/ --target-os=mingw32 --arch=x86_64 --enable-cross-compile \
        --cross-prefix=$TARGET- --cc=$CC --cxx=$CXX --pkg-config=pkg-config --pkg-config-flags=--static \
        --enable-gpl --disable-shared --enable-static --disable-{doc,programs,debug} \
        --enable-libdav1d --enable-zlib --enable-d3d11va --enable-dxva2 --enable-vulkan \
        --enable-muxer=spdif --enable-encoder=mjpeg,png
    install_build
    popd >/dev/null
}

_libplacebo() {
    gitpin https://github.com/haasn/libplacebo.git libplacebo 285d1cc8af2ab79b72ac662eb09736de8c12392b
    meson_build libplacebo -Ddemos=false -Dtests=false -Dopengl=disabled \
        -D{vulkan,d3d11,lcms,shaderc}=enabled -Dglslang=disabled
}

_freetype() {
    gettar https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz 32427e8c471ac095853212a37aef816c60b42052d4d9e48230bab3bdf2936ccc
    meson_build freetype-2.14.1 -D{harfbuzz,png,bzip2,brotli}=disabled -Dzlib=system
}

_fribidi() {
    gettar https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz 1b1cde5b235d40479e91be2f0e88a309e3214c8ab470ec8a2744d82a5a9ea05c
    meson_build fribidi-1.0.16 -D{tests,docs,bin}=false
}

_harfbuzz() {
    gettar https://github.com/harfbuzz/harfbuzz/releases/download/12.2.0/harfbuzz-12.2.0.tar.xz ecb603aa426a8b24665718667bda64a84c1504db7454ee4cadbd362eea64e545
    meson_build harfbuzz-12.2.0 -D{tests,docs,utilities,glib,gobject,cairo,icu}=disabled -Dfreetype=enabled
}

_libass() {
    gitpin https://github.com/libass/libass.git libass f61db567e6593df3470e91594bcd4ad2d0473aff
    meson_build libass -Dfontconfig=disabled -Ddirectwrite=enabled -Dasm=enabled
}

_luajit() {
    gitpin https://github.com/LuaJIT/LuaJIT.git LuaJIT a4c064539ea931274d407e1249b4bb575424a0f4
    pushd LuaJIT >/dev/null
    make TARGET_SYS=Windows clean
    make TARGET_SYS=Windows HOST_CC="cc" CROSS="$TARGET-" STATIC_CC="$CC" DYNAMIC_CC="$CC" \
        TARGET_LD="$CC" TARGET_AR="$AR rcus" TARGET_STRIP="$TARGET-strip" BUILDMODE=static amalg -j$jobs
    make DESTDIR="$prefix" PREFIX=/ INSTALL_DEP= FILE_T=luajit.exe install
    popd >/dev/null
    sed -i 's/^Libs.private:.*/Libs.private:/' "$prefix/lib/pkgconfig/luajit.pc"
}

for x in zlib iconv vulkan shaderc spirv_cross dav1d lcms2 ffmpeg libplacebo freetype fribidi harfbuzz libass luajit; do
    done_mark $x && continue
    echo "=== building $x"
    _$x
    mark $x
done

echo "=== building mpv"
cd "$src_dir"
build="$work/mpv-build"
[ -f "$build/build.ninja" ] || meson setup "$build" --cross-file "$prefix/crossfile" \
    -D{c,cpp}_args="-I$prefix/include" -D{c,cpp}_link_args="-L$prefix/lib -static -liconv" \
    -Dlibmpv=false -Dtests=false -Dlua=luajit -Djavascript=disabled \
    -D{d3d11,vulkan,shaderc,spirv-cross,iconv,lcms2,zlib}=enabled \
    -D{uchardet,libarchive,rubberband,zimg,cplugins}=disabled
meson compile -C "$build"

out="$work/motionblade-windows-x86_64"
rm -rf "$out" "$out.zip"
mkdir -p "$out/portable_config"
cp "$build/mpv.exe" "$build/mpv.com" "$out/"
$TARGET-strip "$out/mpv.exe" "$out/mpv.com"
cat >"$out/portable_config/mpv.conf" <<EOF
vf=motionblade
gpu-api=vulkan
hwdec=vulkan,auto
EOF
cp "$root/README.md" "$root/VERSION" "$root/LICENSE" "$out/"
printf '@echo off\r\nstart "" "%%~dp0mpv.exe" %%*\r\n' >"$out/run.bat"
(cd "$work" && zip -qr9 "$out.zip" "$(basename "$out")")
echo "=== done: $out.zip"
