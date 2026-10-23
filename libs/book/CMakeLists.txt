add_library(lab_book src/book_builder.cpp src/report.cpp)
add_library(lab::book ALIAS lab_book)

target_include_directories(lab_book PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(lab_book PUBLIC lab::itch lab::core)
