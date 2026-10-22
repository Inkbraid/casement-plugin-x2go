#!/usr/bin/env bash
# Builds third-party libraries for 64-bit Windows with MinGW-w64, as static
# libraries in a prefix, for the cross builds on Linux (CI and local):
#
#   ci/mingw-deps.sh <prefix> zlib openssl ...
#
# Each library is built once: a prefix that already has it is left alone.
# Sources are downloaded from their projects and checked against pinned
# SHA-256 sums.
set -euo pipefail

PREFIX=$(realpath -m "$1")
shift
HOST=x86_64-w64-mingw32
JOBS=$(nproc)
WORK=${MINGW_DEPS_WORK:-$(mktemp -d)}
mkdir -p "$PREFIX" "$WORK"

fetch() {  # url sha256 file
    local url=$1 sum=$2 file=$WORK/$3
    if [ ! -f "$file" ]; then curl -sSLf --retry 3 -o "$file" "$url"; fi
    echo "$sum  $file" | sha256sum -c --quiet -
}

have() { [ -f "$PREFIX/.built-$1" ]; }
done_() { touch "$PREFIX/.built-$1"; }

build_zlib() {
    have zlib && return
    fetch https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz \
        9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23 zlib.tar.gz
    rm -rf "$WORK/zlib" && mkdir -p "$WORK/zlib" && tar -xzf "$WORK/zlib.tar.gz" -C "$WORK/zlib" --strip-components=1
    (cd "$WORK/zlib" && make -f win32/Makefile.gcc PREFIX=$HOST- -j"$JOBS" libz.a &&
        make -f win32/Makefile.gcc PREFIX=$HOST- \
            INCLUDE_PATH="$PREFIX/include" LIBRARY_PATH="$PREFIX/lib" BINARY_PATH="$PREFIX/bin" install)
    done_ zlib
}

build_openssl() {
    have openssl && return
    fetch https://github.com/openssl/openssl/releases/download/openssl-3.5.4/openssl-3.5.4.tar.gz \
        967311f84955316969bdb1d8d4b983718ef42338639c621ec4c34fddef355e99 openssl.tar.gz
    rm -rf "$WORK/openssl" && mkdir -p "$WORK/openssl" && tar -xzf "$WORK/openssl.tar.gz" -C "$WORK/openssl" --strip-components=1
    (cd "$WORK/openssl" &&
        ./Configure mingw64 --cross-compile-prefix=$HOST- --prefix="$PREFIX" --libdir=lib \
            no-shared no-tests no-docs no-apps no-module &&
        make -j"$JOBS" build_libs >/dev/null && make install_dev >/dev/null)
    rm -rf "$WORK/openssl"
    done_ openssl
}

# libssh2 with the OpenSSL backend and zlib, both from this prefix. Needs
# zlib and openssl first; configured with the repository's toolchain file,
# which adds CMAKE_PREFIX_PATH to the find roots.
build_libssh2() {
    have libssh2 && return
    fetch https://github.com/libssh2/libssh2/releases/download/libssh2-1.11.1/libssh2-1.11.1.tar.xz \
        9954cb54c4f548198a7cbebad248bdc87dd64bd26185708a294b2b50771e3769 libssh2.tar.xz
    rm -rf "$WORK/libssh2" && mkdir -p "$WORK/libssh2" && tar -xJf "$WORK/libssh2.tar.xz" -C "$WORK/libssh2" --strip-components=1
    local toolchain
    toolchain=$(realpath "$(dirname "${BASH_SOURCE[0]}")/../cmake/mingw-w64.cmake")
    cmake -S "$WORK/libssh2" -B "$WORK/libssh2/build" -DCMAKE_TOOLCHAIN_FILE="$toolchain" \
        -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DCRYPTO_BACKEND=OpenSSL -DOPENSSL_USE_STATIC_LIBS=ON \
        -DENABLE_ZLIB_COMPRESSION=ON -DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF >/dev/null
    cmake --build "$WORK/libssh2/build" -j"$JOBS" >/dev/null
    cmake --install "$WORK/libssh2/build" >/dev/null
    rm -rf "$WORK/libssh2"
    done_ libssh2
}

for lib in "$@"; do
    "build_$lib"
done
