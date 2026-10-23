add_executable(itch-replay main.cpp)
target_link_libraries(itch-replay PRIVATE lab::feed lab::itch lab::core)
