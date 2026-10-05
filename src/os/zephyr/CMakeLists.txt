######################################################################
#
# CMAKE build recipe for Zephyr OSAL implementation
#
######################################################################

# This CMake script generates targets specific to the Zephyr implementation

# The basic set of files which are always built
set(ZEPHYR_BASE_SRCLIST
    src/os-impl-binsem.c
    src/os-impl-clock.c
    src/os-impl-common.c
    src/os-impl-condvar.c
    src/os-impl-console.c
    src/os-impl-countsem.c
    src/os-impl-errors.c
    src/os-impl-idmap.c
    src/os-impl-mutex.c
    src/os-impl-queues.c
    src/os-impl-tasks.c
    src/os-impl-timebase.c
)

set(ZEPHYR_IMPL_SRCLIST
    ../portable/os-impl-console-bsp.c
)

# Areas not yet ported to Zephyr. Every call reports OS_ERR_NOT_IMPLEMENTED
# so the full OSAL links; replace each entry when its area is implemented.
list(APPEND ZEPHYR_IMPL_SRCLIST
    src/os-impl-no-dirs.c
    src/os-impl-no-files.c
    src/os-impl-no-filesys.c
    src/os-impl-no-heap.c
    ../portable/os-impl-no-loader.c
    ../portable/os-impl-no-network-gethostid.c
    ../portable/os-impl-no-network-gethostname.c
    ../portable/os-impl-no-rwlock.c
    ../portable/os-impl-no-select.c
    ../portable/os-impl-no-shell.c
    ../portable/os-impl-no-sockets.c
    ../portable/os-impl-no-symtab.c
)

# Defines an OBJECT target named "osal_zephyr_impl" with selected source files
add_library(osal_zephyr_impl OBJECT
    ${ZEPHYR_BASE_SRCLIST}
    ${ZEPHYR_IMPL_SRCLIST}
)

target_link_libraries(osal_zephyr_impl PUBLIC zephyr_interface)
add_dependencies(osal_zephyr_impl zephyr_generated_headers)

target_include_directories(osal_zephyr_impl PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
)
