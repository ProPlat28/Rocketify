cmake_minimum_required(VERSION 3.15)
project(my_mod LANGUAGES CXX)

# `rocketify build` sets ROCKETIFY_DIR; editors that run CMake themselves find it here.
if(NOT ROCKETIFY_DIR)
    if(CMAKE_HOST_WIN32)
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}/.rocketify/files" ROCKETIFY_DIR)
    else()
        set(ROCKETIFY_DIR "$ENV{HOME}/.rocketify/files")
    endif()
endif()
if(NOT EXISTS "${ROCKETIFY_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "Build this mod with: rocketify build")
endif()
add_subdirectory("${ROCKETIFY_DIR}" "${CMAKE_BINARY_DIR}/rocketify")

add_library(${PROJECT_NAME} SHARED src/main.cpp)
setup_rocketify_mod(${PROJECT_NAME})
