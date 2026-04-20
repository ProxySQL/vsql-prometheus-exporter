cmake_minimum_required(VERSION 3.16)
project(vsql_prometheus_exporter_extension)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(EXTENSION_NAME "prometheus_exporter")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# This extension uses pre-release ABI features (InstallResult, run_query,
# background_thread). Build against include-dev/ when available.
set(VillageSQL_USE_DEV_HEADERS ON)
find_package(VillageSQL REQUIRED)

message(STATUS "VillageSQL SDK found at: ${VillageSQL_PREFIX}")
message(STATUS "VillageSQL include dir: ${VillageSQL_INCLUDE_DIR}")

add_library(extension SHARED
    src/extension.cc
)

target_include_directories(extension PRIVATE
    ${VillageSQL_INCLUDE_DIR}
)

VEF_CREATE_VEB(
    NAME ${EXTENSION_NAME}
    LIBRARY_TARGET extension
    MANIFEST ${CMAKE_CURRENT_SOURCE_DIR}/manifest.json
)
