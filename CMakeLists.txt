cmake_minimum_required(VERSION 3.5)
project(protodb)

set(CMAKE_CXX_STANDARD 17)

### set up llvm
find_package(LLVM REQUIRED CONFIG)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
include_directories(${LLVM_INCLUDE_DIRS} .)
add_definitions(${LLVM_DEFINITIONS})

llvm_map_components_to_libnames(LLVM_LIBS
	support
	core
	irreader
	ExecutionEngine
	nativecodegen
	X86AsmParser
	mcjit
	Passes
	InstCombine
	ScalarOpts
)

### locate source files
file(GLOB_RECURSE ALGEBRA_FILES "algebra/*.cpp" "algebra/*.hpp" "algebra/*.tcc")
file(GLOB_RECURSE CODEGEN_FILES "codegen/*.cpp" "codegen/*.hpp" "codegen/*.tcc")
file(GLOB_RECURSE FOUNDATIONS_FILES "foundations/*.cpp" "foundations/*.hpp" "foundations/*.tcc")
file(GLOB_RECURSE NATIVE_FILES "native/*.cpp" "native/*.hpp" "native/*.tcc")
file(GLOB_RECURSE SQL_FILES "sql/*.cpp" "sql/*.hpp" "sql/*.tcc")
file(GLOB_RECURSE QUERIES_FILES "queries/*.cpp" "queries/*.hpp" "queries/*.tcc")
file(GLOB_RECURSE QUERY_COMPILER_FILES "query_compiler/*.cpp" "query_compiler/*.hpp" "query_compiler/*.tcc")
#file(GLOB_RECURSE TESTS_FILES "tests/*.cpp" "tests/*.hpp" "tests/*.tcc")
file(GLOB_RECURSE UTILS_FILES "utils/*.cpp" "utils/*.hpp" "utils/*.tcc")

set(THIRD_PARTY_FILES third_party/hexdump.cpp third_party/hexdump.hpp)

set(DB_LIB_SOURCE_FILES
	main.cpp
	${ALGEBRA_FILES}
	${CODEGEN_FILES}
	${FOUNDATIONS_FILES}
	${NATIVE_FILES}
	${SQL_FILES}
	${QUERIES_FILES}
	${QUERY_COMPILER_FILES}
	${UTILS_FILES}
	${THIRD_PARTY_FILES}
)

set(TEST_SOURCE_FILES
#    tests/join_test.cpp # TODO update
	tests/if_test.cpp
	tests/loop_test.cpp
	tests/interop_test.cpp
	tests/codegen_test.cpp
	tests/types_test.cpp
#    tests/query_test.cpp
	tests/expression_test.cpp
	tests/groupby_test.cpp
	tests/tests.cpp
	tests/tests.hpp
)

### default schema file
get_filename_component(SCHEMA_FILE schemas/schema.sql ABSOLUTE)

### database loader generation
set(LOADER_FILE loader_generated.cpp)

add_custom_command(
	OUTPUT ${LOADER_FILE}
	COMMAND schema_parser ${SCHEMA_FILE} > ${LOADER_FILE}
	DEPENDS ${SCHEMA_FILE} schema_parser
	COMMENT "Generating database loader"
	VERBATIM
)

add_custom_target(
	GenerateLoader ALL
	DEPENDS ${LOADER_FILE}
)

### all generated files
set(GENERATED_SOURCE_FILES ${LOADER_FILE})

### main library
add_library(dblib ${DB_LIB_SOURCE_FILES})
target_link_libraries(dblib LLVM)

### main application
add_executable(protodb ${GENERATED_SOURCE_FILES} ${TEST_SOURCE_FILES})
target_link_libraries(protodb dblib)

### set up dependencies
add_dependencies(GenerateLoader schema_parser)
add_dependencies(protodb GenerateLoader)

### sub projects
add_subdirectory(standalone_tests)
add_subdirectory(schema_parser)

### benchmarks
set(RD_BENCH_SOURCE_FILES
	benchmark/random.cpp
)

add_executable(rd_bench ${RD_BENCH_SOURCE_FILES})
target_link_libraries(rd_bench dblib)

### install
MACRO(INSTALL_HEADERS_WITH_DIRECTORY HEADER_LIST)
    FOREACH(HEADER ${${HEADER_LIST}})
#		message(STATUS ${HEADER})
		get_filename_component(DIR ${HEADER} DIRECTORY)
#		message(STATUS ${DIR})
        install(FILES ${HEADER} DESTINATION include/${DIR})
    ENDFOREACH(HEADER)
ENDMACRO(INSTALL_HEADERS_WITH_DIRECTORY)

file(GLOB_RECURSE HS RELATIVE ${CMAKE_SOURCE_DIR} "*.hpp" "*.tcc")
#message(STATUS ${HS})
INSTALL_HEADERS_WITH_DIRECTORY(HS)

install(TARGETS dblib DESTINATION lib)
install(TARGETS protodb rd_bench DESTINATION bin)
