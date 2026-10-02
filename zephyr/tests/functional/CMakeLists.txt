# Copyright (c) 2026 Space Cubics
# SPDX-License-Identifier: Apache-2.0

# Builds one OSAL functional test as a Zephyr application.
#
# OSAL_TEST selects the test directory relative to the OSAL src/ directory,
# e.g. -DOSAL_TEST=tests/bin-sem-test or -DOSAL_TEST=unit-tests/oscore-test.
# The BSP provides main() and ut_assert provides the application hooks.

cmake_minimum_required(VERSION 3.20.0)

set(OSAL_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)
get_filename_component(OSAL_DIR ${OSAL_DIR} ABSOLUTE)
list(APPEND ZEPHYR_EXTRA_MODULES ${OSAL_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(osal_functional_test)

if(NOT OSAL_TEST)
  message(FATAL_ERROR "Set OSAL_TEST, e.g. -DOSAL_TEST=tests/bin-sem-test")
endif()

file(GLOB OSAL_TEST_SRCS ${OSAL_DIR}/src/${OSAL_TEST}/*.c)
if(NOT OSAL_TEST_SRCS)
  message(FATAL_ERROR "No test sources found in ${OSAL_DIR}/src/${OSAL_TEST}")
endif()

target_sources(app PRIVATE ${OSAL_TEST_SRCS})
target_include_directories(app PRIVATE ${OSAL_DIR}/src/unit-tests/inc)
target_link_libraries(app PRIVATE ut_assert)
