### Files
file(GLOB_RECURSE TEST_CC test/*.cpp)
list(REMOVE_ITEM TEST_CC test/tester.cpp)

### Executable
add_executable(tester test/tester.cpp ${TEST_CC})
target_link_libraries(tester dblib queryCompiler queryExecutor gtest gmock Threads::Threads)
enable_testing()
add_test(dblib tester)