cmake_minimum_required(VERSION 3.16)
project(prach-inject LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# srsRAN_4G — built locally from source (pre-built system binaries had FFT/nof_prb
# assumptions that differ from our cell config, so we compile from the tree).
#
# Default: sibling directory ../srsRAN_4G relative to this repo.
# Override at configure time with: cmake -DSRSRAN_ROOT=/path/to/srsRAN_4G ..
# ---------------------------------------------------------------------------
if(NOT DEFINED SRSRAN_ROOT)
    set(SRSRAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../srsRAN_4G")
endif()
set(SRSRAN_BUILD "${SRSRAN_ROOT}/build")

if(NOT EXISTS "${SRSRAN_ROOT}/lib/include/srsran/srsran.h")
    message(FATAL_ERROR
        "srsRAN headers not found at ${SRSRAN_ROOT}/lib/include/srsran/\n"
        "Set -DSRSRAN_ROOT=<path> or place srsRAN_4G alongside this repo.")
endif()

if(NOT EXISTS "${SRSRAN_BUILD}/lib/src/phy/libsrsran_phy.a")
    message(FATAL_ERROR
        "libsrsran_phy.a not found. Build srsRAN_4G first:\n"
        "  cd ${SRSRAN_ROOT} && mkdir build && cd build && cmake .. && make -j$(nproc)")
endif()

# Embed RPATH so libsrsran_rf*.so is found without LD_LIBRARY_PATH
set(SRSRAN_RF_RPATH "${SRSRAN_BUILD}/lib/src/phy/rf")
set(CMAKE_BUILD_RPATH "${SRSRAN_RF_RPATH}")
set(CMAKE_INSTALL_RPATH "${SRSRAN_RF_RPATH}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

find_package(PkgConfig REQUIRED)

# PHY static library — whole-archive to pull in all symbols (circular deps)
# allow-multiple-definition handles duplicate weak symbols between srsran libs
set(SRSRAN_PHY_LINK_LIBS
    "-Wl,--allow-multiple-definition"
    "-Wl,--whole-archive" "${SRSRAN_BUILD}/lib/src/phy/libsrsran_phy.a" "-Wl,--no-whole-archive"
)

set(SRSRAN_COMMON_LINK_LIBS "${SRSRAN_BUILD}/lib/src/common/libsrsran_common.a")
set(SRSRAN_SRSLOG_LIBS      "${SRSRAN_BUILD}/lib/src/srslog/libsrslog.a")

# RF plugin — shared lib, loaded at runtime by srsran_rf_open
set(SRSRAN_RF_LIB "${SRSRAN_BUILD}/lib/src/phy/rf/libsrsran_rf.so")

# Transitive dependencies
pkg_check_modules(MBEDTLS REQUIRED mbedtls)
pkg_check_modules(FMT     REQUIRED fmt)
pkg_check_modules(FFTW3F  REQUIRED fftw3f)
find_package(Boost REQUIRED COMPONENTS system)

set(SRSRAN_COMMON_DEPS_LIBS
    ${SRSRAN_SRSLOG_LIBS}
    ${MBEDTLS_LIBRARIES}
    ${FMT_LIBRARIES}
    ${FFTW3F_LIBRARIES}
    Boost::system
)

pkg_check_modules(UHD REQUIRED uhd)

# rt-recon-sdk — header-only path shared with the sniffer (influxdb.hpp schema)
set(RT_RECON_SDK "/home/avi/wireless/rt-recon-sdk")
if(NOT EXISTS "${RT_RECON_SDK}/include/rt-recon-sdk/autoconfig/influx_worker.h")
    message(FATAL_ERROR "rt-recon-sdk not found at ${RT_RECON_SDK}")
endif()

# ---------------------------------------------------------------------------
# Library sources
# ---------------------------------------------------------------------------
set(LIB_SOURCES
    src/cell_config.cpp
    src/ro.cpp
    src/ra_rnti.cpp
    src/log_csv.cpp
    src/ssb_sync.cpp
    src/influx_reader.cpp
    src/prach_tx.cpp
    src/tool_config.cpp
    src/gnb_detect_reader.cpp
)

add_library(prach_inject_lib STATIC ${LIB_SOURCES})

target_include_directories(prach_inject_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${SRSRAN_ROOT}/lib/include
    ${SRSRAN_BUILD}/lib/include
    ${RT_RECON_SDK}/include
)

target_link_libraries(prach_inject_lib PRIVATE
    ${SRSRAN_PHY_LINK_LIBS}
    ${SRSRAN_COMMON_LINK_LIBS}
    ${SRSRAN_COMMON_DEPS_LIBS}
    ${SRSRAN_RF_LIB}
    yaml-cpp
    pthread
    ${UHD_LIBRARIES}
)

target_compile_options(prach_inject_lib PRIVATE -Wall)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(prach-inject src/main.cpp)
target_link_libraries(prach-inject PRIVATE
    prach_inject_lib
    ${SRSRAN_PHY_LINK_LIBS}
    ${SRSRAN_COMMON_LINK_LIBS}
    ${SRSRAN_COMMON_DEPS_LIBS}
    ${SRSRAN_RF_LIB}
)
target_compile_options(prach-inject PRIVATE -Wall)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
add_executable(test_ro tests/test_ro.cpp)
target_link_libraries(test_ro PRIVATE prach_inject_lib)
target_compile_options(test_ro PRIVATE -Wall)

add_executable(test_pbch tests/test_pbch.cpp)
target_link_libraries(test_pbch PRIVATE
    prach_inject_lib
    ${SRSRAN_PHY_LINK_LIBS}
    ${SRSRAN_COMMON_LINK_LIBS}
    ${SRSRAN_COMMON_DEPS_LIBS}
)
target_compile_options(test_pbch PRIVATE -Wall)

add_executable(test_ra_rnti tests/test_ra_rnti.cpp)
target_link_libraries(test_ra_rnti PRIVATE prach_inject_lib)
target_compile_options(test_ra_rnti PRIVATE -Wall)
