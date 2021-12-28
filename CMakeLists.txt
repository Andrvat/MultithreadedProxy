cmake_minimum_required(VERSION 3.21)
project(MultithreadedProxy C)

set(CMAKE_C_STANDARD 99)

SET(CMAKE_C_FLAGS -pthread)

add_executable(MultithreadedProxy main.c)
