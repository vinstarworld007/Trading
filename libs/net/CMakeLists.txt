add_library(lab_net src/udp.cpp)
add_library(lab::net ALIAS lab_net)

target_include_directories(lab_net PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_net PUBLIC lab::core)
