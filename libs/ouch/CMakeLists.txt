add_library(lab_ouch INTERFACE)
add_library(lab::ouch ALIAS lab_ouch)

target_include_directories(lab_ouch INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_ouch INTERFACE lab::core)
