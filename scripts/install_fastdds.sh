#!/usr/bin/env bash
set -euo pipefail

PREFIX=${1:-/opt/fastdds}
FAST_DDS_VERSION=${FAST_DDS_VERSION:-v2.14.6}
FAST_CDR_VERSION=${FAST_CDR_VERSION:-v2.2.7}
FOONATHAN_VERSION=${FOONATHAN_VERSION:-v1.3.1}
FAST_DDS_GEN_VERSION=${FAST_DDS_GEN_VERSION:-v3.3.2}
JOBS=${JOBS:-$(nproc)}

if [[ -f "$PREFIX/include/fastdds/dds/domain/DomainParticipant.hpp" && -x "$PREFIX/bin/fastddsgen" ]]; then
    echo "Fast DDS already installed at $PREFIX"
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$PREFIX"

build_cmake() {
    local source=$1 build=$2
    shift 2
    cmake -S "$source" -B "$build" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX="$PREFIX" \
        "$@"
    cmake --build "$build" -j"$JOBS"
    cmake --install "$build"
}

cd "$work"
git clone --depth 1 --branch "$FOONATHAN_VERSION" https://github.com/eProsima/foonathan_memory_vendor.git
build_cmake foonathan_memory_vendor foonathan-build -DBUILD_SHARED_LIBS=ON

git clone --depth 1 --branch "$FAST_CDR_VERSION" https://github.com/eProsima/Fast-CDR.git
build_cmake Fast-CDR fastcdr-build -DCMAKE_PREFIX_PATH="$PREFIX"

git clone --depth 1 --branch "$FAST_DDS_VERSION" https://github.com/eProsima/Fast-DDS.git
build_cmake Fast-DDS fastdds-build \
    -DCMAKE_PREFIX_PATH="$PREFIX" \
    -DSECURITY=ON \
    -DBUILD_SHARED_LIBS=ON \
    -DCOMPILE_EXAMPLES=OFF

git clone --depth 1 --branch "$FAST_DDS_GEN_VERSION" --recursive \
    https://github.com/eProsima/Fast-DDS-Gen.git
(
    cd Fast-DDS-Gen
    ./gradlew install --no-daemon --install_path="$PREFIX"
)

echo "Installed Fast DDS $FAST_DDS_VERSION with SECURITY=ON at $PREFIX"
