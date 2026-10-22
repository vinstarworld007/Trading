add_executable(ouch-client main.cpp)
target_link_libraries(ouch-client PRIVATE lab::ouch lab::soup lab::net lab::itch lab::core)
