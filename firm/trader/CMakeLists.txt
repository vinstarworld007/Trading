add_executable(trader main.cpp order_entry.cpp trading.cpp)
target_link_libraries(trader PRIVATE lab::strategy lab::risk lab::oms lab::feed lab::book lab::ouch
                                     lab::soup lab::net lab::core Threads::Threads)
