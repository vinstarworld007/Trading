add_executable(itch-fh main.cpp)
target_link_libraries(itch-fh PRIVATE lab::book lab::feed lab::core)
