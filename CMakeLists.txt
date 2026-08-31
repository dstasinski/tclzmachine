cmake_minimum_required(VERSION 3.16)
project(tclzmachine VERSION 0.2.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(TCLZMACHINE_BUILD_TESTS "Build tests" ON)

find_package(TCL 8.6 REQUIRED)

set(TCLZMACHINE_VM_SOURCES
    src/zmachine.c
    src/zmachine_run.c
    src/zmachine_preflight.c
    src/zmachine_stream.c
    src/zmachine_decode.c
    src/zmachine_mirc.c
    src/zmachine_dispatch.c
    src/zmachine_key.c
    src/zmachine_core_extra.c
    src/zmachine_exec.c
    src/zmachine_file_aux.c
    src/zmachine_file.c
    src/zmachine_header.c
    src/zmachine_input.c
    src/zmachine_input_host.c
    src/zmachine_input_preload.c
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
#   zmachine_preflight.c  public step boundary; rejects version/arity errors
#                        before any layer can resolve a variable operand
#   zmachine_stream.c     external replay, transcript, and command-record files
#   zmachine_mirc.c       optional text style/colour state and mIRC rendering
#   zmachine_file_aux.c   V5+ auxiliary byte-region save/restore requests
#   zmachine_file.c       full-game Quetzal file-request base layer
#   zmachine_tokenise.c   non-blocking lexical analysis opcodes
#   zmachine_key.c        exact numeric ZSCII input for read_char
#   zmachine_dispatch.c   remaining text-only compatibility wrapper
#   zmachine_core_extra.c pure control/arithmetic completeness opcodes
#   zmachine_exec.c       original ordinary core opcode executor
# The lower implementations are renamed at compile time so each wrapper can
# delegate downward without duplicating public APIs or mixing host policy into
# ordinary VM execution.
#
set_source_files_properties(src/zmachine_exec.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_core_base"
)
set_source_files_properties(src/zmachine_dispatch.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_visual_base"
)
set_source_files_properties(src/zmachine_file.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_file_base;zmachine_save_file=zmachine_save_file_base;zmachine_restore_file=zmachine_restore_file_base;zmachine_cancel_file=zmachine_cancel_file_base"
)
set_source_files_properties(src/zmachine_file_aux.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_stream_base"
)

#
# The form-aware cooperative loop lives in zmachine_run.c. The external stream
# wrapper still owns replay accumulation and host FILE lifetimes, while the mIRC
# layer wraps its public run/lifetime hooks so presentation output is cleared once
# per Tcl command and story-controlled style state is reset on load/restart/reset.
# The stream step entry point is renamed beneath zmachine_preflight.c so every
# execution path receives version/arity checks before stream or presentation
# policy can resolve operands. Canonical output remains in zmachine.c; its private
# append implementation is mirrored by zmachine_mirc.c before returning through
# the transcript wrapper.
#
set_source_files_properties(src/zmachine_run.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_run=zmachine_run_stream_base"
)
set_source_files_properties(src/zmachine_stream.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_step=zmachine_step_preflight_base;zmachine_run=zmachine_run_mirc_base;zmachine_destroy=zmachine_destroy_mirc_base;zmachine_load_story=zmachine_load_story_mirc_base;zmachine_reset=zmachine_reset_mirc_base;zmachine_stream_after_restart=zmachine_stream_after_restart_mirc_base"
)
set_source_files_properties(src/zmachine.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_run=zmachine_run_legacy;zmachine_supply_input=zmachine_supply_input_base;zmachine_output_append=zmachine_output_append_plain_base;zmachine_destroy=zmachine_destroy_stream_base;zmachine_load_story=zmachine_load_story_stream_base;zmachine_reset=zmachine_reset_stream_base"
)

#
# V5+ read/aread may begin with characters already present in the text buffer.
# Keep the mature writer/tokenizer as the base implementation, let the preload
# wrapper merge that prefix, and let zmachine_stream.c add transcript/recording
# side effects only after the final story buffer is known.
#
set_source_files_properties(src/zmachine_input.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_input_read_line=zmachine_input_read_line_base"
)
set_source_files_properties(src/zmachine_input_preload.c PROPERTIES
    COMPILE_DEFINITIONS "zmachine_input_read_line=zmachine_input_read_line_stream_base"
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

    add_executable(header-test tests/header.c src/zmachine_header.c)
    target_include_directories(header-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    add_test(NAME interpreter-header COMMAND header-test)

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

    add_executable(input-preload-test tests/input_preload.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(input-preload-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(input-preload-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME input-preload COMMAND input-preload-test)

    add_executable(run-form-dispatch-test tests/run_form_dispatch.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(run-form-dispatch-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(run-form-dispatch-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME run-form-dispatch COMMAND run-form-dispatch-test)

    add_executable(core-compat-test tests/core_compat.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(core-compat-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(core-compat-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME core-compat-opcodes COMMAND core-compat-test)

    add_executable(core-extra-test tests/core_extra.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(core-extra-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(core-extra-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME core-extra-opcodes COMMAND core-extra-test)

    add_executable(version-gate-side-effects-test tests/version_gate_side_effects.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(version-gate-side-effects-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(version-gate-side-effects-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME version-gate-side-effects COMMAND version-gate-side-effects-test)

    add_executable(arity-side-effects-test tests/arity_side_effects.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(arity-side-effects-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(arity-side-effects-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME arity-side-effects COMMAND arity-side-effects-test)

    add_executable(mirc-presentation-test tests/mirc_presentation.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(mirc-presentation-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(mirc-presentation-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME mirc-presentation COMMAND mirc-presentation-test)

    add_executable(io-compat-test tests/io_compat.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(io-compat-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(io-compat-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME embedded-io-compat COMMAND io-compat-test)

    add_executable(stream-io-test tests/stream_io.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(stream-io-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(stream-io-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME external-stream-io COMMAND stream-io-test)

    add_executable(file-aux-test tests/file_aux.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(file-aux-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(file-aux-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME auxiliary-file-io COMMAND file-aux-test)

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

    add_executable(presentation-extended-test tests/presentation_extended.c ${TCLZMACHINE_VM_SOURCES})
    target_include_directories(presentation-extended-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${TCL_INCLUDE_PATH})
    target_link_libraries(presentation-extended-test PRIVATE ${TCL_LIBRARY})
    add_test(NAME presentation-extended-opcodes COMMAND presentation-extended-test)

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

    # Run both committed project-owned stories as true Tcl/API integration
    # tests. The binaries are checked into the repository, so these tests do not
    # require Inform or an Inform library on ordinary developer machines.
    add_test(NAME repository-story-integration
             COMMAND ${TCL_TCLSH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration.tcl
                     $<TARGET_FILE:tclzmachine>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/games/compiled/tclzmachine-test.z5)

    add_test(NAME repository-story-v3-integration
             COMMAND ${TCL_TCLSH} ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_v3.tcl
                     $<TARGET_FILE:tclzmachine>
                     ${CMAKE_CURRENT_SOURCE_DIR}/tests/games/compiled/tclzmachine-v3.z3)
endif()
