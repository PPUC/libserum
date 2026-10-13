cmake_minimum_required(VERSION 3.25)

set(PLATFORM "win" CACHE STRING "Platform")
set(ARCH "x64" CACHE STRING "Arch")

option(BUILD_SHARED "Option to build shared library" ON)
option(BUILD_STATIC "Option to build static library" ON)
option(ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan for Debug builds" OFF)
set(SERUM_ACCEL_DIR "" CACHE PATH "Directory whose serum-accel.cmake names the sources to build instead of src/serum-accel-ref.cpp")

message(STATUS "PLATFORM: ${PLATFORM}")
message(STATUS "ARCH: ${ARCH}")

message(STATUS "BUILD_SHARED: ${BUILD_SHARED}")
message(STATUS "BUILD_STATIC: ${BUILD_STATIC}")
message(STATUS "ENABLE_SANITIZERS: ${ENABLE_SANITIZERS}")
message(STATUS "SERUM_ACCEL_DIR: ${SERUM_ACCEL_DIR}")

if(PLATFORM STREQUAL "ios" OR PLATFORM STREQUAL "ios-simulator")
   set(CMAKE_SYSTEM_NAME iOS)
   if (PLATFORM STREQUAL "ios-simulator")
      set(CMAKE_OSX_SYSROOT iphonesimulator)
   endif()
   set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
elseif(PLATFORM STREQUAL "tvos")
   set(CMAKE_SYSTEM_NAME tvOS)
   set(CMAKE_OSX_DEPLOYMENT_TARGET 17.0)
elseif(PLATFORM STREQUAL "android")
   set(CMAKE_SYSTEM_NAME Android)
   set(CMAKE_SYSTEM_VERSION 30)
   set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
endif()


file(READ src/serum-version.h version)
string(REGEX MATCH "#[ \t]*define[ \t]+SERUM_VERSION_MAJOR[ \t]+([0-9]+)" _tmp "${version}")
if("${CMAKE_MATCH_1}" STREQUAL "")
   message(FATAL_ERROR "Failed to parse SERUM_VERSION_MAJOR from src/serum-version.h")
endif()
set(VERSION_MAJOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "#[ \t]*define[ \t]+SERUM_VERSION_MINOR[ \t]+([0-9]+)" _tmp "${version}")
if("${CMAKE_MATCH_1}" STREQUAL "")
   message(FATAL_ERROR "Failed to parse SERUM_VERSION_MINOR from src/serum-version.h")
endif()
set(VERSION_MINOR "${CMAKE_MATCH_1}")
string(REGEX MATCH "#[ \t]*define[ \t]+SERUM_VERSION_PATCH[ \t]+([0-9]+)" _tmp "${version}")
if("${CMAKE_MATCH_1}" STREQUAL "")
   message(FATAL_ERROR "Failed to parse SERUM_VERSION_PATCH from src/serum-version.h")
endif()
set(VERSION_PATCH "${CMAKE_MATCH_1}")
string(REGEX MATCH "#[ \t]*define[ \t]+SERUM_CONCENTRATE_VERSION[ \t]+([0-9]+)" _tmp "${version}")
if("${CMAKE_MATCH_1}" STREQUAL "")
   message(FATAL_ERROR "Failed to parse SERUM_CONCENTRATE_VERSION from src/serum-version.h")
endif()
set(VERSION_CONCENTRATE "${CMAKE_MATCH_1}")

project(serum VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}"
   DESCRIPTION "Cross-platform library for decoding Serum files, a colorization format for pinball ROMs. Concentrate version: ${VERSION_CONCENTRATE}")

if(PLATFORM STREQUAL "win")
   if(ARCH STREQUAL "x86")
      add_compile_definitions(WIN32)
   endif()
   if(MSVC)
      add_compile_options("$<$<CONFIG:Release>:/Zi>")
      add_link_options("$<$<CONFIG:Release>:/DEBUG:FULL>")
   endif()
elseif(PLATFORM STREQUAL "macos")
   if (ARCH STREQUAL "arm64")
      set(CMAKE_OSX_ARCHITECTURES arm64)
   elseif(ARCH STREQUAL "x64")
      set(CMAKE_OSX_ARCHITECTURES x86_64)
   endif()
   set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
   set(CMAKE_INSTALL_RPATH "@executable_path")
elseif(PLATFORM STREQUAL "ios" OR PLATFORM STREQUAL "ios-simulator" OR PLATFORM STREQUAL "tvos")
   set(CMAKE_OSX_DEPLOYMENT_TARGET 16.0)
   set(CMAKE_OSX_ARCHITECTURES arm64)
elseif(PLATFORM STREQUAL "linux" OR PLATFORM STREQUAL "android")
   set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)
   set(CMAKE_INSTALL_RPATH "$ORIGIN")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 99)

set(CMAKE_CXX_VISIBILITY_PRESET hidden)
set(CMAKE_C_VISIBILITY_PRESET hidden)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
   if(ENABLE_SANITIZERS AND (PLATFORM STREQUAL "macos" OR PLATFORM STREQUAL "linux"))
      set(SANITIZER_FLAGS -fsanitize=address,undefined)
      add_compile_options(${SANITIZER_FLAGS} -fno-omit-frame-pointer -g)
      add_link_options(${SANITIZER_FLAGS})
   endif()
endif()

# Say so when the sanitizers were asked for but cannot be turned on. PLATFORM
# defaults to win, so -DENABLE_SANITIZERS=ON on its own configures a plain
# Debug build and every run of the suite looks clean because nothing is
# instrumented.
if(ENABLE_SANITIZERS AND NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" AND
      (PLATFORM STREQUAL "macos" OR PLATFORM STREQUAL "linux")))
   message(WARNING
      "ENABLE_SANITIZERS=ON has no effect here: it needs CMAKE_BUILD_TYPE=Debug "
      "and PLATFORM=macos or linux (this build has CMAKE_BUILD_TYPE="
      "'${CMAKE_BUILD_TYPE}' and PLATFORM='${PLATFORM}'). Nothing will be "
      "instrumented.")
endif()

set(SERUM_SOURCES
   src/serum-decode.cpp
   src/SerumData.cpp
   src/SceneGenerator.cpp
   third-party/include/miniz/miniz.c
   third-party/include/lz4/lz4.c
   third-party/include/lz4/lz4hc.c
)

set(SERUM_INCLUDE_DIRS
   src
   third-party/include
)

# The functions declared in src/serum-accel.h are built from
# src/serum-accel-ref.cpp, or, with SERUM_ACCEL_DIR set, from the sources that
# directory's serum-accel.cmake names. That file must set
# SERUM_ACCEL_SEAM_VERSION (the SERUM_ACCEL_SEAM_VERSION of serum-accel.h) and
# SERUM_ACCEL_SOURCES (absolute paths), and may set
# SERUM_ACCEL_COMPILE_DEFINITIONS and SERUM_ACCEL_INCLUDE_DIRS.
file(READ src/serum-accel.h _serum_accel_header)
string(REGEX MATCH "#[ \t]*define[ \t]+SERUM_ACCEL_SEAM_VERSION[ \t]+([0-9]+)" _tmp "${_serum_accel_header}")
if("${CMAKE_MATCH_1}" STREQUAL "")
   message(FATAL_ERROR "Failed to parse SERUM_ACCEL_SEAM_VERSION from src/serum-accel.h")
endif()
set(SERUM_SEAM_VERSION "${CMAKE_MATCH_1}")

set(SERUM_ACCEL_IS_REFERENCE ON)
set(SERUM_ACCEL_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/serum-accel-ref.cpp)
set(SERUM_ACCEL_COMPILE_DEFINITIONS "")
set(SERUM_ACCEL_INCLUDE_DIRS "")
if(SERUM_ACCEL_DIR)
   set(SERUM_ACCEL_IS_REFERENCE OFF)
   if(NOT IS_DIRECTORY "${SERUM_ACCEL_DIR}")
      message(FATAL_ERROR
         "SERUM_ACCEL_DIR is set to '${SERUM_ACCEL_DIR}', which is not a directory.\n"
         "Point it at a directory holding a serum-accel.cmake, or leave it empty.")
   endif()
   if(NOT EXISTS "${SERUM_ACCEL_DIR}/serum-accel.cmake")
      message(FATAL_ERROR
         "SERUM_ACCEL_DIR='${SERUM_ACCEL_DIR}' holds no serum-accel.cmake.\n"
         "That file has to set SERUM_ACCEL_SEAM_VERSION and SERUM_ACCEL_SOURCES; "
         "see the comment above this check in CMakeLists.txt.")
   endif()
   set(SERUM_ACCEL_SOURCES "")
   unset(SERUM_ACCEL_SEAM_VERSION)
   include("${SERUM_ACCEL_DIR}/serum-accel.cmake")
   if(NOT DEFINED SERUM_ACCEL_SEAM_VERSION)
      message(FATAL_ERROR
         "${SERUM_ACCEL_DIR}/serum-accel.cmake does not set SERUM_ACCEL_SEAM_VERSION.")
   endif()
   if(NOT "${SERUM_ACCEL_SEAM_VERSION}" STREQUAL "${SERUM_SEAM_VERSION}")
      message(FATAL_ERROR
         "${SERUM_ACCEL_DIR}/serum-accel.cmake sets SERUM_ACCEL_SEAM_VERSION "
         "${SERUM_ACCEL_SEAM_VERSION}, and src/serum-accel.h has "
         "${SERUM_SEAM_VERSION}.")
   endif()
   if(NOT SERUM_ACCEL_SOURCES)
      message(FATAL_ERROR
         "${SERUM_ACCEL_DIR}/serum-accel.cmake does not set SERUM_ACCEL_SOURCES.")
   endif()
   foreach(_serum_accel_source IN LISTS SERUM_ACCEL_SOURCES)
      if(NOT IS_ABSOLUTE "${_serum_accel_source}" OR NOT EXISTS "${_serum_accel_source}")
         message(FATAL_ERROR
            "SERUM_ACCEL_SOURCES names '${_serum_accel_source}', which is not "
            "an existing file given as an absolute path (set by "
            "${SERUM_ACCEL_DIR}/serum-accel.cmake).")
      endif()
   endforeach()
   message(STATUS "serum-accel: sources from ${SERUM_ACCEL_DIR} (seam version ${SERUM_SEAM_VERSION})")
else()
   message(STATUS "serum-accel: reference implementation (seam version ${SERUM_SEAM_VERSION})")
endif()
list(APPEND SERUM_SOURCES ${SERUM_ACCEL_SOURCES})

# FrameUtil.h comes from PPUC/libframeutil, by either of two routes:
#
#  * platforms/<platform>/<arch>/external.sh drops it in third-party/include.
#    This is how the standalone builds work.
#  * A packaging system installs it on the include path -- Buildroot, for
#    instance, has a libframeutil package that stages it into
#    $(STAGING_DIR)/usr/include and never runs external.sh at all.
#
# Prefer the vendored copy so a local checkout still wins, then fall back to the
# system one. Fail with an actionable message rather than a bare "file not
# found" deep inside the compile.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third-party/include/FrameUtil.h")
   message(STATUS "FrameUtil.h: third-party/include (fetched by external.sh)")
else()
   find_path(FRAMEUTIL_INCLUDE_DIR NAMES FrameUtil.h)
   if(FRAMEUTIL_INCLUDE_DIR)
      message(STATUS "FrameUtil.h: ${FRAMEUTIL_INCLUDE_DIR} (installed package)")
      list(APPEND SERUM_INCLUDE_DIRS ${FRAMEUTIL_INCLUDE_DIR})
   else()
      message(FATAL_ERROR
         "FrameUtil.h not found.\n"
         "Either fetch it into third-party/include:\n"
         "  ./platforms/${PLATFORM}/${ARCH}/external.sh\n"
         "or point that at a local checkout:\n"
         "  LIBFRAMEUTIL_SOURCE_DIR=../libframeutil ./platforms/${PLATFORM}/${ARCH}/external.sh\n"
         "or install libframeutil so FrameUtil.h is on the include path.")
   endif()
endif()

if(BUILD_SHARED)
   add_library(serum_shared SHARED ${SERUM_SOURCES})

   target_include_directories(serum_shared PUBLIC ${SERUM_INCLUDE_DIRS})
   target_include_directories(serum_shared PRIVATE ${SERUM_ACCEL_INCLUDE_DIRS})
   target_compile_definitions(serum_shared PRIVATE ${SERUM_ACCEL_COMPILE_DEFINITIONS})

   if((PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw") AND ARCH STREQUAL "x64")
      set(SERUM_OUTPUT_NAME "serum64")
   else()
      set(SERUM_OUTPUT_NAME "serum")
   endif()

   if(PLATFORM STREQUAL "win-mingw")
      set_target_properties(serum_shared PROPERTIES
         PREFIX ""
         IMPORT_PREFIX ""
         OUTPUT_NAME ${SERUM_OUTPUT_NAME}
         VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
      )
   else()
      set_target_properties(serum_shared PROPERTIES
         OUTPUT_NAME ${SERUM_OUTPUT_NAME}
         VERSION ${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}
      )
   endif()

   if(PLATFORM STREQUAL "win")
      set(SERUM_VERSION_STR "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_PATCH}")
      set(SERUM_FILE_VERSION "${VERSION_MAJOR},${VERSION_MINOR},${VERSION_PATCH},0")
      set(SERUM_PRODUCT_VERSION "${VERSION_MAJOR},${VERSION_MINOR},${VERSION_PATCH},0")
      configure_file(
         src/serum.rc.in
         ${CMAKE_CURRENT_BINARY_DIR}/serum.rc
         @ONLY
      )
      target_sources(serum_shared PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/serum.rc)
   endif()

   install(TARGETS serum_shared
      LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
   )
   install(FILES
      src/serum.h
      src/serum-decode.h
      src/TimeUtils.h
      DESTINATION ${CMAKE_INSTALL_PREFIX}/include)

   if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw" OR PLATFORM STREQUAL "macos" OR PLATFORM STREQUAL "linux")
      add_executable(serum_test
         src/test.cpp
      )

      target_link_libraries(serum_test PUBLIC serum_shared)

      # Unit tests. The TU includes src/serum-decode.cpp so that the static
      # internals are reachable, so it must NOT link the library as well --
      # see tests/README.md.
      enable_testing()
      add_executable(serum_unit_tests
         tests/unit_tests.cpp
      )
      target_include_directories(serum_unit_tests PRIVATE
         ${CMAKE_CURRENT_SOURCE_DIR}/src
         ${CMAKE_CURRENT_SOURCE_DIR}/third-party/include
      )
      # Everything SERUM_SOURCES has except serum-decode.cpp and
      # serum-accel-ref.cpp, which the test TU includes itself.
      target_sources(serum_unit_tests PRIVATE
         src/SerumData.cpp
         src/SceneGenerator.cpp
         third-party/include/lz4/lz4.c
         third-party/include/lz4/lz4hc.c
         third-party/include/miniz/miniz.c
      )
      if(SERUM_ACCEL_IS_REFERENCE)
         target_compile_definitions(serum_unit_tests PRIVATE SERUM_ACCEL_REFERENCE_IN_TEST_TU=1)
      else()
         target_sources(serum_unit_tests PRIVATE ${SERUM_ACCEL_SOURCES})
         target_include_directories(serum_unit_tests PRIVATE ${SERUM_ACCEL_INCLUDE_DIRS})
         target_compile_definitions(serum_unit_tests PRIVATE ${SERUM_ACCEL_COMPILE_DEFINITIONS})
      endif()
      add_test(NAME serum_unit_tests COMMAND serum_unit_tests)
   endif()
endif()

if(BUILD_STATIC)
   add_library(serum_static STATIC ${SERUM_SOURCES})

   target_include_directories(serum_static PUBLIC ${SERUM_INCLUDE_DIRS})
   target_include_directories(serum_static PRIVATE ${SERUM_ACCEL_INCLUDE_DIRS})
   target_compile_definitions(serum_static PRIVATE ${SERUM_ACCEL_COMPILE_DEFINITIONS})

   if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw")
      set_target_properties(serum_static PROPERTIES
         PREFIX ""
         OUTPUT_NAME "serum_static"
      )
   else()
      set_target_properties(serum_static PROPERTIES
         OUTPUT_NAME "serum"
      )
   endif()

   install(TARGETS serum_static
      LIBRARY DESTINATION ${CMAKE_INSTALL_PREFIX}/lib
   )
   install(FILES
      src/serum.h
      src/serum-decode.h
      src/TimeUtils.h
      DESTINATION ${CMAKE_INSTALL_PREFIX}/include)

   if(PLATFORM STREQUAL "win" OR PLATFORM STREQUAL "win-mingw" OR PLATFORM STREQUAL "macos" OR PLATFORM STREQUAL "linux")
      add_executable(serum_test_s
         src/test.cpp
      )

      target_link_libraries(serum_test_s PUBLIC serum_static)
   endif()
endif()
