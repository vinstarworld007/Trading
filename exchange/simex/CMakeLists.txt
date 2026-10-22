add_executable(simex main.cpp matcher.cpp gateway.cpp liquidity.cpp)
target_link_libraries(simex PRIVATE lab::match lab::ouch lab::soup lab::feed lab::book lab::itch
                                    lab::net lab::core Threads::Threads)
