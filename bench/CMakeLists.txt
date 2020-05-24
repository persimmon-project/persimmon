add_executable(membench_dedup membench_dedup.cc)
target_link_libraries(membench_dedup PRIVATE psm pmem)
target_compile_options(membench_dedup PRIVATE -mclwb -mavx2)
configure_file(bench_repeat.py bench_repeat.py COPYONLY)
