foreach(ex ex_roundtrip ex_config_size ex_large_config
           ex_native_gpu ex_decoupled_chunks ex_progressive
           ex_multi_filter ex_multi_dataset)
  add_executable(${ex} ${ex}.c)
  target_link_libraries(${ex} PRIVATE H5Zcomp m)
endforeach()

# ex_native_gpu hands H5Dwrite/H5Dread a real CUDA device pointer when the CUDA
# toolkit is available (-DUSE_CUDA=ON); otherwise it runs the same code path
# with a host buffer declared as LibPressio domain "malloc".
if(USE_CUDA)
  find_package(CUDAToolkit QUIET)
  if(CUDAToolkit_FOUND)
    target_link_libraries(ex_native_gpu PRIVATE CUDA::cudart)
    target_compile_definitions(ex_native_gpu PRIVATE EX_HAVE_CUDA=1)
    message(STATUS "ex_native_gpu: CUDA device buffers enabled")
  else()
    message(STATUS "ex_native_gpu: CUDA toolkit not found, building the host stand-in")
  endif()
endif()

# ex_parallel needs a parallel HDF5 (H5_HAVE_PARALLEL) and MPI.
if(H5ZCOMP_HDF5_IS_PARALLEL)
  find_package(MPI COMPONENTS C)
  if(MPI_C_FOUND)
    add_executable(ex_parallel ex_parallel.c)
    target_link_libraries(ex_parallel PRIVATE H5Zcomp MPI::MPI_C m)
    message(STATUS "ex_parallel: parallel HDF5 + MPI found, building the MPI example")
  endif()
endif()
