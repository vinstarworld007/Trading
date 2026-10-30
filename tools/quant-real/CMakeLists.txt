add_executable(quant-real main.cpp)
target_link_libraries(quant-real PRIVATE lab::strategy lab::risk lab::oms lab::book lab::itch lab::core)
