add_compile_options(-fPIC -Wall -Wextra -Wno-uninitialized -Wno-unused-parameter -Wl,-zdefs)

add_subdirectory(chkpt)
add_subdirectory(undo)

add_library(psm-bg STATIC bg.h bg.cc internal.h)
target_link_libraries(psm-bg psm-bg-chkpt psm-bg-undo)
target_compile_options(psm-bg PRIVATE -mavx -mclwb)

add_library(psm SHARED fg.cc internal.h)
target_link_libraries(psm PRIVATE psm-bg psm-fg-undo)
target_link_libraries(psm PRIVATE pmem)
target_compile_options(psm PRIVATE -fvisibility=hidden -mavx -mclwb)
# Unfortunately, I don't think the target will rebuild automatically after the version script is modified.
target_link_options(psm PRIVATE "LINKER:--version-script,${CMAKE_CURRENT_SOURCE_DIR}/libpsm.version")
