add_executable(membench_dedup membench_dedup.cc)
target_link_libraries(membench_dedup PRIVATE psm pmem)
target_compile_options(membench_dedup PRIVATE -mclwb -mavx2)
configure_file(bench_dedup.py bench_dedup.py COPYONLY)

add_executable(membench_stack membench_stack.cc)
target_link_libraries(membench_stack PRIVATE psm)
target_compile_options(membench_stack PRIVATE -mavx2)
configure_file(bench_stack.py bench_stack.py COPYONLY)

add_executable(membench_batch membench_batch.cc)
target_link_libraries(membench_batch PRIVATE psm)
target_compile_options(membench_batch PRIVATE -mavx2)
configure_file(bench_batch.py bench_batch.py COPYONLY)
