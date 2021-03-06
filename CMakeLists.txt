cmake_minimum_required(VERSION 2.8.12)
project(bpftrace)

add_compile_options("-std=c++14")
add_compile_options("-Wno-format-security")
#add_compile_options("-Wall")
#add_compile_options("-Wextra")
#add_compile_options("-Wundef")
#add_compile_options("-Wpointer-arith")
#add_compile_options("-Wcast-align")
#add_compile_options("-Wwrite-strings")
#add_compile_options("-Wcast-qual")
#add_compile_options("-Wswitch-default")
#add_compile_options("-Wswitch-enum")
#add_compile_options("-Wconversion")
#add_compile_options("-Wunreachable-code")
#add_compile_options("-Wformat=2")
#add_compile_options("-Wstrict-overflow=5")
#add_compile_options("-Wdisabled-optimization")

enable_testing()

include(ExternalProject)
ExternalProject_Add(bcc
  GIT_REPOSITORY https://github.com/iovisor/bcc
  STEP_TARGETS build update
  EXCLUDE_FROM_ALL 1
  UPDATE_DISCONNECTED 1
  BUILD_COMMAND ${CMAKE_COMMAND} --build . --target bcc-static
)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
find_package(LibElf REQUIRED)
include_directories(${LIBELF_INCLUDE_DIRS})

find_package(BISON REQUIRED)
find_package(FLEX REQUIRED)
bison_target(bison_parser src/parser.yy ${CMAKE_BINARY_DIR}/parser.tab.cc)
flex_target(flex_lexer src/lexer.l ${CMAKE_BINARY_DIR}/lex.yy.cc)
add_flex_bison_dependency(flex_lexer bison_parser)
add_library(parser ${BISON_bison_parser_OUTPUTS} ${FLEX_flex_lexer_OUTPUTS})
target_include_directories(parser PUBLIC src ${CMAKE_BINARY_DIR})

add_subdirectory(src/arch)
add_subdirectory(src)
add_subdirectory(tests)
