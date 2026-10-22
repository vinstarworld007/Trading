add_library(lab_match INTERFACE)
add_library(lab::match ALIAS lab_match)

target_include_directories(lab_match INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_match INTERFACE lab::core)
