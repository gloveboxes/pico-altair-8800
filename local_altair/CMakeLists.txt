cmake_minimum_required(VERSION 3.16)
project(altair_local_altair C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL QUIET)
find_package(Threads REQUIRED)

add_executable(altair-local
    main.c
    host_platform.c
    ../ansi_input.c
    io_ports.c
    ../src/pico/PortDrivers/host_chat_io.c
    ../src/pico/PortDrivers/host_files_io.c
    ../src/pico/PortDrivers/time_io.c
    ../src/pico/PortDrivers/utility_io.c
    ../Altair8800/universal_88dcdd.c
    ../Altair8800/intel8080.c
    ../Altair8800/memory.c
)

target_include_directories(altair-local PRIVATE
    .
    ..
    ../Altair8800
    ../src/pico
    ../src/pico/PortDrivers
)

target_compile_definitions(altair-local PRIVATE
    LOCAL_RUNNER_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/.."
)

target_link_libraries(altair-local PRIVATE Threads::Threads)

if(CURL_FOUND)
    target_compile_definitions(altair-local PRIVATE HAVE_LIBCURL=1)
    target_link_libraries(altair-local PRIVATE CURL::libcurl)
    message(STATUS "altair-local: libcurl found, OpenAI chat port enabled")
else()
    message(STATUS "altair-local: libcurl NOT found, OpenAI chat port disabled")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-local PRIVATE
        -Wall
        -Wextra
        -Wno-missing-field-initializers
    )
elseif(MSVC)
    target_compile_options(altair-local PRIVATE /W4)
endif()
