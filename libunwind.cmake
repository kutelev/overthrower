if(NOT EXISTS "${CMAKE_BINARY_DIR}/libunwind")
    execute_process(
        COMMAND
            git clone -b v1.4.0 --depth 1 https://github.com/libunwind/libunwind.git libunwind
        WORKING_DIRECTORY
            "${CMAKE_BINARY_DIR}"
        RESULTS_VARIABLE
            RET_CODE
    )
    if(NOT "${RET_CODE}" STREQUAL "0")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/libunwind")
        message(FATAL_ERROR "Failed to clone libunwind")
    endif()
    execute_process(
        COMMAND
            ./autogen.sh
        WORKING_DIRECTORY
            "${CMAKE_BINARY_DIR}/libunwind"
        RESULTS_VARIABLE
            RET_CODE
    )
    if(NOT "${RET_CODE}" STREQUAL "0")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/libunwind")
        message(FATAL_ERROR "Failed to configure libunwind")
    endif()
    execute_process(
        COMMAND
            ./configure CFLAGS=-fPIC CXXFLAGS=-fPIC
        WORKING_DIRECTORY
            "${CMAKE_BINARY_DIR}/libunwind"
        RESULTS_VARIABLE
            RET_CODE
    )
    if(NOT "${RET_CODE}" STREQUAL "0")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/libunwind")
        message(FATAL_ERROR "Failed to configure libunwind")
    endif()
    execute_process(
        COMMAND
            make install prefix="${CMAKE_BINARY_DIR}/libunwind/usr/local"
        WORKING_DIRECTORY
            "${CMAKE_BINARY_DIR}/libunwind"
        RESULTS_VARIABLE
            RET_CODE
    )
    if(NOT "${RET_CODE}" STREQUAL "0")
        file(REMOVE_RECURSE "${CMAKE_BINARY_DIR}/libunwind")
        message(FATAL_ERROR "Failed to build libunwind")
    endif()
endif()

add_library(unwind STATIC IMPORTED)
set_target_properties(
    unwind
    PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES
            "${CMAKE_BINARY_DIR}/libunwind/include"
        IMPORTED_LOCATION
            "${CMAKE_BINARY_DIR}/libunwind/usr/local/lib/libunwind.a"
        INTERFACE_LINK_LIBRARIES
            "${CMAKE_BINARY_DIR}/libunwind/usr/local/lib/libunwind-x86_64.a"
)
