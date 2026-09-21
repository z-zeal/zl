cmake_minimum_required(VERSION 3.10)
project(zl_language VERSION 0.1.0 LANGUAGES CXX)

set(ZL_VERSION_STRING "${PROJECT_VERSION}")
# Keep the build self-contained: the release archive intentionally excludes
# the cmake/ helper directory, so generate the stdlib version file directly.
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/stdlib")
file(WRITE "${CMAKE_BINARY_DIR}/stdlib/VERSION" "${ZL_VERSION_STRING}\n")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# windows.h defines min and max as macros unless NOMINMAX is defined, which
# turns std::numeric_limits<T>::max(), std::min and std::max into syntax errors
# (C2589 / C2059) in every translation unit that includes it -- MSVC's error
# points at the innocent call site, not at the macro. Defined here rather than
# per file because a source that included windows.h first would still break.
if(MSVC)
    add_compile_definitions(NOMINMAX)
endif()

# Keep the public headers visible to every target, including standalone tools.
# This also makes the project robust against older/stale per-target CMake state.
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add source files
# Explicit list: a stray .cpp dropped under src/ must be added here on
# purpose rather than silently joining the link (GLOB_RECURSE hid that).
set(SOURCES
    ${CMAKE_SOURCE_DIR}/src/common/error.cpp
    ${CMAKE_SOURCE_DIR}/src/common/type_name.cpp
    ${CMAKE_SOURCE_DIR}/src/common_stdlib_version.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/builtin_library.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/bytecode.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/compilation_plan.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/compiler.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/dispatch_table.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/generic_instantiation.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/ir.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/ir_lowering.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/ir_optimizer.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/machine_code.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/module_graph.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/module_loader.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/native_catalog.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/native_compiler.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/native_ffi_declarations.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/operator_rules.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/overload_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/semantic_model.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/semantic_types.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/thread_capture.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/type_checker.cpp
    ${CMAKE_SOURCE_DIR}/src/compiler/type_resolver.cpp
    ${CMAKE_SOURCE_DIR}/src/lexer/lexer.cpp
    ${CMAKE_SOURCE_DIR}/src/main.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/analysis.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/builder.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/dataflow.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/differential.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/effects.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/folding.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/function.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/instruction.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/lowering.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/opt_constant.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/opt_dead.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/opt_dead_functions.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/opt_simplify.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/pass_manager.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/printer.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/reachability.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/safety.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/ssa.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/type.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/verifier.cpp
    ${CMAKE_SOURCE_DIR}/src/mir/vm_backend.cpp
    ${CMAKE_SOURCE_DIR}/src/native/emit.cpp
    ${CMAKE_SOURCE_DIR}/src/native/exec.cpp
    ${CMAKE_SOURCE_DIR}/src/native/lir.cpp
    ${CMAKE_SOURCE_DIR}/src/native/pipeline.cpp
    ${CMAKE_SOURCE_DIR}/src/native/select.cpp
    ${CMAKE_SOURCE_DIR}/src/native/target.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/ast.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/expression_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/parser/type_lookahead.cpp
    ${CMAKE_SOURCE_DIR}/src/regex/regex.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/execution_state.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/gc.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/gc_safepoint.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/native.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/native_boundary.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/native_library.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/native_resource.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_exception.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_scheduler.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_task.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_task_executor.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_thread.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/runtime_type_checks.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/type_contracts.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/value.cpp
    ${CMAKE_SOURCE_DIR}/src/vm/vm.cpp
)

# Create executable. zlpkg/manifest.cpp is the single TOML parser; --check
# uses it to discover local path dependencies instead of a second regex scan.
add_executable(zl_language ${SOURCES} ${CMAKE_SOURCE_DIR}/zlpkg/manifest.cpp)

# --- Target organization (Objective 8) --------------------------------------
# Everything registered below joins exactly one bucket, and the buckets are the
# only thing the build scripts ever name:
#
#   zl-core  -> the usable ZL toolchain (interpreter/compiler/runtime + the
#               tools that ship with it). This is the default developer build.
#   zl-tests -> zl-core plus every C++ regression executable and its fixtures.
#   zl-full  -> zl-core + zl-tests + optional/auxiliary targets.
#
# ZL_CORE_TARGETS / ZL_TEST_TARGETS / ZL_OPTIONAL_TARGETS are accumulated as the
# file is read and wired into the aggregates at the very bottom, so a new target
# only has to be appended to one list.
set(ZL_CORE_TARGETS "")
set(ZL_TEST_TARGETS "")
set(ZL_OPTIONAL_TARGETS "")

# The ZL executable is the root of the core closure: CMake pulls in every
# object/library it needs, so the core build is the smallest correct closure
# rather than a hand-written binary list.
list(APPEND ZL_CORE_TARGETS zl_language)

# Include directories
target_include_directories(zl_language PRIVATE ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/zlpkg)
target_compile_definitions(zl_language PRIVATE ZL_VERSION_STRING="${ZL_VERSION_STRING}")

if(WIN32)
    # src/vm/native.cpp exposes Network.resolve() through Winsock.
    target_link_libraries(zl_language PRIVATE ws2_32)
endif()

# Bundle stdlib/ next to the built binary, so ZL_STDLIB_ROOT-less runs
# (main.cpp falls back to "<executable's own dir>/stdlib") find it without
# any extra setup, matching how a real install would ship them together.
add_custom_command(TARGET zl_language POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_SOURCE_DIR}/stdlib
            $<TARGET_FILE_DIR:zl_language>/stdlib
    COMMENT "Copying stdlib/ next to zl_language"
)

add_custom_command(TARGET zl_language POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:zl_language>/stdlib
    COMMAND ${CMAKE_COMMAND} -E copy ${CMAKE_BINARY_DIR}/stdlib/VERSION $<TARGET_FILE_DIR:zl_language>/stdlib/VERSION
    COMMENT "Writing stdlib version metadata"
)

# --- zlpkg: the package manager CLI (Phase 6) -------------------------------
# Deliberately its own target/sources under zlpkg/ (NOT under src/) - it's a
# standalone tool that talks to zl_language only as a subprocess (see
# zlpkg/commands.cpp), so it has no business linking against the
# interpreter's own sources/headers. `manifest.cpp` is also compiled into
# zl_language so `--check` uses the same TOML parser.
set(ZLPKG_SOURCES
    ${CMAKE_SOURCE_DIR}/zlpkg/commands.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/git.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/lockfile.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/main.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/manifest.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/process.cpp
    ${CMAKE_SOURCE_DIR}/zlpkg/resolver.cpp
)
add_executable(zlpkg ${ZLPKG_SOURCES})
target_include_directories(zlpkg PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(zlpkg PRIVATE ZL_VERSION_STRING="${ZL_VERSION_STRING}")

# zlpkg finds zl_language by looking next to its own binary at runtime, so
# both need to land in the same output directory.
set_target_properties(zlpkg PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:zl_language>
)
# --- zl-bind: restricted C/C++ header binding generator (Phase 17) ---------
file(GLOB ZLBIND_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/tools/zl-bind/*.cpp)
add_executable(zl-bind ${ZLBIND_SOURCES})
target_include_directories(zl-bind PRIVATE ${CMAKE_SOURCE_DIR}/include)

# zlpkg and zl-bind are part of the shipped toolchain (both are installed
# below), so both belong to the core build.
list(APPEND ZL_CORE_TARGETS zlpkg zl-bind)

# --- C++ regression targets --------------------------------------------------
# A test target is either REQUIRED - part of the suite, a missing source is a
# configuration error - or explicitly OPTIONAL - genuinely unavailable on some
# platforms/configurations, and the skip message must say why. A silently
# skipped required target is not a passing test, so the required path fails
# configuration instead of skipping; that is also the completeness guard
# (Objective 6.9): the (target, source) pairs below are the single source of
# truth, checked as configuration runs, with no duplicated filename list.
find_package(Threads REQUIRED)

macro(zl_add_required_test target primary_source)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${primary_source}")
        message(FATAL_ERROR
            "Required test source for ${target} is missing: ${primary_source}. "
            "Restore the source, or delete the target if the subsystem is gone - "
            "a missing required test must fail configuration, not be skipped.")
    endif()
    add_executable(${target} ${primary_source} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    list(APPEND ZL_CTEST_TARGETS ${target})
    # Tests are reachable only through zl-tests / zl-full, never through the
    # default core build: keep them out of the default dependency graph.
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    list(APPEND ZL_TEST_TARGETS ${target})
endmacro()

macro(zl_add_optional_test target primary_source)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${primary_source}")
        message(STATUS "Skipping ${target}: optional ${primary_source} missing (platform-specific target, documented at its registration)")
        return()
    endif()
    add_executable(${target} ${primary_source} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    list(APPEND ZL_CTEST_TARGETS ${target})
    set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    list(APPEND ZL_TEST_TARGETS ${target})
endmacro()

zl_add_required_test(zl-runtime-type-tests tests/runtime_type_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/common/type_name.cpp
    src/vm/runtime_type_checks.cpp
    src/vm/type_contracts.cpp)

# Controlled collector seam: exercises GC parking/reactivation and collector
# election without depending on allocation volume or thread scheduling luck.
# The test defines its own TracingGC methods (including Collection::reclaim),
# so gc.cpp must NOT be linked: only the coordinator is under test.
zl_add_required_test(zl-gc-safepoint-tests tests/gc_safepoint_tests.cpp src/vm/gc_safepoint.cpp)
    target_link_libraries(zl-gc-safepoint-tests PRIVATE Threads::Threads)

# Program graph roots and blocking reclamation across consecutive collections.
zl_add_required_test(zl-gc-lifetime-tests tests/gc_lifetime_tests.cpp
    src/vm/gc.cpp src/vm/gc_safepoint.cpp src/vm/value.cpp
    src/vm/runtime_task.cpp src/vm/runtime_exception.cpp src/vm/runtime_thread.cpp)
    target_link_libraries(zl-gc-lifetime-tests PRIVATE Threads::Threads)

zl_add_required_test(zl-gc-tests tests/gc_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-runtime-task-executor-tests tests/runtime_task_executor_tests.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/runtime_thread.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)
target_link_libraries(zl-runtime-task-executor-tests PRIVATE Threads::Threads)

zl_add_required_test(zl-runtime-sync-tests tests/runtime_sync_tests.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)
target_link_libraries(zl-runtime-sync-tests PRIVATE Threads::Threads)

zl_add_required_test(zl-ir-tests tests/ir_tests.cpp src/compiler/ir.cpp src/compiler/ir_optimizer.cpp src/vm/value.cpp src/vm/gc.cpp src/vm/gc_safepoint.cpp src/vm/runtime_exception.cpp src/vm/runtime_task.cpp src/common/type_name.cpp src/vm/type_contracts.cpp src/vm/runtime_type_checks.cpp)
zl_add_required_test(zl-machine-code-tests tests/machine_code_tests.cpp src/compiler/machine_code.cpp src/compiler/ir.cpp)

zl_add_required_test(zl-regex-engine-tests tests/regex_engine_tests.cpp src/regex/regex.cpp)
zl_add_required_test(zl-regex-unicode-tests tests/regex_unicode_tests.cpp src/regex/regex.cpp)

# Source-root walk: an ancestor named `src` above a zlpkg.toml/.git boundary
# must not become the import root.
zl_add_required_test(zl-module-root-tests tests/module_root_tests.cpp)

# Git dependency URL allowlist (https/ssh/scp-like only).
zl_add_required_test(zl-zlpkg-git-url-tests tests/zlpkg_git_url_tests.cpp
    zlpkg/git.cpp zlpkg/process.cpp zlpkg/manifest.cpp)
target_include_directories(zl-zlpkg-git-url-tests PRIVATE ${CMAKE_SOURCE_DIR}/zlpkg)

zl_add_required_test(zl-runtime-scheduler-tests tests/runtime_scheduler_tests.cpp
    src/vm/runtime_scheduler.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/vm/runtime_task_executor.cpp
    src/vm/value.cpp
    src/vm/gc.cpp)
target_link_libraries(zl-runtime-scheduler-tests PRIVATE Threads::Threads)

# VM boundary-hardening regression target. It links the interpreter sources
# directly so malformed reflection handles can be exercised without a ZL
# program manufacturing an impossible internal Function object.
set(ZL_RUNTIME_VM_SOURCES ${SOURCES})
list(FILTER ZL_RUNTIME_VM_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

# Any target that links ZL_RUNTIME_VM_SOURCES pulls in src/vm/native.cpp, which
# calls Winsock (Network.resolve), and the runtime's threading. Those platform
# link requirements are a property of the source set, not of the individual
# test, so they are attached in one place: registering a VM-sources test any
# other way is what let zl-mir-type-tests and zl-mir-ownership-tests ship
# without ws2_32 and fail to link on Windows only.
macro(zl_add_vm_test target primary_source)
    zl_add_required_test(${target} ${primary_source} ${ZL_RUNTIME_VM_SOURCES} ${ARGN})
    target_link_libraries(${target} PRIVATE Threads::Threads)
    if(WIN32)
        target_link_libraries(${target} PRIVATE ws2_32)
    endif()
    list(APPEND ZL_VM_TEST_TARGETS ${target})
endmacro()
zl_add_vm_test(zl-runtime-hardening-vm-tests tests/runtime_hardening_vm_tests.cpp)
# Lets the target re-run the boundary lint inside its own process as a
# regression case (compiler boundary violations must keep being caught).
find_program(ZL_TEST_BASH NAMES bash)
if(ZL_TEST_BASH)
    target_compile_definitions(zl-runtime-hardening-vm-tests PRIVATE
        ZL_BOUNDARY_LINT_ROOT="${CMAKE_SOURCE_DIR}"
        ZL_BOUNDARY_LINT_BASH="${ZL_TEST_BASH}")
endif()

# --- Native backend (MIR -> native IR -> machine code) ----------------------
# Drives the whole native pipeline and *executes* the emitted machine code, so
# it links the front end (its end-to-end cases lower real ZL source) plus the
# native sources themselves.
zl_add_vm_test(zl-native-backend-tests tests/native_backend_tests.cpp)
zl_add_vm_test(zl-native-exec-tests tests/native_exec_tests.cpp)

# --- MIR (mid-level IR) -----------------------------------------------------
# Two targets, split by what they need to link.
#
# zl-mir-tests builds MIR by hand and checks the verifier rejects each class of
# malformed module. It only needs the MIR sources plus the two compiler
# utilities the verifier delegates to, so it stays fast and does not depend on
# the front end.
# CFG queries, data-flow analyses, and slot -> block-parameter construction.
zl_add_required_test(zl-mir-ssa-tests tests/mir_ssa_tests.cpp
    src/mir/folding.cpp
    src/mir/effects.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/dataflow.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/printer.cpp
    src/mir/ssa.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)
zl_add_required_test(zl-mir-tests tests/mir_tests.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/printer.cpp
    src/mir/reachability.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/native_catalog.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)

# The optimiser framework: pass manager, analysis manager, snapshots, the
# passes themselves, and the differential validator. It needs no front end, so
# it stays a fast target like zl-mir-ssa-tests.
zl_add_required_test(zl-mir-opt-tests tests/mir_pass_tests.cpp
    src/mir/analysis.cpp
    src/mir/builder.cpp
    src/mir/dataflow.cpp
    src/mir/differential.cpp
    src/mir/effects.cpp
    src/mir/folding.cpp
    src/mir/function.cpp
    src/mir/instruction.cpp
    src/mir/opt_constant.cpp
    src/mir/opt_dead.cpp
    src/mir/opt_dead_functions.cpp
    src/mir/opt_simplify.cpp
    src/mir/pass_manager.cpp
    src/mir/printer.cpp
    src/mir/reachability.cpp
    src/mir/ssa.cpp
    src/mir/type.cpp
    src/mir/verifier.cpp
    src/mir/safety.cpp
    src/compiler/operator_rules.cpp
    src/compiler/native_catalog.cpp
    src/compiler/semantic_types.cpp
    src/common/type_name.cpp)

# zl-mir-opt-pipeline-tests runs the optimiser over real ZL programs and diffs
# the two modules twice - statically (compareModules) and by executing both
# through the bytecode backend with their output captured - so it links the
# whole compiler minus main(), like the lowering target.
zl_add_vm_test(zl-mir-opt-pipeline-tests tests/mir_opt_pipeline_tests.cpp)

# zl-pipeline-tests owns the compiler pipeline itself: stage order, the
# invariant that no backend sees unverified MIR, backend selection as a
# code-generation decision only, and the exit-code contract. It links the whole
# compiler minus main() (ZL_RUNTIME_VM_SOURCES) because these are end-to-end
# properties: a pipeline assembled out of mocks would prove nothing about the
# one that ships.
zl_add_vm_test(zl-pipeline-tests tests/pipeline_tests.cpp)

# zl-mir-lowering-tests drives the real pipeline - ModuleLoader, TypeChecker,
# lowerProgram, verifyModule - so it links the whole compiler minus main().
zl_add_vm_test(zl-mir-lowering-tests tests/mir_lowering_tests.cpp)

# Type-system integration: the type table, sum types, canonical nested
# generics through lowering, dynamic-to-static boundaries, and the verifier
# rules that keep invalid type assumptions detectable. Links the whole front
# end because most of its cases drive the real pipeline.
zl_add_vm_test(zl-mir-type-tests tests/mir_type_tests.cpp)

# Ownership/lifetime integration: storage contracts and the move/borrow/drop
# events, with the verifier's flow rules pinning each invalid state. Links the
# whole front end for its pipeline case.
zl_add_vm_test(zl-mir-ownership-tests tests/mir_ownership_tests.cpp)


# --- Installation (Phase 12A foundation) -----------------------------------
# The installed runtime, package manager, and stdlib share one prefix-relative
# layout so the runtime can locate its stdlib without ZL_STDLIB_ROOT.
include(GNUInstallDirs)
install(TARGETS zl_language
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
if(WIN32)
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/zl.bat
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME zl.bat
    )
else()
    install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/zl
        DESTINATION ${CMAKE_INSTALL_BINDIR}
        RENAME zl
    )
endif()
install(TARGETS zlpkg
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/tools/zl-lsp/zl_lsp.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME zl-lsp
)
install(PROGRAMS ${CMAKE_SOURCE_DIR}/tools/zl-test/zl_test.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME zl-test
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/stdlib/
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zl/stdlib
    PATTERN "VERSION.in" EXCLUDE
)
install(FILES ${CMAKE_BINARY_DIR}/stdlib/VERSION
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/zl/stdlib
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES ${CMAKE_SOURCE_DIR}/LICENSE
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zl/licenses
    OPTIONAL
)
install(FILES ${CMAKE_SOURCE_DIR}/README.md ${CMAKE_SOURCE_DIR}/ROADMAP.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/zl
    OPTIONAL
)


# --- Packaging (Phase 12A distribution) ------------------------------------
# CPack keeps the release layout identical across native build hosts. Native
# Windows/macOS runners can invoke the same configuration to produce their
# platform packages; the Linux build also emits TGZ/DEB/ZIP artifacts.
set(CPACK_PACKAGE_NAME "zl")
set(CPACK_PACKAGE_VENDOR "ZL Language")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "ZL programming language toolchain")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "ZL")
set(CPACK_GENERATOR "TGZ;ZIP")
if(UNIX AND NOT APPLE)
    set(CPACK_GENERATOR "TGZ;DEB;ZIP")
    set(CPACK_DEBIAN_PACKAGE_MAINTAINER "ZL Language")
    set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
    set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    set(CPACK_DEBIAN_PACKAGE_DEPENDS "")

    # RPM is enabled when rpmbuild is available on the build host. This keeps
    # ordinary Linux development environments free of a hard RPM dependency.
    find_program(RPMBUILD_EXECUTABLE rpmbuild)
    if(RPMBUILD_EXECUTABLE)
        list(APPEND CPACK_GENERATOR "RPM")
        set(CPACK_RPM_PACKAGE_LICENSE "PolyForm-Noncommercial-1.0.0")
        set(CPACK_RPM_PACKAGE_GROUP "Development/Languages")
        set(CPACK_RPM_PACKAGE_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
endif()
include(CPack)

# Phase 10+ native/backend smoke tests. These are optional for lightweight
# checkouts for the same reason as the focused tests above.
zl_add_required_test(zl-native-compiler-tests tests/native_compiler_tests.cpp
    src/compiler/native_compiler.cpp
    src/compiler/ir_lowering.cpp
    src/compiler/ir.cpp
    src/compiler/ir_optimizer.cpp
    src/vm/value.cpp
    src/vm/gc.cpp
    src/vm/gc_safepoint.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp
    src/common/type_name.cpp
    src/vm/type_contracts.cpp
    src/vm/runtime_type_checks.cpp
    src/parser/parser.cpp
    src/parser/expression_parser.cpp
    src/parser/type_lookahead.cpp
    src/parser/ast.cpp
    src/lexer/lexer.cpp)

zl_add_required_test(zl-native-ffi-metadata-tests tests/native_ffi_metadata_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-native-resource-tests tests/native_resource_tests.cpp
    src/vm/native_resource.cpp)

zl_add_required_test(zl-native-boundary-tests tests/native_boundary_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-native-runtime-registry-tests tests/native_runtime_registry_tests.cpp
    src/vm/native_boundary.cpp
    src/vm/native_resource.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-native-ffi-transport-tests tests/native_ffi_transport_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-native-ffi-lifetime-tests tests/native_ffi_lifetime_adapter_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

zl_add_required_test(zl-native-resource-stress-tests tests/native_resource_stress_tests.cpp
    src/vm/native_resource.cpp)

# Phase 16 native struct/library runtime tests.
zl_add_required_test(zl-native-struct-tests tests/native_struct_tests.cpp
    src/vm/native_resource.cpp
    src/vm/native_boundary.cpp
    src/vm/value.cpp
    src/vm/execution_state.cpp
    src/vm/gc.cpp
    src/vm/runtime_exception.cpp
    src/vm/runtime_task.cpp)

# A real shared object to load, so the library test exercises dlopen/dlsym
# against a known symbol instead of a system library it cannot assume exists.
add_library(zl-test-native-library SHARED tests/fixtures/native_library_fixture.cpp)
set_target_properties(zl-test-native-library PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    WINDOWS_EXPORT_ALL_SYMBOLS TRUE
    EXCLUDE_FROM_ALL TRUE)
list(APPEND ZL_TEST_TARGETS zl-test-native-library)

zl_add_required_test(zl-native-library-tests tests/native_library_tests.cpp
    src/vm/native_library.cpp)
target_link_libraries(zl-native-library-tests PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(zl-native-library-tests PRIVATE
    ZL_TEST_NATIVE_LIBRARY_PATH="$<TARGET_FILE:zl-test-native-library>")

zl_add_required_test(zl-native-ffi-declaration-tests tests/native_ffi_declaration_tests.cpp
    src/compiler/native_ffi_declarations.cpp
    src/parser/parser.cpp
    src/parser/expression_parser.cpp
    src/parser/type_lookahead.cpp
    src/parser/ast.cpp
    src/lexer/lexer.cpp)

# Safety analysis regression: malformed MIR is measured separately from source
# corpus rejection layers, because it tests compiler/pass contracts.
zl_add_required_test(zl-mir-safety-tests tests/mir_safety_tests.cpp
    src/mir/analysis.cpp src/mir/builder.cpp src/mir/function.cpp
    src/mir/instruction.cpp src/mir/printer.cpp src/mir/type.cpp
    src/mir/verifier.cpp src/mir/safety.cpp
    src/compiler/operator_rules.cpp src/compiler/semantic_types.cpp src/common/type_name.cpp)

include(CTest)
if(BUILD_TESTING)
    # Every C++ regression target is registered under its canonical name, so
    # `ctest` runs the whole suite and a broken target is visible there.
    foreach(zl_test_target IN LISTS ZL_CTEST_TARGETS)
        add_test(NAME ${zl_test_target} COMMAND ${zl_test_target})
    endforeach()

    # The compiler boundary, enforced as a check rather than as a convention:
    # the backends cannot see the AST, the lowerer cannot infer, the front end
    # has one driver, and the legacy IR cannot grow a second consumer.
    # Intentionally optional: the check needs bash, a development-host tool.
    find_program(ZL_BASH NAMES bash)
    if(ZL_BASH)
        add_test(NAME boundary-lint COMMAND ${ZL_BASH}
            ${CMAKE_SOURCE_DIR}/tools/boundary_lint.sh ${CMAKE_SOURCE_DIR})
        # The lint's own regressions: the rules must fail on the broken trees
        # they exist for, with a diagnostic that names the file and line.
        add_test(NAME boundary-lint-regressions COMMAND ${ZL_BASH}
            ${CMAKE_SOURCE_DIR}/tests/boundary_lint_tests.sh ${CMAKE_SOURCE_DIR})
    endif()

    # Intentionally optional: the checks need python3, a development-host tool.
    find_program(ZL_SAFETY_PYTHON NAMES python3)
    if(ZL_SAFETY_PYTHON)
        add_test(NAME safety-pipeline COMMAND ${ZL_SAFETY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/safety_pipeline.py $<TARGET_FILE:zl_language>)
        set_tests_properties(safety-pipeline PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    # The --pipeline-report CLI contract: a runnable program's pipeline, or a
    # precise refusal (library without an entry point, invalid entry point).
    find_program(ZL_REPORT_PYTHON NAMES python3)
    if(ZL_REPORT_PYTHON)
        add_test(NAME pipeline-report COMMAND ${ZL_REPORT_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/pipeline_report_cli.py $<TARGET_FILE:zl_language>)
        set_tests_properties(pipeline-report PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    # Backend fold parity: the self-checking LiteralFoldParity fixture must
    # print "ok" under the MIR, AST, unoptimized-MIR, and native configs, so
    # no constant folder or pool can drift from checked runtime arithmetic.
    find_program(ZL_PARITY_PYTHON NAMES python3)
    if(ZL_PARITY_PYTHON)
        add_test(NAME backend-fold-parity COMMAND ${ZL_PARITY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/backend_fold_parity.py $<TARGET_FILE:zl_language>)
        set_tests_properties(backend-fold-parity PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    # Double-format parity: the same fixture under the same four configs, for
    # the other half of the printing contract - a double renders as its shortest
    # round-trip decimal, and every surface that renders one agrees.
    find_program(ZL_FORMAT_PYTHON NAMES python3)
    if(ZL_FORMAT_PYTHON)
        add_test(NAME double-format-parity COMMAND ${ZL_FORMAT_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/double_format_parity.py $<TARGET_FILE:zl_language>)
        set_tests_properties(double-format-parity PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    # Sum-match parity: matching Option/Result, and a match whose arms produce
    # no value, must be the same program on every backend - the checker's case
    # coverage, the inferred pattern arguments and the statement-position
    # lowering all meet in this fixture.
    if(ZL_PARITY_PYTHON)
        add_test(NAME sum-match-parity COMMAND ${ZL_PARITY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/sum_match_parity.py $<TARGET_FILE:zl_language>)
        set_tests_properties(sum-match-parity PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
    # Native execution parity (P1-6): the emitted x86-64 bytes, run by the
    # driver, must compute the same number as the VM loop over the same
    # program - and the driver must refuse what it cannot honestly call.
    # Skips off x86-64 Linux, where the driver refuses by design.
    if(ZL_PARITY_PYTHON)
        add_test(NAME native-exec-parity COMMAND ${ZL_PARITY_PYTHON}
            ${CMAKE_SOURCE_DIR}/tests/native_exec_parity.py $<TARGET_FILE:zl_language>)
        set_tests_properties(native-exec-parity PROPERTIES WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
    endif()
endif()

# --- Platform link-requirement guard ----------------------------------------
# A test that compiles src/vm/native.cpp but never links Winsock builds fine on
# Linux and fails only at link time on Windows. Rather than trust review to
# catch that, assert it here: every target carrying the VM sources must have
# gone through zl_add_vm_test, which is the only place the link requirements
# live.
foreach(zl_vm_target IN LISTS ZL_VM_TEST_TARGETS)
    get_target_property(zl_vm_libs ${zl_vm_target} LINK_LIBRARIES)
    if(WIN32 AND NOT "ws2_32" IN_LIST zl_vm_libs)
        message(FATAL_ERROR
            "${zl_vm_target} links the VM sources but not ws2_32; it would fail "
            "to link on Windows. Register it with zl_add_vm_test().")
    endif()
endforeach()
list(LENGTH ZL_VM_TEST_TARGETS ZL_VM_TEST_COUNT)
message(STATUS "[ZL] VM-sources tests: ${ZL_VM_TEST_COUNT} (Threads + Winsock linked centrally)")

# --- Aggregate targets (Objective 8.7) --------------------------------------
# Declared last so every target registered above has already joined a bucket.
#
#   zl-core       default developer build: the usable ZL toolchain
#   zl-tests      zl-core + every C++ regression executable and fixture
#   zl-benchmarks zl-core + the benchmark gate driver
#   zl-examples   zl-core (the examples are .zl programs run by the toolchain)
#   zl-full       everything the repository can build on purpose
#
# The buckets share one dependency list each; no build logic is duplicated.
add_custom_target(zl-core
    COMMENT "[ZL] Core toolchain built (compiler, runtime, VM, tools).")
add_dependencies(zl-core ${ZL_CORE_TARGETS})

# `zl` is the human-facing alias for the default developer build, so
# `cmake --build build --target zl` does the documented thing.
add_custom_target(zl COMMENT "[ZL] zl toolchain")
add_dependencies(zl zl-core)

add_custom_target(zl-tests
    COMMENT "[ZL] Test executables built. Run them with ctest.")
add_dependencies(zl-tests zl-core ${ZL_TEST_TARGETS})

# The benchmark gate compiles generated native C++ together with its driver, so
# it is driven by scripts/native_gate.sh rather than by a plain link target.
add_custom_target(zl-benchmarks
    COMMENT "[ZL] Benchmark gate prerequisites built - drive them with scripts/native_gate.sh.")
add_dependencies(zl-benchmarks zl-core)

add_custom_target(zl-examples
    COMMENT "[ZL] Example prerequisites built - run them with scripts/run_examples.sh or .bat.")
add_dependencies(zl-examples zl-core)

add_custom_target(zl-full
    COMMENT "[ZL] Full repository build complete.")
add_dependencies(zl-full zl-core zl-tests zl-benchmarks zl-examples)
if(ZL_OPTIONAL_TARGETS)
    add_dependencies(zl-full ${ZL_OPTIONAL_TARGETS})
endif()

# Reported at configure time so the target boundary is visible, not folklore.
list(LENGTH ZL_CORE_TARGETS ZL_CORE_TARGET_COUNT)
list(LENGTH ZL_TEST_TARGETS ZL_TEST_TARGET_COUNT)
message(STATUS "[ZL] core targets: ${ZL_CORE_TARGET_COUNT} (${ZL_CORE_TARGETS})")
message(STATUS "[ZL] test targets: ${ZL_TEST_TARGET_COUNT} (built only via zl-tests / zl-full)")
