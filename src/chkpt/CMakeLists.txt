add_library(psm-bg-chkpt STATIC chkpt.h chkpt.cc)
target_link_libraries(psm-bg-chkpt criu boost_filesystem boost_chrono boost_system)
