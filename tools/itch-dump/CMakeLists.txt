add_executable(itch-dump main.cpp)
target_link_libraries(itch-dump PRIVATE lab::book lab::itch lab::core)
