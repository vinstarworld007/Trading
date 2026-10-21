add_library(lab_mold INTERFACE)
add_library(lab::mold ALIAS lab_mold)

target_include_directories(lab_mold INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_mold INTERFACE lab::core)
