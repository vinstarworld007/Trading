add_executable(agents main.cpp)
target_link_libraries(agents PRIVATE lab::ouch lab::soup lab::net lab::core)
