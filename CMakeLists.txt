cmake_minimum_required(VERSION 3.16)
project(tclzmachine VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TCLZMACHINE_BUILD_TESTS "Build tests" ON)

find_package(TCL 8.6 REQUIRED)

add_library(tclzmachine SHARED
    src/tcl_extension.c
    src/zmachine.c
    src/zmachine_decode.c
    src/zmachine_exec.c
    src/zmachine_object.c
    src/zmachine_state.c
    src/zmachine_version.c
)

target_include_directories(tclzmachine
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${TCL_INCLUDE_PATH}
)

target_link_libraries(tclzmachine PRIVATE ${TCL_LIBRARY})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tclzmachine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(tclzmachine PROPERTIES
    PREFIX ""
    OUTPUT_NAME "tclzmachine"
)

include(GNUInstallDirs)

install(TARGETS tclzmachine
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgIndex.tcl.in
    ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0
)

if(TCLZMACHINE_BUILD_TESTS)
    enable_testing()

    add_executable(version-layout-test
        tests/version_layout.c
        src/zmachine_version.c
    )
    target_include_directories(version-layout-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME version-layout COMMAND version-layout-test)

    add_executable(instruction-decode-test
        tests/instruction_decode.c
        src/zmachine_decode.c
    )
    target_include_directories(instruction-decode-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME instruction-decode COMMAND instruction-decode-test)

    add_executable(vm-state-test
        tests/vm_state.c
        src/zmachine_state.c
    )
    target_include_directories(vm-state-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${TCL_INCLUDE_PATH}
    )
    target_link_libraries(vm-state-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME vm-state COMMAND vm-state-test)

    add_executable(object-test
        tests/object.c
        src/zmachine_object.c
    )
    target_include_directories(object-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${TCL_INCLUDE_PATH}
    )
    target_link_libraries(object-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME object-subsystem COMMAND object-test)

    add_executable(vm-exec-test
        tests/vm_exec.c
        src/zmachine.c
        src/zmachine_decode.c
        src/zmachine_exec.c
        src/zmachine_object.c
        src/zmachine_state.c
        src/zmachine_version.c
    )
    target_include_directories(vm-exec-test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${TCL_INCLUDE_PATH}
    )
    target_link_libraries(vm-exec-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME vm-exec COMMAND vm-exec-test)

    add_test(
        NAME package-smoke
        COMMAND ${TCL_TCLSH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.tcl $<TARGET_FILE:tclzmachine>
    )
endif()
