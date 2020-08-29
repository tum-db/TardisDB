# ---------------------------------------------------------------------------
# WIKIPARSER
# ---------------------------------------------------------------------------

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBXML REQUIRED libxml++-2.6)
pkg_check_modules(GLIBMM REQUIRED glibmm-2.4)

# ---------------------------------------------------------------------------
# Files
# ---------------------------------------------------------------------------

file(GLOB_RECURSE SRC_CC "tools/wikiParser/*.cpp")

# ---------------------------------------------------------------------------
# Libraries
# ---------------------------------------------------------------------------

add_library(wikiparser STATIC ${SRC_CC})
target_include_directories(wikiparser PUBLIC include)
target_include_directories(wikiparser SYSTEM PUBLIC ${LIBXML_INCLUDE_DIRS})
target_include_directories(wikiparser SYSTEM PUBLIC ${GLIBMM_INCLUDE_DIRS})
target_link_libraries(wikiparser ${LIBXML_LIBRARIES} ${GLIBMM_LIBRARIES})