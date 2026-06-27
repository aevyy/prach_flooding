cmake_minimum_required(VERSION 3.16)
project(ra-spoof LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Embed runtime library path so libsrsran_rf*.so is found without LD_LIBRARY_PATH
set(SNIFFER_RF_RPATH "/home/avi/wireless/ssb_Spoofer/build/lib/src/phy/rf")
set(CMAKE_BUILD_RPATH "${SNIFFER_RF_RPATH}")
set(CMAKE_INSTALL_RPATH "${SNIFFER_RF_RPATH}")
set(CMAKE_BUILD_WITH_INSTALL_RPATH TRUE)

find_package(PkgConfig REQUIRED)

# ---------------------------------------------------------------------------
# Find srsRAN
# ---------------------------------------------------------------------------
# Headers are installed at /usr/local/include/srsran/ (from srsRAN Project)
# Pre-built static libraries come from the 5g-sniffer build (monolithic)
# ---------------------------------------------------------------------------
if(NOT EXISTS "/usr/local/include/srsran/srsran.h")
  message(FATAL_ERROR "srsRAN headers not found at /usr/local/include/srsran/")
endif()

set(SNIFFER_BUILD "/home/avi/wireless/ssb_Spoofer/build")
set(SRSRAN_BUILD  "/home/avi/wireless/srsRAN_Project/build")

# PHY library (monolithic, from ssb_Spoofer)
if(NOT EXISTS "${SNIFFER_BUILD}/lib/src/phy/libsrsran_phy.a")
  message(FATAL_ERROR "libsrsran_phy.a not found at ${SNIFFER_BUILD}/lib/src/phy/")
endif()
# Whole-archive to resolve circular deps; allow-multiple-definition for duplicate symbols
set(SRSRAN_PHY_LINK_LIBS
  "-Wl,--allow-multiple-definition"
  "-Wl,--whole-archive" "${SNIFFER_BUILD}/lib/src/phy/libsrsran_phy.a" "-Wl,--no-whole-archive"
)

# Common library (from ssb_Spoofer) - normal linking
if(NOT EXISTS "${SNIFFER_BUILD}/lib/src/common/libsrsran_common.a")
  message(FATAL_ERROR "libsrsran_common.a not found at ${SNIFFER_BUILD}/lib/src/common/")
endif()
set(SRSRAN_COMMON_LINK_LIBS "${SNIFFER_BUILD}/lib/src/common/libsrsran_common.a")

# srsran_rf (shared library for RF abstraction — UHD plugin loaded at runtime)
set(SRSRAN_RF_LIB "${SNIFFER_BUILD}/lib/src/phy/rf/libsrsran_rf.so")

# srslog library (provides fmt::v7 symbols used by phy/common)
if(EXISTS "${SNIFFER_BUILD}/lib/src/srslog/libsrslog.a")
  set(SRSRAN_SRSLOG_LIBS "${SNIFFER_BUILD}/lib/src/srslog/libsrslog.a")
else()
  set(SRSRAN_SRSLOG_LIBS "")
endif()

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

# MAC library (TODO M2: from srsRAN_Project, requires fmt/srslog deps)

# yaml-cpp
find_package(yaml-cpp REQUIRED)

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
  src/ul_rx.cpp
  src/dl_grid.cpp
  src/rar.cpp
  src/msg4.cpp
  src/inject.cpp
  src/influx_reader.cpp
  src/prach_tx.cpp
  # NOTE: rt-recon-sdk used header-only (influxdb.hpp flux_query + local parse logic)
  # SDK .cc files are NOT compiled here — they drag in srslog/fmt7 which breaks GCC 15
)

add_library(ra_spoof_lib STATIC ${LIB_SOURCES})

target_include_directories(ra_spoof_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  /usr/local/include
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

add_executable(test_pdcch_loopback tests/test_pdcch_loopback.cpp)
target_link_libraries(test_pdcch_loopback PRIVATE
  ra_spoof_lib
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
)
target_compile_options(test_pdcch_loopback PRIVATE -Wall)

add_executable(test_pdsch_loopback tests/test_pdsch_loopback.cpp)
target_link_libraries(test_pdsch_loopback PRIVATE
  ra_spoof_lib
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
)
target_compile_options(test_pdsch_loopback PRIVATE -Wall)

add_executable(test_prach_loopback tests/test_prach_loopback.cpp)
target_link_libraries(test_prach_loopback PRIVATE
  ra_spoof_lib
  ${SRSRAN_PHY_LINK_LIBS}
  ${SRSRAN_COMMON_LINK_LIBS}
  ${SRSRAN_COMMON_DEPS_LIBS}
)
target_compile_options(test_prach_loopback PRIVATE -Wall)
