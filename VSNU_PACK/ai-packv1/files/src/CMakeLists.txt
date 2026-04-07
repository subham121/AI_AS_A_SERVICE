# Core library - header-only, but we create a compiled lib for shared utilities

add_library(aipack_core STATIC
    aipack_core.cpp
)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE REQUIRED libpulse-simple libpulse)



target_include_directories(aipack_core PUBLIC
    ${CMAKE_SOURCE_DIR}/include
    ${PULSE_INCLUDE_DIRS}
)

target_link_libraries(aipack_core PUBLIC
    Threads::Threads
    ${DL_LIBRARIES}
    ${PULSE_LIBRARIES}

)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(aipack_core PUBLIC stdc++fs)
endif()

target_compile_definitions(aipack_core PUBLIC
    AIPACK_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    AIPACK_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    AIPACK_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
