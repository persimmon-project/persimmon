find_package(DynamoRIO)
if (NOT DynamoRIO_FOUND)
    message(FATAL_ERROR "DynamoRIO package required to build")
endif (NOT DynamoRIO_FOUND)

include_directories(${DynamoRIO_INCLUDE_DIRS})

add_library(psm-bg-undo STATIC
        undo_bg.h state.h
        initial_chkpt.cc
        instrument.cc undo_log.h
        mem_region/mem_region.h mem_region/mem_region.cc
        mem_region/common.h mem_region/fg.h
        mem_region/dir_iter.h
        flush.h memset_nt_avx.cc
        my_libc/my_libc.cc my_libc/my_libc.h my_libc/prohibit_libc.h
        my_libc/musl/memset.s my_libc/musl/memcpy.s my_libc/musl/memmove.s
        my_libc/musl/strcpy.c my_libc/musl/strncpy.c my_libc/musl/strcmp.c
        my_libc/musl/strlen.c my_libc/musl/memcpy.c
        )
configure_DynamoRIO_static(psm-bg-undo)
use_DynamoRIO_extension(psm-bg-undo drreg_static)
use_DynamoRIO_extension(psm-bg-undo drmgr_static)
use_DynamoRIO_extension(psm-bg-undo drutil_static)
use_DynamoRIO_extension(psm-bg-undo drwrap_static)
use_DynamoRIO_extension(psm-bg-undo drcontainers)
target_link_libraries(psm-bg-undo criu)
target_compile_options(psm-bg-undo PRIVATE -DUSE_VISIBILITY_ATTRIBUTES -mavx -mclwb)
target_link_options(psm-bg-undo PRIVATE "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/bg.version")

add_library(psm-fg-undo STATIC
        undo_fg.h undo_fg.cc
        mem_region/common.h mem_region/fg.h mem_region/fg.cc
        )
