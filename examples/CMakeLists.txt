foreach(ex ex_roundtrip ex_config_size ex_large_config
           ex_native_gpu ex_decoupled_chunks ex_progressive)
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
