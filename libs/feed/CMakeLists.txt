add_library(lab_feed src/publisher.cpp src/receiver.cpp)
add_library(lab::feed ALIAS lab_feed)

target_include_directories(lab_feed PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_feed PUBLIC lab::mold lab::net lab::core)
