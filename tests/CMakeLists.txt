# ============================================================================
# H5Zcomp test suite -- mirrors vol-external-passthrough/tests
#
#   VOL repo                    here                      ctest label
#   ------------------------    ------------------------  -----------
#   test_ci.c                   test_ci.c                 ci
#   test_cpu_write.c            test_cpu_write.c          ci
#   test_error_miranda.c        test_error_miranda.c      ci   (synthetic fallback)
#   test_time_miranda.c         test_time_miranda.c       data (skips w/o SDRBench)
#   test_time_hurricane.c       test_time_hurricane.c     data;gpu
#   bench_filter_timing.cc      bench_comp_timing.cc      (PBS jobs, not ctest)
#   bench_config.h/timing.h     verbatim copies
#   tier2_h5repack_roundtrip    test_plugin_read.c        ci   (plugin autoload)
#   --                          test_large_config.c       ci   (blob + DXPL path)
#
#   ctest -L ci        fast, no data needed (laptop / login node)
#   ctest -L data      needs BENCH_DATA_ROOT (SDRBench) -- run on the cluster
# ============================================================================

set(H5ZC_TEST_LIBS H5Zcomp LibPressio::libpressio m)

function(h5zc_test name src)
  add_executable(${name} ${src})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(${name} PRIVATE BENCH_DATA_ROOT="${BENCH_DATA_ROOT}")
  target_link_libraries(${name} PRIVATE ${H5ZC_TEST_LIBS})
endfunction()

h5zc_test(test_ci              test_ci.c)
h5zc_test(test_cpu_write       test_cpu_write.c)
h5zc_test(test_error_miranda   test_error_miranda.c)
h5zc_test(test_large_config    test_large_config.c)
h5zc_test(test_time_miranda    test_time_miranda.c)
h5zc_test(test_time_hurricane  test_time_hurricane.c)
h5zc_test(plugin_fixture_write plugin_fixture_write.c)

# Reader intentionally NOT linked to H5Zcomp / LibPressio: it can only decode
# the fixture if HDF5 loads the plugin from HDF5_PLUGIN_PATH.
add_executable(test_plugin_read test_plugin_read.c)
target_link_libraries(test_plugin_read PRIVATE ${H5ZCOMP_HDF5_TARGET} m)

# Benchmark harness (codec-matched to bench_vol_timing)
add_executable(bench_comp_timing bench_comp_timing.cc)
target_include_directories(bench_comp_timing PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(bench_comp_timing PRIVATE BENCH_DATA_ROOT="${BENCH_DATA_ROOT}")
target_link_libraries(bench_comp_timing PRIVATE ${H5ZC_TEST_LIBS})
target_link_options(bench_comp_timing PRIVATE -rdynamic)

# ---------------------------------------------------------------------------
# ctest registrations. Each test runs in its own directory so the .h5 files
# never collide when ctest -j is used.
# ---------------------------------------------------------------------------
set(H5ZC_PLUGIN_DIR ${CMAKE_LIBRARY_OUTPUT_DIRECTORY})

function(h5zc_add name)
  cmake_parse_arguments(A "" "" "COMMAND;LABELS;ENV" ${ARGN})
  set(wd ${CMAKE_CURRENT_BINARY_DIR}/run/${name})
  file(MAKE_DIRECTORY ${wd})
  add_test(NAME ${name} COMMAND ${A_COMMAND} WORKING_DIRECTORY ${wd})
  set_tests_properties(${name} PROPERTIES LABELS "${A_LABELS}" SKIP_RETURN_CODE 77
                       ENVIRONMENT "HDF5_PLUGIN_PATH=${H5ZC_PLUGIN_DIR};${A_ENV}")
endfunction()

h5zc_add(ci.roundtrip       COMMAND $<TARGET_FILE:test_ci>            LABELS ci)
h5zc_add(ci.cpu_write       COMMAND $<TARGET_FILE:test_cpu_write>     LABELS ci)
h5zc_add(ci.errors          COMMAND $<TARGET_FILE:test_error_miranda> LABELS ci)
h5zc_add(ci.large_config    COMMAND $<TARGET_FILE:test_large_config>  LABELS ci)
h5zc_add(ci.errors_chunked  COMMAND $<TARGET_FILE:test_error_miranda> LABELS ci ENV H5ZCOMP_CHUNK_N=8)

# plugin autoload: fixture writer (linked) -> reader (plugin path only)
set(_fx ${CMAKE_CURRENT_BINARY_DIR}/run/plugin_fixture.h5)
add_test(NAME ci.plugin_fixture COMMAND $<TARGET_FILE:plugin_fixture_write> ${_fx})
set_tests_properties(ci.plugin_fixture PROPERTIES LABELS ci FIXTURES_SETUP plugin_fx)
add_test(NAME ci.plugin_autoload COMMAND $<TARGET_FILE:test_plugin_read> ${_fx})
set_tests_properties(ci.plugin_autoload PROPERTIES LABELS ci FIXTURES_REQUIRED plugin_fx
                     ENVIRONMENT "HDF5_PLUGIN_PATH=${H5ZC_PLUGIN_DIR}")

# examples as smoke tests
if(BUILD_EXAMPLES)
  h5zc_add(ci.ex_roundtrip_noop COMMAND $<TARGET_FILE:ex_roundtrip> noop "{}" LABELS ci)
  h5zc_add(ci.ex_large_config   COMMAND $<TARGET_FILE:ex_large_config>       LABELS ci)
endif()

# harness sanity: lists the matrix + which codecs/datasets are reachable
h5zc_add(ci.bench_list COMMAND $<TARGET_FILE:bench_comp_timing> --list LABELS ci)

# dataset-backed (skip with code 77 when SDRBench is not mounted)
h5zc_add(data.miranda_timing         COMMAND $<TARGET_FILE:test_time_miranda> LABELS data)
h5zc_add(data.miranda_timing_8chunks COMMAND $<TARGET_FILE:test_time_miranda> LABELS data ENV H5ZCOMP_CHUNK_N=8)
h5zc_add(data.miranda_bench_smoke
         COMMAND $<TARGET_FILE:bench_comp_timing> --only miranda --comp sz3_1e3
                 --out smoke.csv --ext smoke.ext.csv
         LABELS data ENV BENCH_FSYNC=0 BENCH_DROP_CACHE=0)
if(USE_CUDA)
  # one process per GPU codec: a CUDA fault is sticky for the whole process
  foreach(gc cuszp cusz nvcomp)
    h5zc_add(data.hurricane_${gc} COMMAND $<TARGET_FILE:test_time_hurricane> ${gc} LABELS "data;gpu")
  endforeach()
  # Nonclean CLOUDf01 (what the VOL test used): NaN/Inf input breaks the
  # error bound. Expected to fail -- a documented finding, not a regression.
  h5zc_add(data.hurricane_nonclean_cuszp COMMAND $<TARGET_FILE:test_time_hurricane> cuszp
           LABELS "data;gpu"
           ENV H5ZCOMP_HURRICANE_FILE=${BENCH_DATA_ROOT}/Hurricane-ISABEL/nonclean-data/CLOUDf01.bin)
  set_tests_properties(data.hurricane_nonclean_cuszp PROPERTIES WILL_FAIL TRUE)
  # GPU sanity on clean data (Miranda density), cuszp only, 1 and 8 chunks
  h5zc_add(gpu.miranda_cuszp
           COMMAND $<TARGET_FILE:bench_comp_timing> --only miranda --comp cuszp_1e3
                   --out gpu.csv --ext gpu.ext.csv
           LABELS gpu ENV BENCH_FSYNC=0 BENCH_DROP_CACHE=0)
  h5zc_add(gpu.miranda_cuszp_8chunks
           COMMAND $<TARGET_FILE:bench_comp_timing> --only miranda --comp cuszp_1e3 --chunk-n 8
                   --out gpu8.csv --ext gpu8.ext.csv
           LABELS gpu ENV BENCH_FSYNC=0 BENCH_DROP_CACHE=0)
endif()
set_tests_properties(data.miranda_timing data.miranda_timing_8chunks PROPERTIES TIMEOUT 1800)
