#!/bin/bash

set -e

if [ "$#" -lt 2 ]; then
   echo "Usage: $0 <platform> <arch> [extra-cmake-args...]"
   exit 1
fi

PLATFORM_NAME="$1"
ARCH_NAME="$2"
shift 2

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi

EXTERNAL_SCRIPT="./platforms/${PLATFORM_NAME}/${ARCH_NAME}/external.sh"
if [ -f "${EXTERNAL_SCRIPT}" ]; then
   BUILD_TYPE=${BUILD_TYPE} "${EXTERNAL_SCRIPT}"
fi

if [ "${PLATFORM_NAME}" = "win" ]; then
   if [ "${ARCH_NAME}" = "x86" ]; then
      cmake -G "Visual Studio 17 2022" -A Win32 -DPLATFORM=${PLATFORM_NAME} -DARCH=${ARCH_NAME} "$@" -B build
   else
      cmake -G "Visual Studio 17 2022" -DPLATFORM=${PLATFORM_NAME} -DARCH=${ARCH_NAME} "$@" -B build
   fi
   cmake --build build --config ${BUILD_TYPE}
elif [ "${PLATFORM_NAME}" = "win-mingw" ]; then
   cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} -DPLATFORM=${PLATFORM_NAME} -DARCH=${ARCH_NAME} "$@" -B build
   cmake --build build -- -j$(nproc)
else
   cmake -DPLATFORM=${PLATFORM_NAME} -DARCH=${ARCH_NAME} -DCMAKE_BUILD_TYPE=${BUILD_TYPE} "$@" -B build
   cmake --build build
fi
