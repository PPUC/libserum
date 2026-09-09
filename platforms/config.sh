#!/bin/bash

set -e

# Pinned revision of https://github.com/PPUC/libframeutil.
# libserum consumes the header-only FrameUtil.h from it for the shared frame
# scaling algorithms (see AGENTS.md, "Resolution scaling").
LIBFRAMEUTIL_SHA=c2e1a9b2fe583f17f975cb82bae33aa5f3f0368b

PROJECT_SOURCE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)"

# Resolve an optional <DEP>_SOURCE_DIR override to an absolute path.
# Set e.g. LIBFRAMEUTIL_SOURCE_DIR=../libframeutil to build against a local
# checkout instead of the pinned archive. Paths are relative to the repository
# root.
dependency_source_dir() {
   local var_name="$1"
   local source_dir="${!var_name:-}"

   if [ -z "${source_dir}" ]; then
      return 0
   fi

   (cd "${PROJECT_SOURCE_ROOT}" && cd "${source_dir}" && pwd -P)
}

print_dependency_source() {
   local label="$1"
   local sha="$2"
   local source_var="$3"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "  ${label}_SOURCE_DIR: ${source_dir}"
   else
      echo "  ${label}_SOURCE: archive ${sha}"
   fi
}

prepare_dependency_source() {
   local name="$1"
   local sha="$2"
   local url="$3"
   local archive_type="${4:-tar}"
   local source_var="$5"
   local source_dir

   source_dir="$(dependency_source_dir "${source_var}")"
   if [ -n "${source_dir}" ]; then
      echo "Using ${source_var}: ${source_dir}"
      ln -s "${source_dir}" "${name}"
   elif [ "${archive_type}" = "zip" ]; then
      curl -sL "${url}" -o "${name}.zip"
      unzip "${name}.zip"
      mv "${name}-${sha}" "${name}"
   else
      curl -sL "${url}" -o "${name}-${sha}.tar.gz"
      tar xzf "${name}-${sha}.tar.gz"
      mv "${name}-${sha}" "${name}"
   fi
}

if [ -z "${BUILD_TYPE}" ]; then
   BUILD_TYPE="Release"
fi
