add_executable(fixgw main.cpp gateway.cpp)
target_link_libraries(fixgw PRIVATE lab::fix lab::ouch lab::soup lab::net lab::core Threads::Threads)
