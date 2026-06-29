cmake_minimum_required(VERSION 3.16)
project(ra-spoof LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Find srsRAN
# ---------------------------------------------------------------------------
# We now use the locally built srsRAN_4G from the srsRAN_4G/build directory
# instead of linking against pre-built binaries that might have different
# FFT/nof_prb assumptions.
# ---------------------------------------------------------------------------
set(SRSRAN_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/srsRAN_4G")
set(SRSRAN_BUILD "${SRSRAN_ROOT}/build")

# Verify local srsRAN headers exist
if(NOT EXISTS "${SRSRAN_ROOT}/lib/include/srsran/srsran.h")
  message(FATAL_ERROR "srsRAN headers not found in ${SRSRAN_ROOT}/lib/include/srsran/")
endif()

# Verify local srsRAN build exists
if(NOT EXISTS "${SRSRAN_BUILD}/lib/src/phy/libsrsran_phy.a")
  message(FATAL_ERROR "libsrsran_phy.a not found. Please build srsRAN_4G in ${SRSRAN_BUILD} first.")
endif()

# Embed runtime library path so libsrsran_rf*.so is found without LD_LIBRARY_PATH
set(SRSRAN_RF_RPATH "${SRSRAN_BUILD}/lib/src/phy/rf")
set(CMAKE_BUILD_RPATH "${SRSRAN_RF_RPATH}")
set(CMAKE_INSTALL_RPATH "${SRSRAN_RF_RPATH}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

find_package(PkgConfig REQUIRED)

# PHY library
# Whole-archive to resolve circular deps; allow-multiple-definition for duplicate symbols
set(SRSRAN_PHY_LINK_LIBS
  "-Wl,--allow-multiple-definition"
  "-Wl,--whole-archive" "${SRSRAN_BUILD}/lib/src/phy/libsrsran_phy.a" "-Wl,--no-whole-archive"
)

# Common library
set(SRSRAN_COMMON_LINK_LIBS "${SRSRAN_BUILD}/lib/src/common/libsrsran_common.a")

# srslog library
set(SRSRAN_SRSLOG_LIBS "${SRSRAN_BUILD}/lib/src/srslog/libsrslog.a")

# srsran_rf (shared library for RF abstraction — UHD plugin loaded at runtime)
set(SRSRAN_RF_LIB "${SRSRAN_BUILD}/lib/src/phy/rf/libsrsran_rf.so")

# Transitive deps needed by libsrsran_phy and libsrsran_common
pkg_check_modules(MBEDTLS REQUIRED mbedtls)
pkg_check_modules(FMT REQUIRED fmt)
pkg_check_modules(FFTW3F REQUIRED fftw3f)
find_package(Boost REQUIRED COMPONENTS system)

set(SRSRAN_COMMON_DEPS_LIBS
  ${SRSRAN_SRSLOG_LIBS}
  ${MBEDTLS_LIBRARIES}
  ${FMT_LIBRARIES}
  ${FFTW3F_LIBRARIES}
  Boost::system
)

# UHD
pkg_check_modules(UHD REQUIRED uhd)

# rt-recon-sdk — shares influxdb.hpp schema with sni5gect-compact
# Source files are compiled in directly (no separate build step required)
set(RT_RECON_SDK "/home/avi/wireless/rt-recon-sdk")
if(NOT EXISTS "${RT_RECON_SDK}/include/rt-recon-sdk/autoconfig/influx_worker.h")
  message(FATAL_ERROR "rt-recon-sdk not found at ${RT_RECON_SDK}")
endif()

# Common sources for library
set(LIB_SOURCES
    src/cell_config.cpp
    src/ro.cpp
    src/ra_rnti.cpp
    src/log_csv.cpp
    src/ssb_sync.cpp
    src/influx_reader.cpp
    src/prach_tx.cpp
    src/tool_config.cpp
  # NOTE: rt-recon-sdk used header-only (influxdb.hpp flux_query + local parse logic)
)

add_library(ra_spoof_lib STATIC ${LIB_SOURCES})

target_include_directories(ra_spoof_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${SRSRAN_ROOT}/lib/include
  ${SRSRAN_BUILD}/lib/include  # For generated build_info.h / version.h
  ${RT_RECON_SDK}/include
)

target_link_libraries(ra_spoof_lib PRIVATE
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
  ${SRSRAN_RF_LIB}
  yaml-cpp
  pthread
  ${UHD_LIBRARIES}
)

target_compile_options(ra_spoof_lib PRIVATE -Wall)

# Main executable
add_executable(ra-spoof src/main.cpp)
target_link_libraries(ra-spoof PRIVATE
  ra_spoof_lib
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
  ${SRSRAN_RF_LIB}
)
target_compile_options(ra-spoof PRIVATE -Wall)

# Tests
add_executable(test_ro tests/test_ro.cpp)
target_link_libraries(test_ro PRIVATE ra_spoof_lib)
target_compile_options(test_ro PRIVATE -Wall)

add_executable(test_pbch tests/test_pbch.cpp)
target_link_libraries(test_pbch PRIVATE
  ra_spoof_lib
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
)
target_compile_options(test_pbch PRIVATE -Wall)

add_executable(test_ra_rnti tests/test_ra_rnti.cpp)
target_link_libraries(test_ra_rnti PRIVATE ra_spoof_lib)
target_compile_options(test_ra_rnti PRIVATE -Wall)


