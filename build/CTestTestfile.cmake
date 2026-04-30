# CMake generated Testfile for 
# Source directory: /home/vakshay/exchange-engine
# Build directory: /home/vakshay/exchange-engine/build
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(OrderBookTests "/home/vakshay/exchange-engine/build/tests")
set_tests_properties(OrderBookTests PROPERTIES  _BACKTRACE_TRIPLES "/home/vakshay/exchange-engine/CMakeLists.txt;25;add_test;/home/vakshay/exchange-engine/CMakeLists.txt;0;")
subdirs("_deps/googletest-build")
