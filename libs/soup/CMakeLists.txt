add_library(lab_soup INTERFACE)
add_library(lab::soup ALIAS lab_soup)

target_include_directories(lab_soup INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_soup INTERFACE lab::core)
