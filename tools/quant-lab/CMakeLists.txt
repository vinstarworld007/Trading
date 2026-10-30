add_executable(quant-lab main.cpp)
target_link_libraries(quant-lab PRIVATE lab::strategy lab::risk lab::oms lab::match lab::book lab::itch
                                        lab::core)
