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
gitclone="git clone --depth=1 --recursive --shallow-submodules"
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
    tar -xaf "$name"
    rm "$name"
}

meson_build() {
    builddir "$1"
    meson setup .. --cross-file "$prefix/crossfile" --prefix=/ --libdir=lib "${@:2}"
    install_build
    popd >/dev/null
}

done_mark() { [ -e "$prefix/.done-$1" ]; }
mark() { touch "$prefix/.done-$1"; }

_zlib() {
    gettar https://zlib.net/fossils/zlib-1.3.1.tar.gz
    pushd zlib-1.3.1 >/dev/null
    make -fwin32/Makefile.gcc clean
    make -fwin32/Makefile.gcc PREFIX=$TARGET- CC=$CC libz.a -j$jobs
    install -Dm644 libz.a "$prefix/lib/libz.a"
    install -Dm644 zlib.h "$prefix/include/zlib.h"
    install -Dm644 zconf.h "$prefix/include/zconf.h"
    mkdir -p "$prefix/lib/pkgconfig"
    printf 'prefix=/\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: zlib\nDescription: zlib\nVersion: 1.3.1\nLibs: -L${libdir} -lz\nCflags: -I${includedir}\n' \
        >"$prefix/lib/pkgconfig/zlib.pc"
    popd >/dev/null
}

_iconv() {
    gettar https://ftpmirror.gnu.org/gnu/libiconv/libiconv-1.18.tar.gz
    builddir libiconv-1.18
    ../configure --host=$TARGET --prefix=/ --disable-shared --enable-static --disable-nls
    install_build
    popd >/dev/null
    mkdir -p "$prefix/lib/pkgconfig"
    printf 'prefix=/\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: iconv\nDescription: iconv\nVersion: 1.18\nLibs: -L${libdir} -liconv\nCflags: -I${includedir}\n' \
        >"$prefix/lib/pkgconfig/iconv.pc"
}

_vulkan() {
    [ -d Vulkan-Headers ] || $gitclone https://github.com/KhronosGroup/Vulkan-Headers
    builddir Vulkan-Headers
    cmake .. "${cmake_args[@]}"
    install_build
    popd >/dev/null
    [ -d Vulkan-Loader ] || $gitclone https://github.com/KhronosGroup/Vulkan-Loader
    builddir Vulkan-Loader
    cmake .. "${cmake_args[@]}" -DBUILD_SHARED_LIBS=ON -DUSE_GAS=ON -DBUILD_TESTS=OFF \
        -DVULKAN_HEADERS_INSTALL_DIR="$prefix"
    install_build
    popd >/dev/null
}

_shaderc() {
    if [ ! -d shaderc ]; then
        $gitclone https://github.com/google/shaderc.git
        (cd shaderc && ./utils/git-sync-deps)
    fi
    builddir shaderc
    cmake .. "${cmake_args[@]}" -DSHADERC_SKIP_TESTS=ON -DSHADERC_SKIP_EXAMPLES=ON \
        -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_EXECUTABLES=ON -DENABLE_GLSLANG_BINARIES=OFF
    install_build
    popd >/dev/null
    sed 's/-lshaderc_combined/-lshaderc_combined -lc++/' "$prefix/lib/pkgconfig/shaderc_combined.pc" \
        | sed 's/^Name:.*/Name: shaderc/' >"$prefix/lib/pkgconfig/shaderc.pc"
}

_spirv_cross() {
    [ -d SPIRV-Cross ] || $gitclone https://github.com/KhronosGroup/SPIRV-Cross
    builddir SPIRV-Cross
    cmake .. "${cmake_args[@]}" -DSPIRV_CROSS_SHARED=OFF -DSPIRV_CROSS_STATIC=ON \
        -DSPIRV_CROSS_CLI=OFF -DSPIRV_CROSS_ENABLE_TESTS=OFF
    install_build
    popd >/dev/null
    local v=$(sed -n 's/^Version: //p' "$prefix/lib/pkgconfig/spirv-cross-c.pc")
    printf 'prefix=/\nlibdir=${prefix}/lib\nincludedir=${prefix}/include/spirv_cross\nName: spirv-cross-c-shared\nDescription: SPIRV-Cross (static)\nVersion: %s\nLibs: -L${libdir} -lspirv-cross-c -lspirv-cross-hlsl -lspirv-cross-msl -lspirv-cross-glsl -lspirv-cross-reflect -lspirv-cross-util -lspirv-cross-cpp -lspirv-cross-core -lc++\nCflags: -I${includedir}\n' "$v" \
        >"$prefix/lib/pkgconfig/spirv-cross-c-shared.pc"
}

_dav1d() {
    [ -d dav1d ] || $gitclone https://github.com/videolan/dav1d.git
    meson_build dav1d -Denable_{tools,tests}=false
}

_lcms2() {
    [ -d lcms2 ] || $gitclone https://github.com/mm2/Little-CMS.git lcms2
    meson_build lcms2 -Dtests=disabled -D{utils,versionedlibs}=false
}

_ffmpeg() {
    [ -d ffmpeg ] || $gitclone https://github.com/FFmpeg/FFmpeg.git ffmpeg
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
    [ -d libplacebo ] || $gitclone https://github.com/haasn/libplacebo.git
    meson_build libplacebo -Ddemos=false -Dtests=false -Dopengl=disabled \
        -D{vulkan,d3d11,lcms,shaderc}=enabled -Dglslang=disabled
}

_freetype() {
    gettar https://download.savannah.gnu.org/releases/freetype/freetype-2.14.1.tar.xz
    meson_build freetype-2.14.1 -D{harfbuzz,png,bzip2,brotli}=disabled -Dzlib=system
}

_fribidi() {
    gettar https://github.com/fribidi/fribidi/releases/download/v1.0.16/fribidi-1.0.16.tar.xz
    meson_build fribidi-1.0.16 -D{tests,docs,bin}=false
}

_harfbuzz() {
    gettar https://github.com/harfbuzz/harfbuzz/releases/download/12.2.0/harfbuzz-12.2.0.tar.xz
    meson_build harfbuzz-12.2.0 -D{tests,docs,utilities,glib,gobject,cairo,icu}=disabled -Dfreetype=enabled
}

_libass() {
    [ -d libass ] || $gitclone https://github.com/libass/libass.git
    meson_build libass -Dfontconfig=disabled -Ddirectwrite=enabled -Dasm=enabled
}

_luajit() {
    [ -d LuaJIT ] || $gitclone https://github.com/LuaJIT/LuaJIT.git
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
rm -rf "$build"
meson setup "$build" --cross-file "$prefix/crossfile" \
    -D{c,cpp}_args="-I$prefix/include" -D{c,cpp}_link_args="-L$prefix/lib -static -liconv" \
    -Dlibmpv=false -Dtests=false -Dlua=luajit -Djavascript=disabled \
    -D{d3d11,vulkan,shaderc,spirv-cross,iconv,lcms2,zlib}=enabled \
    -D{uchardet,libarchive,rubberband,zimg,cplugins}=disabled
meson compile -C "$build"

out="$work/mpv-memc-windows-x86_64"
rm -rf "$out" "$out.zip"
mkdir -p "$out/portable_config"
cp "$build/mpv.exe" "$build/mpv.com" "$out/"
$TARGET-strip "$out/mpv.exe" "$out/mpv.com"
cat >"$out/portable_config/mpv.conf" <<EOF
vf=memc
hwdec=auto-copy
video-sync=display-resample
EOF
cp "$root/README.md" "$root/MEMC_VERSION" "$out/"
printf '@echo off\r\nstart "" "%%~dp0mpv.exe" --vf=memc --hwdec=auto-copy --video-sync=display-resample %%*\r\n' >"$out/run.bat"
(cd "$work" && zip -qr9 "$out.zip" "$(basename "$out")")
echo "=== done: $out.zip"
