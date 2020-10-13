include(ExternalProject)
find_package(Git REQUIRED)

# Get protodb
ExternalProject_Add(
    hyrise_src
    PREFIX "vendor/hyrise"
    GIT_REPOSITORY "ssh://git@gitlab.db.in.tum.de:2222/thomasb/sql-parser.git"
    GIT_TAG develop
    TIMEOUT 10
    CMAKE_ARGS
        -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_CURRENT_BINARY_DIR}/vendor/hyrise
        -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
        -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
        -DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} -DLIBARY_TYPE_STATIC=true
)

# Prepare protodb
ExternalProject_Get_Property(hyrise_src install_dir)
set(HYRISE_INCLUDE_DIR ${install_dir}/include)
set(HYRISE_LIBRARY_PATH ${install_dir}/lib/libsqlparser.a)
file(MAKE_DIRECTORY ${HYRISE_INCLUDE_DIR})
add_library(hyriselib STATIC IMPORTED)
set_property(TARGET hyriselib PROPERTY IMPORTED_LOCATION ${HYRISE_LIBRARY_PATH})
set_property(TARGET hyriselib APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${HYRISE_INCLUDE_DIR})

# Dependencies
add_dependencies(hyriselib hyrise_src)

