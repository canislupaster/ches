cmake_minimum_required(VERSION 3.17)
project(termchess C)

set(CMAKE_C_STANDARD 11)

add_subdirectory(corecommon)
include_directories(corecommon/src)

add_executable(termchess src/chess.c src/network.c src/main.c)
add_executable(termchess_server src/chess.c src/network.c src/server.c)

add_custom_target(genheader_termchess WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND headergen ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_dependencies(termchess genheader_termchess corecommon)
add_dependencies(termchess_server genheader_termchess corecommon)

set(CURSES_NEED_NCURSES TRUE)
set(CURSES_NEED_WIDE TRUE)

find_package(Curses REQUIRED)
include_directories(${CURSES_INCLUDE_DIR})
target_link_libraries(termchess ${CURSES_LIBRARIES} corecommon)
target_link_libraries(termchess_server corecommon)
