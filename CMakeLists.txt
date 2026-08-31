cmake_minimum_required(VERSION 3.16)
project(tclzmachine VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TCLZMACHINE_BUILD_TESTS "Build tests" ON)

find_package(TCL 8.6 REQUIRED)

set(TCLZMACHINE_VM_SOURCES
    src/zmachine.c
    src/zmachine_decode.c
    src/zmachine_dispatch.c
    src/zmachine_exec.c
    src/zmachine_file.c
    src/zmachine_input.c
    src/zmachine_object.c
    src/zmachine_property.c
    src/zmachine_quetzal.c
    src/zmachine_state.c
    src/zmachine_text.c
    src/zmachine_tokenise.c
    src/zmachine_undo.c
    src/zmachine_version.c
    src/zmachine_wrap.c
)

#
# Instruction execution is layered so each module owns one concern:
#   zmachine_file.c       public zmachine_step(), cooperative file requests
#   zmachine_tokenise.c   non-blocking lexical analysis opcode
#   zmachine_dispatch.c   presentation/text-only compatibility wrapper
#   zmachine_exec.c       ordinary core opcode executor
# Rename the two lower implementations at compile time rather than duplicating
# their APIs or mixing file/presentation policy into the core executor.
#
set_source_files_properties(src/zmachine_exec.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_core"
)
set_source_files_properties(src/zmachine_dispatch.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_present"
)

add_library(tclzmachine SHARED
    src/tcl_extension.c
    ${TCLZMACHINE_VM_SOURCES}
)

target_include_directories(tclzmachine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
target_link_libraries(tclzmachine PRIVATE ${TCL_LIBRARY})

if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(tclzmachine PRIVATE -Wall -Wextra -Wpedantic)
endif()

set_target_properties(tclzmachine PROPERTIES PREFIX "" OUTPUT_NAME "tclzmachine")
include(GNUInstallDirs)
install(TARGETS tclzmachine LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0 RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/cmake/pkgIndex.tcl.in ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pkgIndex.tcl DESTINATION ${CMAKE_INSTALL_LIBDIR}/tclzmachine0.2.0)

if(TCLZMACHINE_BUILD_TESTS)
    enable_testing()

    add_executable(version-layout-test tests/version_layout.c src/zmachine_version.c)
    target_include_directories(version-layout-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME version-layout COMMAND version-layout-test)

    add_executable(instruction-decode-test tests/instruction_decode.c src/zmachine_decode.c)
    target_include_directories(instruction-decode-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
    add_test(NAME instruction-decode COMMAND instruction-decode-test)

    add_executable(vm-state-test tests/vm_state.c src/zmachine_state.c)
    target_include_directories(vm-state-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(vm-state-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME vm-state COMMAND vm-state-test)

    add_executable(object-test tests/object.c src/zmachine_object.c)
    target_include_directories(object-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(object-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME object-subsystem COMMAND object-test)

    add_executable(text-test tests/text.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(text-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(text-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME text-decoder COMMAND text-test)

    add_executable(vm-exec-test tests/vm_exec.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(vm-exec-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(vm-exec-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME vm-exec COMMAND vm-exec-test)

    add_executable(object-exec-test tests/object_exec.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(object-exec-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(object-exec-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME object-opcodes COMMAND object-exec-test)

    add_executable(text-exec-test tests/text_exec.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(text-exec-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(text-exec-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME text-opcodes COMMAND text-exec-test)

    add_executable(input-run-test tests/input_run.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(input-run-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(input-run-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME input-run-loop COMMAND input-run-test)

    add_executable(core-compat-test tests/core_compat.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(core-compat-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(core-compat-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME core-compat-opcodes COMMAND core-compat-test)

    add_executable(property-length-test tests/property_length.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(property-length-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(property-length-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME property-length COMMAND property-length-test)

    add_executable(undo-test tests/undo.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(undo-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(undo-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME undo-state COMMAND undo-test)

    add_executable(quetzal-test tests/quetzal.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(quetzal-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(quetzal-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME quetzal-save-restore COMMAND quetzal-test)

    add_executable(wrap-test tests/wrap.c src/zmachine_wrap.c)
    target_include_directories(wrap-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(wrap-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME output-wrap COMMAND wrap-test)

    add_executable(presentation-test tests/presentation.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(presentation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(presentation-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME presentation-opcodes COMMAND presentation-test)

    add_executable(get-child-stack-test tests/get_child_stack.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(get-child-stack-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(get-child-stack-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME get-child-stack-operand COMMAND get-child-stack-test)

    add_executable(tokenise-test tests/tokenise.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(tokenise-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(tokenise-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME tokenise-opcode COMMAND tokenise-test)

    add_test(NAME package-smoke
             COMMAND ${TCL_TCLSH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/smoke.tcl
                     $<TARGET_FILE:tclzmachine>)

    # Run the committed project-owned story as a true Tcl/API integration test.
    # The binary is checked into the repository, so this test does not require
    # an Inform compiler or library on ordinary developer machines.
    add_test(NAME repository-story-integration
             COMMAND ${TCL_TCLSH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.tcl
                     $<TARGET_FILE:tclzmachine>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/games/compiled/tclzmachine-test.z5)
endif()
