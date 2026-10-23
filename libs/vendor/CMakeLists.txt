add_library(lab_vendor src/server.cpp src/cli.cpp src/icf_session.cpp src/omm_session.cpp
  src/bpipe_session.cpp)
add_library(lab::vendor ALIAS lab_vendor)

target_include_directories(lab_vendor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_vendor PUBLIC lab::book lab::feed lab::soup lab::net lab::core Threads::Threads)

# GCC 13 reports a false maybe-uninitialized inside std::regex's own
# std::function members when sanitizers are on (SUBSCRIBEWILDCARD uses it).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(src/icf_session.cpp PROPERTIES COMPILE_OPTIONS
    -Wno-maybe-uninitialized)
endif()
