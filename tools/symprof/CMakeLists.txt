# symprof — symbolize AFFINEUI_SAMPLER profile dumps against the app's
# PDB (dbghelp). Win32-only by nature; see main.cpp for usage.
add_executable(symprof main.cpp)
if(WIN32)
    target_link_libraries(symprof PRIVATE dbghelp)
endif()
target_compile_features(symprof PRIVATE cxx_std_23)
set_target_properties(symprof PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools")
