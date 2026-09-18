cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ufsm292-zephyr-app-2026)

target_sources(app PRIVATE src/main.c)

# This is needed so that custom driver classes using system calls are taken into
# account
zephyr_syscall_include_directories(include)

zephyr_include_directories(include)

add_subdirectory(drivers)
add_subdirectory(lib)
