# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

# The codec tests run two ways from one ztest suite:
#
#   * BOARD=unit_testing  -- plain host executable via Zephyr's `unittest`
#     component. Sub-second TDD loop, and what CI runs.
#   * BOARD=samr21_xpro   -- the same cases as a normal Zephyr ztest app on the
#     real Cortex-M0+. This is NOT redundant with the host run: the host is
#     x86-64, where a misaligned uint32_t load is legal and silently returns
#     the right answer, so the host suite cannot see the one bug class that
#     hard-faults on the SAM R21. Run this before trusting the on-air format.
#
# The real translation units are compiled in both times -- no test-local copy --
# so what runs is exactly the production code.

cmake_minimum_required(VERSION 3.28.0)

set(CODEC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../lib/radio)
set(INC_DIR   ${CMAKE_CURRENT_SOURCE_DIR}/../../../include)
set(CODEC_SRCS
  ${CODEC_DIR}/sensor_frame.c
  ${CODEC_DIR}/radio_test_pattern.c
)

# Byte-access safety on the wire buffer.
#
# Scoped to the codec sources rather than the whole target: -Wcast-align=strict
# flags every CONTAINER_OF() in Zephyr, because that macro is
#   ((type *)(((char *)(ptr)) - offsetof(type, field)))
# i.e. a char* -> typed-pointer cast BY DESIGN. Applied target-wide it floods
# the build with kernel-header false positives (and this unittest component
# builds with -Werror). On the codec it is quiet and useful: the frame buffer is
# a byte array, so any (uint16_t *)/(uint32_t *) cast of it is a real bug.
#
# -Wcast-align=strict is the GCC spelling; plain -Wcast-align stays silent when
# the source is a function parameter. Clang has no =strict -- its -Wcast-align
# already behaves like GCC's strict form. Deliberately not -Wuninitialized /
# -Waddress-of-packed-member: both are already in -Wall.
#
# Nothing here catches endianness. Only the golden byte vectors do.
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
  set_source_files_properties(${CODEC_SRCS}
    PROPERTIES COMPILE_OPTIONS "-Wcast-align=strict")
elseif(CMAKE_C_COMPILER_ID MATCHES "Clang")
  set_source_files_properties(${CODEC_SRCS}
    PROPERTIES COMPILE_OPTIONS "-Wcast-align")
endif()

if(BOARD MATCHES "^unit_testing")
  # unittest mode: `set(SOURCES ...)` before find_package(); the component
  # creates the `testbinary` target from it.
  set(SOURCES
    src/main.c
    ${CODEC_SRCS}
  )

  find_package(Zephyr COMPONENTS unittest REQUIRED HINTS $ENV{ZEPHYR_BASE})

  project(app_lib_radio_test)

  target_include_directories(testbinary PRIVATE ${INC_DIR})

  # Alignment checks at runtime as well as at compile time. Catches what the
  # static warning cannot see -- a misaligned pointer laundered through void *
  # or a union -- and turns the x86-64 blind spot into a hard failure here in
  # the sub-second loop instead of a HardFault on the board.
  target_compile_options(testbinary PRIVATE -fsanitize=undefined)
  target_link_options(testbinary PRIVATE -fsanitize=undefined)

else()
  # On-target mode: ordinary ztest app.
  find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

  project(app_lib_radio_test)

  target_sources(app PRIVATE
    src/main.c
    ${CODEC_SRCS}
  )

  target_include_directories(app PRIVATE ${INC_DIR})
endif()
