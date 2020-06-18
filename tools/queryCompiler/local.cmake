# ---------------------------------------------------------------------------
# SQLPARSER
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

file(GLOB_RECURSE SRC_CC "tools/queryCompiler/*.cpp")

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

add_library(queryCompiler STATIC ${SRC_CC})
target_link_libraries(queryCompiler dblib queryExecutor)