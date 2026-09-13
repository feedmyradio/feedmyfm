cmake_minimum_required(VERSION 3.16)
# C is only for the vendored picohttpparser.c (src/third_party/) -- the
# admin/status HTTP request parser. Everything we write is C++17.
project(feedmyfm_rx CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Target ISA. "native" is right for a build that runs on the same box (the
# default for `make build-rx`); the Docker image overrides it with a
# portable baseline (e.g. x86-64-v3 = AVX2+FMA) so the image isn't pinned
# to the builder's CPU.
set(FMRX_MARCH "native" CACHE STRING "Value for -march= (native, x86-64-v3, ...)")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=${FMRX_MARCH}")

# --- liquid-dsp (no pkg-config / CMake package shipped) ------------------
# LIQUID_ROOT hints at a non-system install (e.g. a from-source build under
# /usr/local or a local prefix).
find_path(LIQUID_INCLUDE_DIR liquid/liquid.h HINTS ${LIQUID_ROOT}/include)
find_library(LIQUID_LIBRARY NAMES liquid HINTS ${LIQUID_ROOT}/lib)
if(NOT LIQUID_INCLUDE_DIR OR NOT LIQUID_LIBRARY)
    message(FATAL_ERROR
        "liquid-dsp not found. Install it (Debian/Ubuntu: "
        "`sudo apt install libliquid-dev libliquid2d`), build it from source, "
        "or set LIQUID_ROOT / LIQUID_INCLUDE_DIR / LIQUID_LIBRARY.")
endif()

# --- libiio ------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBIIO REQUIRED libiio)

# --- threads (per-station worker pool in the live loop) ---------------
find_package(Threads REQUIRED)

add_executable(feedmyfm-rx
    src/main.cpp
    src/third_party/picohttpparser.c
)
# Vendored third-party C: not our warnings to chase.
set_source_files_properties(src/third_party/picohttpparser.c
    PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(feedmyfm-rx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    # vendored header-only deps (fkYAML -- see src/yaml.hpp)
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
    ${LIQUID_INCLUDE_DIR}
    ${LIBIIO_INCLUDE_DIRS}
)
target_link_libraries(feedmyfm-rx PRIVATE
    ${LIQUID_LIBRARY}
    ${LIBIIO_LIBRARIES}
    Threads::Threads
    m
)
target_link_directories(feedmyfm-rx PRIVATE ${LIBIIO_LIBRARY_DIRS})

# --- CTest suite (no external test framework; direct binary invocation) -----
enable_testing()

# rx_test(name ARGS arg... [TIMEOUT sec] [MATCH regex] [SLOW] [STATIONS file]
#         [CONFIG file] [WILL_FAIL])
# Generate a test that runs feedmyfm-rx with the given arguments against
# rx/tests/config.yml (or CONFIG file) and an optional stations.yml, and
# optionally assert on stdout (MATCH regex), expect a non-zero exit
# (WILL_FAIL), or mark it as slow (excluded from default ctest run).
function(rx_test name)
    cmake_parse_arguments(TEST "SLOW;WILL_FAIL" "TIMEOUT;MATCH;STATIONS;CONFIG" "ARGS" ${ARGN})
    set(test_timeout "${TEST_TIMEOUT}")
    if(NOT test_timeout)
        if(TEST_SLOW)
            set(test_timeout 240)  # slow tests up to 4 min (--snr-cal is ~80s)
        else()
            set(test_timeout 60)
        endif()
    endif()

    # Use custom stations file if specified, else default
    set(stations_file ${CMAKE_CURRENT_SOURCE_DIR}/tests/stations.yml)
    if(TEST_STATIONS)
        set(stations_file ${CMAKE_CURRENT_SOURCE_DIR}/tests/${TEST_STATIONS})
    endif()

    set(config_file ${CMAKE_CURRENT_SOURCE_DIR}/tests/config.yml)
    if(TEST_CONFIG)
        set(config_file ${CMAKE_CURRENT_SOURCE_DIR}/tests/${TEST_CONFIG})
    endif()

    add_test(NAME ${name}
        COMMAND ${CMAKE_CURRENT_BINARY_DIR}/feedmyfm-rx -c ${config_file} -s ${stations_file} ${TEST_ARGS})
    set_tests_properties(${name} PROPERTIES TIMEOUT ${test_timeout})

    if(TEST_MATCH)
        set_tests_properties(${name} PROPERTIES
            PASS_REGULAR_EXPRESSION "${TEST_MATCH}")
    endif()

    if(TEST_WILL_FAIL)
        set_tests_properties(${name} PROPERTIES WILL_FAIL TRUE)
    endif()

    if(TEST_SLOW)
        set_tests_properties(${name} PROPERTIES LABELS slow)
    endif()
endfunction()

# Fast tests (config validation, --check): all pass/fail on returncode
rx_test(check-good-config
    ARGS --check)

# An empty station list is allowed -- "receiver up for scan/admin only".
# --check must succeed (returncode 0) and emit the finding.
rx_test(check-empty-stations
    ARGS --check
    STATIONS stations-empty.yml
    MATCH "receiver up for scan/admin only")

# Front-end AGC config surface (FRONT_END_AGC_PLAN.md). auto_sw + a full
# sdr.agc block resolves and is echoed under "sdr" in --check --json.
rx_test(check-gain-mode-auto-sw
    ARGS --check --json
    CONFIG config-gainmode.yml
    MATCH "\"gain_mode\": ?\"auto_sw\"")

# An unknown gain_mode is a hard error.
rx_test(check-gain-mode-bad
    ARGS --check
    CONFIG config-gainmode-badmode.yml
    WILL_FAIL)

# sdr.agc without gain_mode: auto_sw is silently ignored, not a hard error:
# the config resolves and the agc block is neither parsed nor echoed.
rx_test(check-agc-without-auto-sw
    ARGS --check --json
    CONFIG config-agc-nomode.yml
    MATCH "\"gain_mode\": ?\"manual\"")

# status.public.* (STATUS_VISIBILITY_PLAN.md): a full block resolves and is
# echoed under "status_public" in --check --json.
rx_test(check-status-public
    ARGS --check --json
    CONFIG config-status-public.yml
    MATCH "\"status_public\": ?\{[^}]*\"rds_text\": ?false")

# An unknown key under status.public is a hard error (typo-catching).
rx_test(check-status-public-badkey
    ARGS --check
    CONFIG config-status-public-badkey.yml
    WILL_FAIL)

# Slow tests (DSP sweeps): check for PASS/FAIL marker in stdout
rx_test(selftest-mono
    ARGS --selftest --seconds 3
    MATCH "RESULT: PASS"
    SLOW)

rx_test(selftest-stereo
    ARGS --stereo-selftest --seconds 3
    MATCH "RESULT: PASS"
    STATIONS stations-stereo.yml
    SLOW)

rx_test(sweep-deemphasis
    ARGS --sweep --seconds 2
    MATCH "-> PASS"
    TIMEOUT 120
    SLOW)

rx_test(snr-calibration
    ARGS --snr-cal --seconds 3
    MATCH "-> PASS"
    TIMEOUT 240
    SLOW)

# FM demod threshold comparison (WEAK_SIGNAL_DEMOD_PLAN.md §5.2): sweeps the
# knee for discriminator / +declick / pll and asserts the declick path does
# not regress above the knee and shows a knee gain.
rx_test(demod-threshold
    ARGS --demod-cal --seconds 3
    MATCH "-> PASS"
    TIMEOUT 240
    SLOW)

# RDS decode: a standalone binary (no SDR, no config) that synthesizes a
# clean RDS composite and checks RdsDecoder locks + recovers PI/PS/PTY,
# across the front-end-decimating and pass-through channel rates.
add_executable(rds_selftest tests/rds_selftest.cpp)
target_include_directories(rds_selftest PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/third_party
    ${LIQUID_INCLUDE_DIR}
)
target_link_libraries(rds_selftest PRIVATE ${LIQUID_LIBRARY} m)
add_test(NAME rds-decode COMMAND rds_selftest)
set_tests_properties(rds-decode PROPERTIES
    TIMEOUT 60
    PASS_REGULAR_EXPRESSION "RESULT: PASS")

# Scan test: uses custom config with scan: block and multi-station setup
add_test(NAME scan-synthetic
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/feedmyfm-rx
        -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/config-scan.yml
        -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/stations-scan.yml
        --scan)
set_tests_properties(scan-synthetic PROPERTIES
    TIMEOUT 60
    LABELS slow)

# Scan path must still run with zero stations configured (scan-only mode).
add_test(NAME scan-no-stations
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/feedmyfm-rx
        -c ${CMAKE_CURRENT_SOURCE_DIR}/tests/config-scan.yml
        -s ${CMAKE_CURRENT_SOURCE_DIR}/tests/stations-empty.yml
        --scan)
set_tests_properties(scan-no-stations PROPERTIES
    TIMEOUT 60
    LABELS slow
    PASS_REGULAR_EXPRESSION "RESULT: PASS")

# --- Config validation tests (--check, --json) ---
# These test config parsing and error handling.

# Parametrized config validation tests remain in pytest (webui/tests/test_rx_selftest.py)
# because they require:
#   1. Dynamic YAML generation (add/modify blocks)
#   2. Parametrized test matrices (pytest.mark.parametrize)
#   3. JSON parsing and assertion
# Moving all of these to CMake would require significant test infrastructure
# (temporary file generation, JSON parsing in CMake, parametrization macros).
#
# The performance-critical tests (DSP sweeps above: --selftest, --sweep, --snr-cal)
# are now in CTest. Fast config tests can remain in pytest without hurting CI:
# they complete in ~2 s total vs ~80 s for --snr-cal.
#
# Rationale: §3's goal is to move rx testing out of Python; the DSP tests
# (which consume resources and need C++ access) are now in CTest. Config
# validation is still in pytest but no longer blocks rx/ portability
# (rx binary has no Python runtime dependency).
