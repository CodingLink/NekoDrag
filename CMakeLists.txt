cmake_minimum_required(VERSION 3.20)

project(SuperDrag VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(superdrag_core STATIC
    src/core.cpp
)
target_include_directories(superdrag_core PUBLIC src)

if(MSVC)
    target_compile_options(superdrag_core PRIVATE /W4 /permissive- /utf-8)
    set_property(TARGET superdrag_core PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
else()
    target_compile_options(superdrag_core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    enable_language(RC)

    option(SUPERDRAG_TRACE
        "Log drag state transitions via OutputDebugString (view with DebugView)"
        OFF
    )

    add_executable(SuperDrag WIN32
        src/main.cpp
        src/app.cpp
        src/settings_store.cpp
        src/ui_theme.cpp
        src/superdrag.rc
        src/superdrag.manifest
    )
    target_include_directories(SuperDrag PRIVATE src)
    target_compile_definitions(SuperDrag PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
    )
    if(SUPERDRAG_TRACE)
        target_compile_definitions(SuperDrag PRIVATE SUPERDRAG_TRACE)
    endif()
    target_compile_options(SuperDrag PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(SuperDrag PRIVATE
        superdrag_core
        user32
        shell32
        advapi32
        dwmapi
        comctl32
    )
    set_property(TARGET SuperDrag PROPERTY
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
    target_link_options(SuperDrag PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(superdrag_tests tests/core_tests.cpp)
    target_link_libraries(superdrag_tests PRIVATE superdrag_core)
    if(MSVC)
        target_compile_options(superdrag_tests PRIVATE /W4 /permissive- /utf-8)
        set_property(TARGET superdrag_tests PROPERTY
            MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        )
    else()
        target_compile_options(superdrag_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME superdrag_core_tests COMMAND superdrag_tests)
endif()
