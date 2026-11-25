# What mods are built with. `rocketify build` passes the folder of this file as ROCKETIFY_DIR.
cmake_minimum_required(VERSION 3.15)
project(Rocketify LANGUAGES CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Rocketify mods are 64-bit Windows DLLs. Build them with: rocketify build")
endif()

add_library(rocketify-headers INTERFACE)
target_include_directories(rocketify-headers INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(rocketify-headers INTERFACE cxx_std_17)
if(MSVC)
    target_compile_options(rocketify-headers INTERFACE /utf-8 /EHsc)
else()
    target_link_options(rocketify-headers INTERFACE -static -static-libgcc -static-libstdc++)
endif()

# Turns a SHARED library target into a mod (<target>.rlmod).
function(setup_rocketify_mod target)
    target_link_libraries(${target} PRIVATE rocketify-headers)
    set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".rlmod")
    if(MSVC)
        # no Visual C++ runtime needed on players' PCs
        set_target_properties(${target} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endfunction()
