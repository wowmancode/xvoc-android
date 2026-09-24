cmake_minimum_required(VERSION 3.22.1)
project(xvocjni C)

set(CMAKE_C_STANDARD 99)

add_library(xvocjni SHARED
    jni_bridge.c
)

find_library(log-lib log)

target_link_libraries(xvocjni
    ${log-lib}
    m
)

target_compile_options(xvocjni PRIVATE -O2 -Wno-unused-function)
