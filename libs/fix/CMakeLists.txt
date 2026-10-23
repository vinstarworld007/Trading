add_library(lab_fix INTERFACE)
add_library(lab::fix ALIAS lab_fix)

target_include_directories(lab_fix INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_fix INTERFACE lab::core lab::net)
