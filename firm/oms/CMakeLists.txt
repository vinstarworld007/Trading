add_library(lab_oms INTERFACE)
add_library(lab::oms ALIAS lab_oms)

target_include_directories(lab_oms INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_oms INTERFACE lab::core)
