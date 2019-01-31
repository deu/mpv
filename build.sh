#!/bin/bash

### Common functions

pushd () {
    command pushd "$@" > /dev/null
}

popd () {
    command popd "$@" > /dev/null
}

choice () {
    echo ""
    read -e -n 1 -p "$@ (y/n) [n]: " answer
    [[ "${answer:0:1}" == [Yy] ]]
}

###

pushd "$(dirname "$0")"

echo "----------------------"
echo "deu's mpv build system"
echo "----------------------"

pushd "external"
BUILD_LIBS="$(pwd)/build_libs"

if choice "Build ffmpeg?"; then
    pushd "ffmpeg"
    if choice "--- Update ffmpeg?"; then
        git pull
    fi
    if choice "--- Reconfigure ffmpeg?"; then
        make distclean
        ./configure \
            --prefix="${BUILD_LIBS}" \
            --enable-static \
            --disable-shared \
            --enable-gpl \
            --disable-debug \
            --disable-doc \
            --enable-gnutls
    fi
    make
    make install
    popd # ffmpeg
fi

if choice "Build libass?"; then
    pushd "libass"
    if choice "--- Update libass?"; then
        git pull
    fi
    if choice "--- Reconfigure libass?"; then
        make distclean
        ./autogen.sh \
            --prefix="${BUILD_LIBS}" \
            --libdir="${BUILD_LIBS}/lib" \
            --enable-static \
            --disable-shared
        ./configure \
            --prefix="${BUILD_LIBS}" \
            --libdir="${BUILD_LIBS}/lib" \
            --enable-static \
            --disable-shared
    fi
    make
    make install
    popd # libass
fi

if choice "Build fribidi?"; then
    pushd "fribidi"
    if choice "--- Update fribidi?"; then
        git pull
    fi
    if choice "--- Reconfigure fribidi?"; then
        ./autogen.sh
        make distclean
        ./bootstrap
        ./configure \
            --prefix="${BUILD_LIBS}" \
            --libdir="${BUILD_LIBS}/lib" \
            --enable-static \
            --disable-shared \
            --without-glib
    fi
    make
    make install
    popd # fribidi
fi

popd # external

if choice "Configure mpv?"; then
    PKG_CONFIG_PATH="${BUILD_LIBS}/lib/pkgconfig" ./waf configure
fi

if choice "Build mpv?"; then
    ./waf build
fi

popd # ORIGINAL DIR
