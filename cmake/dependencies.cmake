# ---------------------------------------------------------------------------
# Everything FastPlugs builds on, fetched at configure time and pinned.
#
# Fetch-only checkouts are declared with a SOURCE_SUBDIR that does not exist,
# which is how FetchContent_MakeAvailable is told to download a project and
# not build it: these are compiled here from explicit source lists, as
# FastRoute does, rather than through their own build systems.
# ---------------------------------------------------------------------------
include(FetchContent)

# --- FastRoute -------------------------------------------------------------
# Where the effects come from. Point this at a working copy to build against
# whatever is there now:
#
#     cmake -B build -DFASTROUTE_SOURCE_DIR=C:/git/FastRoute
#
# Left empty, the pinned commit below is fetched, which is what CI does.
set(FASTROUTE_SOURCE_DIR "" CACHE PATH "A FastRoute checkout to take the effects from")
set(FASTROUTE_GIT_TAG 157be33eae1311d1f303d54f751bcda9528b9056 CACHE STRING
    "FastRoute commit to fetch when FASTROUTE_SOURCE_DIR is empty")

if (FASTROUTE_SOURCE_DIR STREQUAL "")
    FetchContent_Declare(fastroute
        GIT_REPOSITORY https://github.com/masonasons/FastRoute.git
        GIT_TAG        ${FASTROUTE_GIT_TAG}
        SOURCE_SUBDIR  do-not-build)
    FetchContent_MakeAvailable(fastroute)
    set(FASTROUTE_SOURCE_DIR ${fastroute_SOURCE_DIR})
endif()
if (NOT EXISTS ${FASTROUTE_SOURCE_DIR}/src/model.h)
    message(FATAL_ERROR "No FastRoute source at ${FASTROUTE_SOURCE_DIR}")
endif()
message(STATUS "FastPlugs: effects from ${FASTROUTE_SOURCE_DIR}")

# --- RNNoise ---------------------------------------------------------------
# The same pin as FastRoute, for the same reason: v0.1.1 is the last release
# that commits its trained weights, so the build never downloads a model.
FetchContent_Declare(rnnoise
    GIT_REPOSITORY https://github.com/xiph/rnnoise.git
    GIT_TAG        6cbfd53eb348a8d394e0757b4025c6ded34eb2b6   # v0.1.1
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(rnnoise)

set(_rnn_lpc ${rnnoise_SOURCE_DIR}/src/pitch.c ${rnnoise_SOURCE_DIR}/src/celt_lpc.c)
if (MSVC)
    # MSVC's C compiler has no variable length arrays and v0.1.1 has five.
    # Rewritten to _alloca in copies in the build tree, exactly as FastRoute's
    # third_party/CMakeLists.txt does. Clang and GCC take the originals.
    set(RNNOISE_FIXED ${CMAKE_CURRENT_BINARY_DIR}/rnnoise_msvc)
    file(MAKE_DIRECTORY ${RNNOISE_FIXED})
    set(RNNOISE_VLA_FIXES
        "opus_val16 x_lp4[len>>2]|opus_val16 *x_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(len>>2))"
        "opus_val16 y_lp4[lag>>2]|opus_val16 *y_lp4 = (opus_val16*)_alloca(sizeof(opus_val16)*(lag>>2))"
        "opus_val32 xcorr[max_pitch>>1]|opus_val32 *xcorr = (opus_val32*)_alloca(sizeof(opus_val32)*(max_pitch>>1))"
        "opus_val32 yy_lookup[maxperiod+1]|opus_val32 *yy_lookup = (opus_val32*)_alloca(sizeof(opus_val32)*(maxperiod+1))"
        "opus_val16 xx[n]|opus_val16 *xx = (opus_val16*)_alloca(sizeof(opus_val16)*(n))")
    set(_rnn_lpc "")
    foreach(f pitch.c celt_lpc.c)
        file(READ ${rnnoise_SOURCE_DIR}/src/${f} _text)
        foreach(fix ${RNNOISE_VLA_FIXES})
            string(REPLACE "|" ";" _pair "${fix}")
            list(GET _pair 0 _from)
            list(GET _pair 1 _to)
            string(REPLACE "${_from}" "${_to}" _text "${_text}")
        endforeach()
        file(WRITE ${RNNOISE_FIXED}/${f} "#include <malloc.h>\n${_text}")
        list(APPEND _rnn_lpc ${RNNOISE_FIXED}/${f})
    endforeach()
endif()

add_library(rnnoise STATIC
    ${rnnoise_SOURCE_DIR}/src/denoise.c
    ${rnnoise_SOURCE_DIR}/src/rnn.c
    ${rnnoise_SOURCE_DIR}/src/rnn_data.c
    ${rnnoise_SOURCE_DIR}/src/kiss_fft.c
    ${_rnn_lpc})
target_include_directories(rnnoise PUBLIC ${rnnoise_SOURCE_DIR}/include)
target_include_directories(rnnoise PRIVATE ${rnnoise_SOURCE_DIR}/src)
target_compile_definitions(rnnoise PRIVATE
    RNNOISE_EXPORT= _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)

# --- SpeexDSP --------------------------------------------------------------
FetchContent_Declare(speexdsp
    GIT_REPOSITORY https://github.com/xiph/speexdsp.git
    GIT_TAG        7a158783df74efe7c2d1c6ee8363c1e695c71226
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(speexdsp)

add_library(speexdsp STATIC
    ${speexdsp_SOURCE_DIR}/libspeexdsp/preprocess.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/filterbank.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/fftwrap.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/kiss_fftr.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/smallft.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/resample.c
    ${speexdsp_SOURCE_DIR}/libspeexdsp/mdf.c)
target_include_directories(speexdsp PUBLIC ${speexdsp_SOURCE_DIR}/include)
target_include_directories(speexdsp PRIVATE ${speexdsp_SOURCE_DIR}/libspeexdsp)
target_compile_definitions(speexdsp PRIVATE
    FLOATING_POINT USE_KISS_FFT EXPORT= _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS
    # Turning the VAD off prints "The VAD has been replaced by a hack" to
    # stderr every time, which in a plugin is every instance in a host's log.
    DISABLE_WARNINGS)
# speexdsp_types.h names its integer types itself on Windows and on the Mac,
# and otherwise expects autotools to have generated a header that says what
# they are. That header is written here, for anything else this is built on.
if (NOT WIN32 AND NOT APPLE)
    set(_spx_cfg ${CMAKE_CURRENT_BINARY_DIR}/speexdsp_cfg)
    file(WRITE ${_spx_cfg}/speexdsp_config_types.h
"#pragma once
#include <stdint.h>
typedef int16_t spx_int16_t;
typedef uint16_t spx_uint16_t;
typedef int32_t spx_int32_t;
typedef uint32_t spx_uint32_t;
")
    target_include_directories(speexdsp PUBLIC ${_spx_cfg})
endif()

foreach(t rnnoise speexdsp)
    if (MSVC)
        target_compile_options(${t} PRIVATE /W0)
    else()
        target_compile_options(${t} PRIVATE -w)
    endif()
endforeach()

# --- Signalsmith Stretch ---------------------------------------------------
# The pitch shifter, header only. 1.1.0 for the reason FastRoute gives: it is
# self-contained, where later versions split the FFT into a second repository.
FetchContent_Declare(signalsmith_stretch
    GIT_REPOSITORY https://github.com/Signalsmith-Audio/signalsmith-stretch.git
    GIT_TAG        44c8f865af9da8c29cc4a70a2d5a3ec83639c711   # 1.1.0
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""
    SOURCE_SUBDIR  do-not-build)
FetchContent_MakeAvailable(signalsmith_stretch)
add_library(signalsmith_stretch INTERFACE)
target_include_directories(signalsmith_stretch SYSTEM INTERFACE ${signalsmith_stretch_SOURCE_DIR})

# --- clap-wrapper ----------------------------------------------------------
# Turns the one CLAP into a VST3 and an AUv2. It fetches what it needs itself:
# the CLAP headers, the VST3 SDK (MIT since 3.8) and Apple's AudioUnitSDK
# (Apache 2.0). All three are compatible with FastRoute's GPLv3.
set(CLAP_WRAPPER_DOWNLOAD_DEPENDENCIES TRUE CACHE BOOL "" FORCE)
FetchContent_Declare(clap-wrapper
    GIT_REPOSITORY https://github.com/free-audio/clap-wrapper.git
    GIT_TAG        806bba0a5f792e3966b99c1561ab9952d3316d7c   # v0.17.0
    GIT_SHALLOW    FALSE)
FetchContent_MakeAvailable(clap-wrapper)
