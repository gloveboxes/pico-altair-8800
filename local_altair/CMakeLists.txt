cmake_minimum_required(VERSION 3.16)
project(altair_local_altair C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(altair-local
    main.c
    ../ansi_input.c
    io_ports.c
    ../PortDrivers/host_files_io.c
    ../PortDrivers/time_io.c
    ../PortDrivers/utility_io.c
    ../Altair8800/universal_88dcdd.c
    ../Altair8800/intel8080.c
    ../Altair8800/memory.c
)

target_include_directories(altair-local PRIVATE
    .
    ..
    ../Altair8800
    ../PortDrivers
)

target_compile_definitions(altair-local PRIVATE
    LOCAL_RUNNER_REPO_ROOT="${CMAKE_CURRENT_LIST_DIR}/.."
)

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(altair-local PRIVATE
        -Wall
        -Wextra
        -Wno-missing-field-initializers
    )
endif()
