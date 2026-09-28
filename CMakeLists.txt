# ============================================================================
# H5Z-Libpressio  --  LibPressio-backed HDF5 filter plugin (H5Zcomp)
#
# Builds against an EXTERNAL HDF5 that has the RFC-HDFG-2026 blob/class3
# filter API (brtnfld/hdf5, branch `blob`). A stock HDF5 release will not work.
#
#   cmake -S . -B build \
#         -DHDF5_ROOT=/path/to/hdf5-blob-install \
#         -DCMAKE_PREFIX_PATH=/path/to/libpressio-install
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# Produces build/lib/libH5Zcomp.so, which is BOTH
#   - an HDF5 filter plugin (export HDF5_PLUGIN_PATH=build/lib), and
#   - a linkable library exporting H5Pset_comp() & co. (include/H5Zpressio.h)
# ============================================================================
cmake_minimum_required(VERSION 3.18)
project(H5Z_LIBPRESSIO VERSION 0.1.0 LANGUAGES C CXX)

option(BUILD_TESTS    "Build the test suite + benchmarks"               ON)
option(BUILD_EXAMPLES "Build the example programs"                      ON)
option(USE_CUDA       "Tests: enable GPU-codec cases (cuszp, zfp-cuda)" OFF)
set(BENCH_DATA_ROOT "/lcrc/project/ECP-EZ/public/compression" CACHE PATH
    "Root of the SDRBench datasets used by the dataset tests/benchmarks")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Make the build tree usable without LD_LIBRARY_PATH gymnastics.
set(CMAKE_BUILD_RPATH_USE_LINK_PATH ON)
set(CMAKE_INSTALL_RPATH_USE_LINK_PATH ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(HDF5 REQUIRED COMPONENTS C)
find_package(LibPressio REQUIRED)

# Imported HDF5 target (FindHDF5 variables differ between CMake-config and
# h5cc-wrapper discovery; normalise to one interface target).
if(TARGET hdf5::hdf5-shared)
  set(H5ZCOMP_HDF5_TARGET hdf5::hdf5-shared)
elseif(TARGET hdf5::hdf5)
  set(H5ZCOMP_HDF5_TARGET hdf5::hdf5)
else()
  add_library(h5zcomp_hdf5 INTERFACE)
  target_include_directories(h5zcomp_hdf5 INTERFACE ${HDF5_C_INCLUDE_DIRS} ${HDF5_INCLUDE_DIRS})
  target_link_libraries(h5zcomp_hdf5 INTERFACE ${HDF5_C_LIBRARIES} ${HDF5_LIBRARIES})
  set(H5ZCOMP_HDF5_TARGET h5zcomp_hdf5)
endif()
message(STATUS "HDF5 version: ${HDF5_VERSION}  (target ${H5ZCOMP_HDF5_TARGET})")

# ---------------------------------------------------------------------------
# Probe the blob-branch filter API so a wrong HDF5 fails at configure time
# with a clear message instead of 40 compile errors.
# ---------------------------------------------------------------------------
include(CheckCSourceCompiles)
include(CMakePushCheckState)
cmake_push_check_state(RESET)
get_target_property(_h5inc ${H5ZCOMP_HDF5_TARGET} INTERFACE_INCLUDE_DIRECTORIES)
set(CMAKE_REQUIRED_INCLUDES ${_h5inc} ${HDF5_C_INCLUDE_DIRS} ${HDF5_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${H5ZCOMP_HDF5_TARGET})
set(CMAKE_REQUIRED_QUIET ON)
check_c_source_compiles("
#include \"hdf5.h\"
int main(void) {
  H5Z_class3_t c; c.version = H5Z_CLASS3_T_VERS; (void)c;
  return (int)(size_t)&H5Pappend_filter_blob + (int)(size_t)&H5Pget_filter_blob;
}" H5ZCOMP_HAVE_BLOB_API)
check_c_source_compiles("
#include \"hdf5.h\"
int main(void) { H5Z_init_func_t f = 0; H5Z_class3_t c; c.init = f; (void)c; return 0; }"
  H5ZCOMP_HAVE_FILTER_STATE)
cmake_pop_check_state()

if(NOT H5ZCOMP_HAVE_BLOB_API)
  message(FATAL_ERROR
    "The HDF5 found at '${HDF5_INCLUDE_DIRS}' has no H5Z_class3_t / filter-blob API.\n"
    "Build HDF5 from https://github.com/brtnfld/hdf5 (branch `blob`) -- see\n"
    "scripts/build_hdf5_blob.sh -- and pass -DHDF5_ROOT=<that install>.")
endif()
if(H5ZCOMP_HAVE_FILTER_STATE)
  message(STATUS "HDF5 filter API: class3 WITH per-dataset state (post-e77c5db layout)")
else()
  message(STATUS "HDF5 filter API: class3 without per-dataset state (blob@e77c5db layout)")
endif()

# ---------------------------------------------------------------------------
# The filter: one shared object, plugin + linkable API
# ---------------------------------------------------------------------------
add_library(H5Zcomp SHARED src/H5Z_comp.cc)
set_target_properties(H5Zcomp PROPERTIES PREFIX "lib" OUTPUT_NAME "H5Zcomp")
target_include_directories(H5Zcomp PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_link_libraries(H5Zcomp PUBLIC ${H5ZCOMP_HDF5_TARGET} PRIVATE LibPressio::libpressio)
if(H5ZCOMP_HAVE_FILTER_STATE)
  target_compile_definitions(H5Zcomp PRIVATE H5ZCOMP_HAVE_FILTER_STATE=1)
endif()

# GPU codecs: no CUDA is compiled into the filter. Device/host transfers are
# performed by LibPressio's own domain manager inside (de)compress; the filter
# only pulls a device-resident output back to host. Build LibPressio with CUDA
# support to enable GPU codecs (cusz, cuszp, bitcomp, nvcomp, ...).

install(TARGETS H5Zcomp LIBRARY DESTINATION lib/hdf5/plugin)
install(FILES include/H5Zpressio.h DESTINATION include)

if(BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
