# Shared warning, tuning and sanitizer flags. Every lab target links lab_options.
add_library(lab_options INTERFACE)
add_library(lab::options ALIAS lab_options)

target_compile_options(lab_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough -Wsign-compare)

if(LAB_WERROR)
  target_compile_options(lab_options INTERFACE -Werror)
endif()

if(LAB_NATIVE)
  target_compile_options(lab_options INTERFACE -march=native)
endif()

if(LAB_SANITIZE)
  target_compile_options(lab_options INTERFACE -fsanitize=${LAB_SANITIZE} -fno-omit-frame-pointer)
  target_link_options(lab_options INTERFACE -fsanitize=${LAB_SANITIZE})
  target_compile_definitions(lab_options INTERFACE LAB_SANITIZED=1)
endif()
