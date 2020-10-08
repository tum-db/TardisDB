set(SOURCE_FILES cpu.cpp common.cpp)

#include_directories(${CMAKE_SOURCE_DIR}/src)

add_executable(tpch ${SOURCE_FILES})
target_include_directories(tpch PRIVATE $<BUILD_INTERFACE:${JEVENTS_INCLUDE_DIR}>)
target_link_libraries(tpch ${JEVENTS_LIBRARY_PATH})
add_dependencies(tpch jevents)

add_executable(tpch_gpu gpu.cu common.cpp)
