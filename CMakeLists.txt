cmake_minimum_required(VERSION 3.20)

project(NekoDrag VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(DEFINED SUPERDRAG_TRACE)
    message(DEPRECATION
        "SUPERDRAG_TRACE has been renamed to NEKODRAG_TRACE; "
        "the legacy cache value will be removed after this configure."
    )
    if(NOT DEFINED NEKODRAG_TRACE)
        set(NEKODRAG_TRACE "${SUPERDRAG_TRACE}" CACHE BOOL
            "Log drag state transitions via OutputDebugString (view with DebugView)"
            FORCE
        )
    endif()
    unset(SUPERDRAG_TRACE CACHE)
    unset(SUPERDRAG_TRACE)
endif()

option(NEKODRAG_TRACE
    "Log drag state transitions via OutputDebugString (view with DebugView)"
    OFF
)

add_library(nekodrag_core STATIC
    src/core.cpp
)
target_include_directories(nekodrag_core PUBLIC src)

if(MSVC)
    target_compile_options(nekodrag_core PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET nekodrag_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
else()
    target_compile_options(nekodrag_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    enable_language(RC)

    add_library(nekodrag_window_move STATIC
        src/native_move_worker.cpp
        src/window_move_worker.cpp
    )
    target_include_directories(nekodrag_window_move PUBLIC src)
    target_compile_definitions(nekodrag_window_move PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    target_compile_options(nekodrag_window_move PRIVATE
        /W4 /permissive- /utf-8 /EHsc
    )
    target_link_libraries(nekodrag_window_move PUBLIC
        nekodrag_core
        user32
    )
    set_property(TARGET nekodrag_window_move PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )

    add_executable(NekoDrag WIN32
        src/main.cpp
        src/app.cpp
        src/settings_store.cpp
        src/ui_theme.cpp
        src/nekodrag.rc
        src/nekodrag.manifest
    )
    target_include_directories(NekoDrag PRIVATE src)
    target_compile_definitions(NekoDrag PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    if(NEKODRAG_TRACE)
        target_compile_definitions(NekoDrag PRIVATE NEKODRAG_TRACE)
    endif()
    target_compile_options(NekoDrag PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(NekoDrag PRIVATE
        nekodrag_core
        nekodrag_window_move
        user32
        shell32
        advapi32
        dwmapi
        comctl32
    )
    set_property(TARGET NekoDrag PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_link_options(NekoDrag PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(nekodrag_tests tests/core_tests.cpp)
    target_link_libraries(nekodrag_tests PRIVATE nekodrag_core)
    if(MSVC)
        target_compile_options(nekodrag_tests PRIVATE /W4 /permissive- /utf-8)
        set_property(TARGET nekodrag_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    else()
        target_compile_options(nekodrag_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME nekodrag_core_tests COMMAND nekodrag_tests)

    if(WIN32)
        add_executable(nekodrag_window_move_tests
            tests/native_move_worker_tests.cpp
            tests/window_move_worker_tests.cpp
        )
        target_link_libraries(nekodrag_window_move_tests PRIVATE
            nekodrag_window_move
        )
        target_compile_definitions(nekodrag_window_move_tests PRIVATE
            UNICODE
            _UNICODE
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            _WIN32_WINNT=0x0A00
        )
        target_compile_options(nekodrag_window_move_tests PRIVATE
            /W4 /permissive- /utf-8 /EHsc
        )
        set_property(TARGET nekodrag_window_move_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
        add_test(NAME nekodrag_window_move_tests
                 COMMAND nekodrag_window_move_tests)
    endif()
endif()
