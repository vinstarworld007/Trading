add_library(lab_strategy INTERFACE)
add_library(lab::strategy ALIAS lab_strategy)

target_include_directories(lab_strategy INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_strategy INTERFACE lab::oms)
