cmake_minimum_required(VERSION 3.5)
project(protodb)

set(CMAKE_CXX_STANDARD 17)

# optimised and debuggable unless a build type is given
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
endif()

set(LIBXML_AVAILABLE false)
set(ENABLE_PERFBENCHMARKING false)
option(INTERVAL_VISIBILITY "decide version visibility with interval labels of branch identifiers instead of the lineage map" OFF)
option(COMPILED_LINEAGE "without INTERVAL_VISIBILITY: compile the lineage test into the scans (one array lookup per version, with the same in-place fast path as the labels)" OFF)
option(LAZY_MEMBERSHIP "keep the membership of a branch as its changes against its parent instead of one bitmap column per branch" OFF)

### common flags
# TODO add -Wall -Wextra
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fPIC -rdynamic -fno-omit-frame-pointer -march=native -Wno-unknown-pragmas")

### set up boost
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
# LLVM 7 is the original target; newer versions work through codegen/LLVMCompat.hpp (tested: 18).
# TARDISDB_LLVM_VERSION selects the major version; left empty, the first installed of 18 and 7 is used.
# Searched are LLVM_DIR and the versioned directories of Debian/Ubuntu and Homebrew only, so that
# another installed LLVM (e.g. a partly removed one) is not picked up instead.
set(TARDISDB_LLVM_VERSION "" CACHE STRING "major version of LLVM to build against (empty: the first installed of 18 and 7)")
if(TARDISDB_LLVM_VERSION)
	set(TARDISDB_LLVM_CANDIDATES ${TARDISDB_LLVM_VERSION})
else()
	set(TARDISDB_LLVM_CANDIDATES 18 7)
endif()
set(TARDISDB_LLVM_FOUND FALSE)
foreach(version IN LISTS TARDISDB_LLVM_CANDIDATES)
	# an LLVM_DIR of another version, e.g. cached by an earlier configure, is dropped
	if(LLVM_DIR)
		set(llvm_dir_version "")
		if(EXISTS "${LLVM_DIR}/LLVMConfigVersion.cmake")
			file(STRINGS "${LLVM_DIR}/LLVMConfigVersion.cmake" llvm_dir_version REGEX "^set\\(PACKAGE_VERSION ")
		endif()
		if(NOT llvm_dir_version MATCHES "\"${version}\\.")
			unset(LLVM_DIR CACHE)
		endif()
	endif()
	find_package(LLVM CONFIG QUIET NO_DEFAULT_PATH PATHS
			/usr/lib/llvm-${version}/lib/cmake/llvm
			/usr/local/opt/llvm@${version}/lib/cmake/llvm
			/opt/homebrew/opt/llvm@${version}/lib/cmake/llvm)
	if(LLVM_FOUND AND LLVM_VERSION_MAJOR EQUAL version)
		set(TARDISDB_LLVM_FOUND TRUE)
		break()
	endif()
	unset(LLVM_DIR CACHE)
endforeach()
if(NOT TARDISDB_LLVM_FOUND)
	string(REPLACE ";" " or " llvm_wanted "${TARDISDB_LLVM_CANDIDATES}")
	message(FATAL_ERROR "LLVM ${llvm_wanted} not found (TARDISDB_LLVM_VERSION is \"${TARDISDB_LLVM_VERSION}\"). "
			"Install llvm-18-dev, or select an installed version with -DTARDISDB_LLVM_VERSION=<major> "
			"(empty: the first of 18 and 7) or its directory with -DLLVM_DIR=<prefix>/lib/cmake/llvm.")
endif()
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

##################
### config #######
##################
include_directories(.
		${LLVM_INCLUDE_DIRS}
		${GFLAGS_INCLUDE_DIR}
		${CMAKE_SOURCE_DIR}/tools
		)
add_definitions(${LLVM_DEFINITIONS})

##################
### libraries ####
##################
include(tools/sqlParser/local.cmake)
include(tools/semanticAnalyser/local.cmake)
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
target_link_libraries(sql readline dblib queryCompiler queryExecutor pthread)

# REST server: only with the Pistache HTTP library (PISTACHE_INCLUDE, PISTACHE_LIBRARY_PATH)
find_path(PISTACHE_HEADER_DIR pistache/endpoint.h HINTS ${PISTACHE_INCLUDE})
find_library(PISTACHE_LIB pistache HINTS ${PISTACHE_LIBRARY_PATH})
if(PISTACHE_HEADER_DIR AND PISTACHE_LIB)
	add_executable(server server.cpp)
	target_include_directories(server PRIVATE ${PISTACHE_HEADER_DIR})
	target_link_libraries(server dblib queryCompiler queryExecutor ${PISTACHE_LIB} pthread)
else()
	message(STATUS "Pistache not found: the REST server (target server) is not built")
endif()

# PostgreSQL-compatible server (frontend/backend protocol 3.0)
add_executable(pgserver pgserver.cpp)
target_link_libraries(pgserver dblib queryCompiler queryExecutor pthread)

##################
### benchmark ####
##################
add_executable(branchBench benchmark/branchBench.cpp)
target_link_libraries(branchBench dblib queryCompiler queryExecutor gflags pthread)
add_executable(semanticalBench benchmark/semanticalBench.cpp)
target_link_libraries(semanticalBench dblib queryCompiler queryExecutor gflags pthread)
if(LIBXML_AVAILABLE)
	target_compile_definitions(semanticalBench PUBLIC -D LIBXML_AVAILABLE=true)
	target_link_libraries(semanticalBench wikiparser)
endif(LIBXML_AVAILABLE)
if(ENABLE_PERFBENCHMARKING)
	target_compile_definitions(semanticalBench PUBLIC -D PERF_AVAILABLE=true)
endif(ENABLE_PERFBENCHMARKING)

##################
### DBLIB ########
##################
file(GLOB_RECURSE ALGEBRA_FILES "algebra/*.cpp" "algebra/*.hpp" "algebra/*.tcc")
file(GLOB_RECURSE CODEGEN_FILES "codegen/*.cpp" "codegen/*.hpp" "codegen/*.tcc")
file(GLOB_RECURSE FOUNDATIONS_FILES "foundations/*.cpp" "foundations/*.hpp" "foundations/*.tcc")
file(GLOB_RECURSE RUNTIME_FILES "runtime/*.cpp" "runtime/*.hpp" "runtime/*.tcc")
file(GLOB_RECURSE SQL_FILES "sql/*.cpp" "sql/*.hpp" "sql/*.tcc")
file(GLOB_RECURSE UTILS_FILES "utils/*.cpp" "utils/*.hpp" "utils/*.tcc")

# the adaptive radix tree of the indexes; Tree.cpp includes the nodes' sources
set(THIRD_PARTY_FILES third_party/hexdump.cpp third_party/hexdump.hpp third_party/ART/Tree.cpp)

set(DB_LIB_SOURCE_FILES
	${ALGEBRA_FILES}
	${CODEGEN_FILES}
	${FOUNDATIONS_FILES}
	${RUNTIME_FILES}
	${SQL_FILES}
	${QUERY_COMPILER_FILES}
	${UTILS_FILES}
	${THIRD_PARTY_FILES}
)

### main library
add_library(dblib ${DB_LIB_SOURCE_FILES})
target_link_libraries(dblib semanticAnalyser LLVM dl pthread)
if(INTERVAL_VISIBILITY)
	target_compile_definitions(dblib PUBLIC -D INTERVAL_VISIBILITY=1)
endif(INTERVAL_VISIBILITY)
if(COMPILED_LINEAGE)
	if(INTERVAL_VISIBILITY)
		message(FATAL_ERROR "COMPILED_LINEAGE replaces the lineage map; it does not combine with INTERVAL_VISIBILITY")
	endif(INTERVAL_VISIBILITY)
	target_compile_definitions(dblib PUBLIC -D COMPILED_LINEAGE=1)
endif(COMPILED_LINEAGE)
if(LAZY_MEMBERSHIP)
	target_compile_definitions(dblib PUBLIC -D LAZY_MEMBERSHIP=1)
endif(LAZY_MEMBERSHIP)
target_link_libraries(dblib sqlparser)

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
install(TARGETS sql pgserver rd_bench DESTINATION bin)
