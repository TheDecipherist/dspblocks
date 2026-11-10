cmake_minimum_required(VERSION 3.20)
project(dspblocks CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Header-only: a consumer just adds the include directory.
add_library(dspblocks INTERFACE)
target_include_directories(dspblocks INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

enable_testing()
add_executable(dsp_tests tests/test_blocks.cpp)
target_link_libraries(dsp_tests PRIVATE dspblocks)
add_test(NAME dsp_tests COMMAND dsp_tests)

# The Delay Designer gets its own binary: every check in it is a MEASUREMENT of
# rendered audio at three tempos, so it is slower than the rest and worth being
# able to run on its own while working on it.
add_executable(dsp_delay_designer_tests tests/test_delay_designer.cpp)
target_link_libraries(dsp_delay_designer_tests PRIVATE dspblocks)
add_test(NAME dsp_delay_designer_tests COMMAND dsp_delay_designer_tests)

# The super filter and the shared oversampler get their own binary, for the same
# reason the Delay Designer does: every check in them renders audio - the
# stability sweep alone is 6400 settings - so being able to run them alone while
# working on the filter is worth a second target.
add_executable(dsp_superfilter_tests tests/test_superfilter.cpp)
target_link_libraries(dsp_superfilter_tests PRIVATE dspblocks)
add_test(NAME dsp_superfilter_tests COMMAND dsp_superfilter_tests)

# The A/B renderer: the same signal through both reverb topologies, written out
# as files. Not a test - which of the two sounds better is not something a test
# can assert - so it is built on request and never run by ctest.
add_executable(dsp_reverb_ab EXCLUDE_FROM_ALL tools/reverb_ab.cpp)
add_executable(dsp_fdn_measure EXCLUDE_FROM_ALL tools/fdn_measure.cpp)
target_include_directories(dsp_fdn_measure PRIVATE include)
target_compile_features(dsp_fdn_measure PRIVATE cxx_std_17)
target_link_libraries(dsp_reverb_ab PRIVATE dspblocks)
