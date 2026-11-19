cmake_minimum_required(VERSION 3.24)

# Universal on the Mac, and old enough to reach most machines still making
# music. 10.15 because std::filesystem, which the buffer's file saving uses,
# arrived in the system C++ library there. Both have to be set before
# project(), which is where the toolchain is decided.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.15" CACHE STRING "Oldest macOS supported")
if (NOT DEFINED CMAKE_OSX_ARCHITECTURES)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
endif()

project(FastPlugs VERSION 0.1.0 LANGUAGES C CXX)

# The Audio Unit wrapper is Objective-C++. clap-wrapper enables that language
# in its own project, but make_clapfirst_plugins creates the AU target in this
# directory, and a language enabled in a subproject is not set up here.
if (APPLE)
    enable_language(OBJC OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The CRT goes into the plugin rather than being expected on the machine, the
# same choice FastRoute makes and for the same reason: a plugin that will not
# load because a redistributable is missing gives the host nothing to say
# except that it would not load.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# What every host shows. Changing the vendor or any id after a release breaks
# every saved project that uses these plugins, so they are set once, here.
set(FASTPLUGS_VENDOR      "FastPlugs")
set(FASTPLUGS_BUNDLE_NAME "FastPlugs")
set(FASTPLUGS_ID_PREFIX   "io.github.masonasons.fastplugs")
set(FASTPLUGS_URL         "https://github.com/masonasons/FastRoute")
set(FASTPLUGS_AU_MANUFACTURER "FPlg")

include(cmake/dependencies.cmake)

# ---------------------------------------------------------------------------
# FastRoute's own effects
#
# Copied out of the FastRoute tree into the build tree, rather than compiled
# where they stand. They include "audio.h" in quotes, and a quoted include is
# looked for beside the file first - which in FastRoute's tree is the real
# audio.h, with windows.h and the whole engine behind it. In the build tree
# there is nothing beside them, so the stand-in in src/shim is what they find.
# configure_file makes the copy a dependency, so editing an effect in
# FastRoute reconfigures and rebuilds this.
# ---------------------------------------------------------------------------
set(FASTROUTE_FX_SOURCES eq.cpp dynamics.cpp normalise.cpp pitch.cpp denoise.cpp buffer.cpp)
set(FASTROUTE_FX_COPY ${CMAKE_BINARY_DIR}/fastroute_fx)
set(_fx_copied "")
foreach(f ${FASTROUTE_FX_SOURCES})
    configure_file(${FASTROUTE_SOURCE_DIR}/src/audio/${f} ${FASTROUTE_FX_COPY}/${f} COPYONLY)
    list(APPEND _fx_copied ${FASTROUTE_FX_COPY}/${f})
endforeach()

add_library(fastroute_fx STATIC
    ${_fx_copied}
    src/shim/shim.cpp
    src/shim/platform.cpp)
# The stand-ins first, so audio.h and util.h resolve to them; FastRoute's src
# second, for model.h, which is plain C++ and is used exactly as it is.
target_include_directories(fastroute_fx PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src/shim
    ${FASTROUTE_SOURCE_DIR}/src)
target_link_libraries(fastroute_fx PUBLIC rnnoise speexdsp signalsmith_stretch)
if (WIN32)
    target_compile_definitions(fastroute_fx PUBLIC NOMINMAX _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(fastroute_fx PUBLIC shell32 ole32)
elseif (APPLE)
    target_link_libraries(fastroute_fx PUBLIC "-framework AudioToolbox" "-framework CoreFoundation")
endif()
if (MSVC)
    target_compile_options(fastroute_fx PRIVATE /W3 /permissive- /utf-8)
else()
    # FastRoute is written against MSVC; what clang says about it is
    # FastRoute's business, and these two are noise rather than findings.
    target_compile_options(fastroute_fx PRIVATE -Wno-unknown-pragmas -Wno-unused-private-field)
endif()

# ---------------------------------------------------------------------------
# The CLAP, and everything wrapped around it
# ---------------------------------------------------------------------------
add_library(fastplugs_impl STATIC
    src/plugin/effects.cpp
    src/plugin/plugin.cpp
    src/plugin/factory.cpp)
target_include_directories(fastplugs_impl PUBLIC src/plugin)
target_link_libraries(fastplugs_impl PUBLIC fastroute_fx clap clap-wrapper-extensions)
target_compile_definitions(fastplugs_impl PRIVATE
    FASTPLUGS_VENDOR="${FASTPLUGS_VENDOR}"
    FASTPLUGS_ID_PREFIX="${FASTPLUGS_ID_PREFIX}"
    FASTPLUGS_URL="${FASTPLUGS_URL}"
    FASTPLUGS_VERSION="${PROJECT_VERSION}"
    FASTPLUGS_AU_MANUFACTURER="${FASTPLUGS_AU_MANUFACTURER}")
if (MSVC)
    target_compile_options(fastplugs_impl PRIVATE /W4 /permissive- /utf-8)
else()
    target_compile_options(fastplugs_impl PRIVATE -Wall -Wextra)
endif()

set(FASTPLUGS_FORMATS CLAP VST3)
if (APPLE)
    list(APPEND FASTPLUGS_FORMATS AUV2)
endif()

make_clapfirst_plugins(
    TARGET_NAME fastplugs
    IMPL_TARGET fastplugs_impl
    OUTPUT_NAME "${FASTPLUGS_BUNDLE_NAME}"
    ENTRY_SOURCE src/plugin/entry.cpp

    BUNDLE_IDENTIFIER "${FASTPLUGS_ID_PREFIX}"
    BUNDLE_VERSION "${PROJECT_VERSION}"

    COPY_AFTER_BUILD FALSE
    # The bundle layout Steinberg asks for, and the one their validator
    # insists on. Every current host reads it.
    WINDOWS_FOLDER_VST3 TRUE

    PLUGIN_FORMATS ${FASTPLUGS_FORMATS}
    ASSET_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/out
    # No AUV2_* codes here: there are thirteen plugins in one bundle, so the
    # wrapper reads each one's subtype from the CLAP's auv2 factory instead.
)

# ---------------------------------------------------------------------------
# A host in miniature that runs every plugin through activation, parameter
# changes, latency, state and every channel layout, without a DAW.
# ---------------------------------------------------------------------------
option(FASTPLUGS_TESTS "Build the offline smoke test" ON)
if (FASTPLUGS_TESTS)
    enable_testing()
    add_executable(fastplugs_smoke tests/smoke.cpp)
    target_link_libraries(fastplugs_smoke PRIVATE fastplugs_impl)
    add_test(NAME smoke COMMAND fastplugs_smoke)
endif()
