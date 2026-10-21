add_executable(itch-replay main.cpp)
target_link_libraries(itch-replay PRIVATE lab::mold lab::net lab::itch lab::core)
