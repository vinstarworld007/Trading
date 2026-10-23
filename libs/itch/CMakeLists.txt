find_package(ZLIB REQUIRED)

add_library(lab_itch src/file_reader.cpp src/synthetic.cpp)
add_library(lab::itch ALIAS lab_itch)

target_include_directories(lab_itch PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_itch PUBLIC lab::core PRIVATE ZLIB::ZLIB)
