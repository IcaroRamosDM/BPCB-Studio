cmake_minimum_required(VERSION 3.20)
project(bpcb_studio VERSION 0.6.0 LANGUAGES C CXX)
include(cmake/EmbedCatalogs.cmake)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(_POSIX_C_SOURCE=200809L)
if(MINGW)
    add_compile_definitions(__USE_MINGW_ANSI_STDIO=1)
endif()
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(
    DEPS REQUIRED IMPORTED_TARGET json-c libarchive libcrypto poppler-glib librsvg-2.0 pangocairo
    libusb-1.0
)
option(ENABLE_SANITIZERS "Verificar memoria e comportamento indefinido" OFF)
if(ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined,float-cast-overflow -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined,float-cast-overflow)
endif()
add_compile_options(-Wall -Wextra -Wpedantic -Wconversion)
add_library(
    bancada_compat STATIC
    vendor/bpcb/wiring.c
    vendor/bpcb/communication/frame.c
    vendor/bpcb/communication/lab.c
    vendor/bpcb/communication/peripherals.c
    vendor/bpcb/communication/uart_protocol.c
    vendor/bpcb/communication/replay.c
    vendor/bpcb/communication/qualification.c
    vendor/bpcb/communication/config.c
    vendor/bpcb/communication/protocol.c
    vendor/bpcb/communication/ch347.c
    vendor/bpcb/communication/usb.c
    vendor/bpcb/communication/hid.c
    vendor/bpcb/communication/serial.c
    vendor/bpcb/communication/session.c
    vendor/bpcb/communication/tools.c
    vendor/bpcb/common.c
    vendor/bpcb/model.c
    vendor/bpcb/packages.c
    vendor/bpcb/jobs.c
    vendor/bpcb/browse.c
    vendor/bpcb/pdf.c
    vendor/bpcb/assessment.c
    vendor/bpcb/plot.c
    vendor/bpcb/reading.c
    vendor/bpcb/calibration.c
)
if(WIN32)
    target_sources(bancada_compat PRIVATE vendor/bpcb/windows_compat.c)
    target_compile_definitions(bancada_compat PUBLIC _WIN32_WINNT=0x0601)
    target_link_libraries(bancada_compat PUBLIC ws2_32 shell32 gdi32 setupapi hid uuid)
endif()
target_include_directories(bancada_compat PUBLIC vendor/bpcb)
target_link_libraries(bancada_compat PUBLIC PkgConfig::DEPS Threads::Threads m)
add_library(
    studio_core STATIC
    core/workbench.c
    core/lab_authoring.c
    core/communication.c
    core/platform.c
    core/catalog.c
    core/catalog_extra.c
    core/authoring.c
    core/continuity.c
    core/project.c
    core/validate.c
    core/engine.c
    core/io.c
    core/storage.c
    core/async.c
    vendor/drivers/ads7924_c.c
)
target_include_directories(studio_core PUBLIC core PRIVATE vendor/drivers)
target_include_directories(studio_core PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(studio_core PUBLIC bancada_compat)
add_executable(bpcb_sequence runtime/main.c)
target_link_libraries(bpcb_sequence PRIVATE studio_core)
add_executable(studio_cli runtime/cli.c)
target_link_libraries(studio_cli PRIVATE studio_core)
find_package(FLTK 1.4 CONFIG REQUIRED)
pkg_check_modules(FONTCONFIG REQUIRED IMPORTED_TARGET fontconfig)
add_executable(bpcb_studio ui/main.cpp)
target_link_libraries(bpcb_studio PRIVATE studio_core fltk::fltk fltk::images PkgConfig::FONTCONFIG)
add_dependencies(bpcb_studio bpcb_sequence)
include(CTest)
if(BUILD_TESTING)
    add_executable(studio_test tests/core_test.c)
    target_link_libraries(studio_test PRIVATE studio_core)
    target_compile_options(studio_test PRIVATE -UNDEBUG)
    add_test(NAME studio_core COMMAND studio_test $<TARGET_FILE:bpcb_sequence>)
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND AND NOT WIN32)
        add_test(NAME studio_backup_adversarial
                 COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/backup_test.py
                         $<TARGET_FILE:studio_cli>
        )
    endif()
    find_program(XVFB_RUN xvfb-run)
    find_program(XDOTOOL xdotool)
    if(NOT WIN32 AND XVFB_RUN AND XDOTOOL)
        add_test(NAME studio_gui COMMAND bash ${CMAKE_SOURCE_DIR}/tests/ui_test.sh
                                         ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(studio_gui PROPERTIES TIMEOUT 120)
    endif()
endif()
if(WIN32)
    enable_language(RC)
    foreach(exe bpcb_studio bpcb_sequence studio_cli)
        target_sources(${exe} PRIVATE packaging/windows.rc)
        target_include_directories(${exe} PRIVATE packaging)
    endforeach()
    if(BUILD_TESTING)
        target_sources(studio_test PRIVATE packaging/windows.rc)
        target_include_directories(studio_test PRIVATE packaging)
    endif()
    set_target_properties(bpcb_studio PROPERTIES WIN32_EXECUTABLE TRUE)
endif()
install(TARGETS bpcb_studio bpcb_sequence studio_cli RUNTIME DESTINATION bin)

if(BUILD_TESTING)
    add_executable(studio_communication_test tests/communication_test.c)
    target_link_libraries(studio_communication_test PRIVATE studio_core)
    target_compile_options(studio_communication_test PRIVATE -UNDEBUG)
    add_test(NAME studio_communication COMMAND studio_communication_test)
endif()

if(ENABLE_SANITIZERS AND NOT WIN32)
    foreach(exe studio_cli bpcb_sequence studio_test studio_communication_test)
        if(TARGET ${exe})
            target_sources(${exe} PRIVATE tests/sanitizer_cleanup.c)
            target_link_libraries(${exe} PRIVATE PkgConfig::FONTCONFIG)
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(studio_lab_benchmark tests/lab_benchmark.c)
    target_link_libraries(studio_lab_benchmark PRIVATE studio_core)
endif()

if(BUILD_TESTING AND NOT WIN32)
    add_executable(studio_lab_test tests/lab_authoring_test.c)
    target_link_libraries(studio_lab_test PRIVATE studio_core)
    target_compile_options(studio_lab_test PRIVATE -UNDEBUG)
    add_test(NAME studio_lab_authoring COMMAND studio_lab_test ${CMAKE_SOURCE_DIR})
    add_test(NAME studio_lab_execution COMMAND studio_lab_benchmark)
    if(ENABLE_SANITIZERS)
        target_sources(studio_lab_test PRIVATE tests/sanitizer_cleanup.c)
        target_link_libraries(studio_lab_test PRIVATE PkgConfig::FONTCONFIG)
    endif()
endif()

if(BUILD_TESTING AND NOT WIN32)
    add_executable(studio_workbench_test tests/workbench_test.c)
    target_link_libraries(studio_workbench_test PRIVATE studio_core)
    add_test(NAME studio_workbench COMMAND studio_workbench_test ${CMAKE_SOURCE_DIR})
endif()

if(BUILD_TESTING AND Python3_FOUND AND NOT WIN32)
    add_test(NAME studio_debugger_protocol
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/debugger_protocol_test.py
                     $<TARGET_FILE:studio_cli> $<TARGET_FILE:bpcb_sequence>
    )
    set_tests_properties(studio_debugger_protocol PROPERTIES TIMEOUT 45)
endif()

# This target is opt-in: formatters are development tools, not build dependencies.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(PROJECT_WORKSPACE "${CMAKE_CURRENT_SOURCE_DIR}")
if(Python3_FOUND AND EXISTS "${PROJECT_WORKSPACE}/scripts/formatar.py")
    add_custom_target(
        verificar-formato
        COMMAND ${Python3_EXECUTABLE} "${PROJECT_WORKSPACE}/scripts/formatar.py"
        WORKING_DIRECTORY "${PROJECT_WORKSPACE}"
        COMMENT "Verificando o estilo dos fontes ativos"
        VERBATIM
    )
endif()
