foreach(ex ex_roundtrip ex_config_size ex_large_config)
  add_executable(${ex} ${ex}.c)
  target_link_libraries(${ex} PRIVATE H5Zcomp m)
endforeach()
