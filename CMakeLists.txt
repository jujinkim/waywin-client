cmake_minimum_required(VERSION 3.22)
project(WayWinProbe VERSION 0.1.0 LANGUAGES CXX)

include(CTest)
if(BUILD_TESTING)
  find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
  add_test(NAME probe-contracts
    COMMAND "${Python3_EXECUTABLE}" -m unittest discover -s tests -v)
  set_tests_properties(probe-contracts PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
endif()
if(WIN32)
  enable_language(RC)
  add_subdirectory(windows/probe)
endif()
