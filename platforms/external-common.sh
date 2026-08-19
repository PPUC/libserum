#!/bin/bash

# Fetch libserum's external dependencies into third-party/include.
#
# libserum has no compiled external dependencies; everything it needs is
# header-only. This script therefore only downloads headers and copies them
# into place, and is identical for every platform/arch. The per-platform
# platforms/<platform>/<arch>/external.sh wrappers just call it, matching the
# platforms/build-common.sh pattern.

set -e

source ./platforms/config.sh

echo "Fetching external headers..."
echo "  LIBFRAMEUTIL_SHA: ${LIBFRAMEUTIL_SHA}"
print_dependency_source LIBFRAMEUTIL "${LIBFRAMEUTIL_SHA}" LIBFRAMEUTIL_SOURCE_DIR
echo ""

rm -rf external
mkdir -p external third-party/include
cd external

prepare_dependency_source libframeutil "${LIBFRAMEUTIL_SHA}" \
   "https://github.com/PPUC/libframeutil/archive/${LIBFRAMEUTIL_SHA}.tar.gz" tar LIBFRAMEUTIL_SOURCE_DIR
cp libframeutil/include/FrameUtil.h "${PROJECT_SOURCE_ROOT}/third-party/include/"

cd ..

echo "External headers ready in third-party/include."
