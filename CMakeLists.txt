cmake_minimum_required(VERSION 3.5)
project(protodb)

set(CMAKE_CXX_STANDARD 17)

set(LIBXML_AVAILABLE false)
set(ENABLE_PERFBENCHMARKING false)
set(USE_DATA_VERSIONING true)
set(USE_HYRISE false)
set(TUPLE_STREAM_REQUIRED true)

##################
### boost ########
##################
if(APPLE)
	list(APPEND CMAKE_PREFIX_PATH /usr/local/opt/boost/lib/cmake/Boost-1.72.0)
endif(APPLE)
find_package(Boost)
INCLUDE_DIRECTORIES( ${Boost_INCLUDE_DIRS} )

##################
### llvm #########
##################
if(APPLE)
	list(APPEND CMAKE_PREFIX_PATH /usr/local/opt/llvm@7)
	find_package(LLVM 7.1 REQUIRED CONFIG)
else(NOT APPLE)
	find_package(LLVM 7 REQUIRED CONFIG)
endif(APPLE)
message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
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

##################
### vendor #######
##################
include("${CMAKE_SOURCE_DIR}/vendor/googletest.cmake")
include("${CMAKE_SOURCE_DIR}/vendor/gflags.cmake")
if(ENABLE_PERFBENCHMARKING)
	include("${CMAKE_SOURCE_DIR}/vendor/perfevent.cmake")
endif(ENABLE_PERFBENCHMARKING)
if(USE_HYRISE)
	include("${CMAKE_SOURCE_DIR}/vendor/hyrise.cmake")
endif(USE_HYRISE)

##################
### config #######
##################
include_directories(.
		${LLVM_INCLUDE_DIRS}
		${GFLAGS_INCLUDE_DIR}
		${HYRISE_INCLUDE_DIR}
		${CMAKE_SOURCE_DIR}/include/tardisdb
		)
add_definitions(${LLVM_DEFINITIONS})

##################
### libraries ####
##################
if(NOT USE_HYRISE)
	include(tools/sqlParser/local.cmake)
endif(NOT USE_HYRISE)
include(tools/semanticalAnalyser/local.cmake)
include(tools/queryCompiler/local.cmake)
include(tools/queryExecutor/local.cmake)
if(LIBXML_AVAILABLE)
	include(tools/wikiParser/local.cmake)
endif(LIBXML_AVAILABLE)

if (DEFINED PISTACHE_INCLUDE)
    include_directories("${PISTACHE_INCLUDE}")
endif()

if (DEFINED PISTACHE_LIBRARY_PATH)
    link_directories("${PISTACHE_LIBRARY_PATH}")
endif()
##################
### tests ########
##################
include("${CMAKE_SOURCE_DIR}/test/local.cmake")

##################
### tools ########
##################
add_executable(sql sql.cpp)
target_link_libraries(sql readline dblib queryCompiler queryExecutor)

add_executable(server server.cpp)
target_link_libraries(server dblib queryCompiler queryExecutor pistache pthread)

##################
### benchmark ####
##################
add_executable(semanticalBench benchmark/semanticalBench.cpp)
target_link_libraries(semanticalBench dblib queryCompiler queryExecutor gflags pthread)
if(LIBXML_AVAILABLE)
	target_compile_definitions(semanticalBench PUBLIC -D LIBXML_AVAILABLE=true)
	target_link_libraries(semanticalBench wikiparser)
endif(LIBXML_AVAILABLE)
if(USE_DATA_VERSIONING)
	target_compile_definitions(semanticalBench PUBLIC -D USE_DATA_VERSIONING=true)
endif(USE_DATA_VERSIONING)
if(ENABLE_PERFBENCHMARKING)
	target_compile_definitions(semanticalBench PUBLIC -D PERF_AVAILABLE=true)
endif(ENABLE_PERFBENCHMARKING)

##################
### DBLIB ########
##################
file(GLOB_RECURSE ALGEBRA_FILES "algebra/*.cpp" "algebra/*.hpp" "algebra/*.tcc")
file(GLOB_RECURSE CODEGEN_FILES "codegen/*.cpp" "codegen/*.hpp" "codegen/*.tcc")
file(GLOB_RECURSE FOUNDATIONS_FILES "foundations/*.cpp" "foundations/*.hpp" "foundations/*.tcc")
file(GLOB_RECURSE NATIVE_FILES "native/*.cpp" "native/*.hpp" "native/*.tcc")
file(GLOB_RECURSE SQL_FILES "sql/*.cpp" "sql/*.hpp" "sql/*.tcc")
file(GLOB_RECURSE QUERIES_FILES "queries/*.cpp" "queries/*.hpp" "queries/*.tcc")
#file(GLOB_RECURSE TESTS_FILES "tests/*.cpp" "tests/*.hpp" "tests/*.tcc")
file(GLOB_RECURSE UTILS_FILES "utils/*.cpp" "utils/*.hpp" "utils/*.tcc")

set(THIRD_PARTY_FILES third_party/hexdump.cpp third_party/hexdump.hpp)

set(DB_LIB_SOURCE_FILES
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
	tests/types_test.cpp
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
if(TUPLE_STREAM_REQUIRED)
	target_compile_definitions(dblib PUBLIC -D TUPLE_STREAM_REQUIRED=true)
endif(TUPLE_STREAM_REQUIRED)
if(USE_DATA_VERSIONING)
	target_compile_definitions(dblib PUBLIC -D USE_DATA_VERSIONING=true)
endif(USE_DATA_VERSIONING)
target_link_libraries(dblib semanticalAnalyser LLVM)
if(USE_HYRISE)
	target_compile_definitions(dblib PUBLIC -D USE_HYRISE=true)
	target_link_libraries(dblib hyriselib)
else(NOT USE_HYRISE)
	target_compile_definitions(dblib PUBLIC -D USE_HYRISE=false)
	target_link_libraries(dblib sqlparser)
endif(USE_HYRISE)

### main application
add_executable(protodb main.cpp ${GENERATED_SOURCE_FILES} ${TEST_SOURCE_FILES})
target_link_libraries(protodb dblib queryCompiler queryExecutor)

### set up dependencies
add_dependencies(GenerateLoader schema_parser)
add_dependencies(protodb GenerateLoader)

### sub projects
add_subdirectory(schema_parser)

### benchmarks
set(RD_BENCH_SOURCE_FILES
	benchmark/random.cpp
)

add_executable(rd_bench ${RD_BENCH_SOURCE_FILES})
target_link_libraries(rd_bench dblib)

##################
### install ######
##################
MACRO(INSTALL_HEADERS_WITH_DIRECTORY HEADER_LIST)
    FOREACH(HEADER ${${HEADER_LIST}})
		get_filename_component(DIR ${HEADER} DIRECTORY)
        install(FILES ${HEADER} DESTINATION include/${DIR})
    ENDFOREACH(HEADER)
ENDMACRO(INSTALL_HEADERS_WITH_DIRECTORY)
file(GLOB_RECURSE HS RELATIVE ${CMAKE_SOURCE_DIR} "*.hpp" "*.tcc")

INSTALL_HEADERS_WITH_DIRECTORY(HS)
install(TARGETS dblib DESTINATION lib)
install(TARGETS sql protodb rd_bench DESTINATION bin)
