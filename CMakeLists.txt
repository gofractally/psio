cmake_minimum_required(VERSION 3.20)
project(psio VERSION 0.1.0 LANGUAGES C CXX)

# Support a fresh checkout, add_subdirectory, and FetchContent at the
# repository root. Direct builds of cpp/ remain supported.
option(PSIO_ENABLE_TESTS "Build psio tests" OFF)
if(PSIO_ENABLE_TESTS)
   enable_testing()
endif()
add_subdirectory(cpp)
