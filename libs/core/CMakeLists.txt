add_library(lab_core
  src/affinity.cpp
  src/clock.cpp
  src/histogram.cpp
  src/log.cpp
  src/shm.cpp)
add_library(lab::core ALIAS lab_core)

target_include_directories(lab_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_core PUBLIC lab::options Threads::Threads PRIVATE rt)
