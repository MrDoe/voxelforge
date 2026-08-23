# CMake generated Testfile for 
# Source directory: /home/christoph/code/voxelforge
# Build directory: /home/christoph/code/voxelforge/build-dbg
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[unit_tests]=] "/home/christoph/code/voxelforge/build-dbg/vf_tests")
set_tests_properties([=[unit_tests]=] PROPERTIES  _BACKTRACE_TRIPLES "/home/christoph/code/voxelforge/CMakeLists.txt;129;add_test;/home/christoph/code/voxelforge/CMakeLists.txt;0;")
subdirs("_deps/spdlog-build")
subdirs("_deps/glfw-build")
