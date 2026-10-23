add_executable(order-trace main.cpp)
target_include_directories(order-trace PRIVATE ${PROJECT_SOURCE_DIR}/exchange/simex)
target_link_libraries(order-trace PRIVATE lab::strategy lab::risk lab::oms lab::match lab::ouch
                                          lab::soup lab::mold lab::fix lab::book lab::itch lab::core)
