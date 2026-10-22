add_executable(fix-client main.cpp)
target_link_libraries(fix-client PRIVATE lab::fix lab::net lab::core)
