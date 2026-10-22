add_library(lab_risk INTERFACE)
add_library(lab::risk ALIAS lab_risk)

target_include_directories(lab_risk INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_risk INTERFACE lab::oms)
