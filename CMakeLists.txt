cmake_minimum_required(VERSION 3.17)
project(termchess C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3")

add_subdirectory(corecommon)
include_directories(corecommon/src)

if (EMSCRIPTEN)
    add_executable(termchess src/chess.c src/network.c src/ai.c src/chessfrontend.c src/imwasm.c src/main_em.c)
    add_dependencies(termchess genheader_termchess corecommon)
else()
    #termchess is gone lmao
    #add_executable(termchess src/chess.c src/network.c src/main.c)
endif()

add_executable(termchess_server src/ai.c src/chess.c src/chessfrontend.c src/network.c src/server.c)

add_custom_target(genheader_termchess WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} COMMAND headergen ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_dependencies(termchess_server genheader_termchess corecommon)

# openssl
if(NOT EMSCRIPTEN)
    if(APPLE)
        set(OPENSSL_ROOT_DIR /usr/local/Cellar/openssl@1.1/1.1.1h/)
        set(OPENSSL_CRYPTO_LIBRARY ${OPENSSL_ROOT_DIR}/lib/libcrypto.dylib CACHE FILEPATH "" FORCE)
        set(OPENSSL_SSL_LIBRARY ${OPENSSL_ROOT_DIR}/lib/libssl.dylib CACHE FILEPATH "" FORCE)
    endif()

    find_package(OpenSSL)
    if (OpenSSL_FOUND)
        include_directories(${OPENSSL_INCLUDE_DIR})
        target_link_libraries(termchess_server ${OPENSSL_CRYPTO_LIBRARY})
    endif()
endif()

if(UNIX)
    set(THREADS_PREFER_PTHREAD_FLAG ON)
    find_package(Threads REQUIRED)

    target_link_libraries(termchess_server Threads::Threads)
endif()

if(EMSCRIPTEN)
    file(COPY ./img DESTINATION ${CMAKE_SOURCE_DIR}/docs/)

    file(GLOB BOARDS ./*.board)
    foreach(BOARD ${BOARDS})
        file(READ ${BOARD} content)
        get_filename_component(BOARD_NAME ${BOARD} NAME)
        string(REPLACE "\n" "\\n" content "${content}")
        set(content "\"${BOARD_NAME}\", \"${content}\"")
        file(WRITE "./include/${BOARD_NAME}" "${content}")
    endforeach()

    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/index.html ${CMAKE_SOURCE_DIR}/docs/index.html COPYONLY)
    set(EMCC_OPTIONS "SHELL:-s ASYNCIFY")
    target_compile_options(termchess PUBLIC ${EMCC_OPTIONS})
    target_link_options(termchess PUBLIC ${EMCC_OPTIONS} "LINKER:--fatal-warnings")

    add_custom_command(TARGET termchess POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_CURRENT_BINARY_DIR}/termchess.wasm ${CMAKE_CURRENT_BINARY_DIR}/termchess.cbp ${CMAKE_CURRENT_BINARY_DIR}/termchess.js ${CMAKE_SOURCE_DIR}/docs/)

    target_link_libraries(termchess corecommon)
endif()

target_link_libraries(termchess_server corecommon)
