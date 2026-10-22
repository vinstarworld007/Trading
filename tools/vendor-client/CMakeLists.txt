add_executable(vendor-client main.cpp)
target_link_libraries(vendor-client PRIVATE lab::vendor lab::feed lab::soup lab::net lab::core)
