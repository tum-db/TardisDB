include(ExternalProject)
find_package(Git REQUIRED)
find_package(Threads REQUIRED)

# Get googletest
ExternalProject_Add(
        pugixml_src
        PREFIX "vendor/pugixml"
        GIT_REPOSITORY "https://github.com/zeux/pugixml.git"
        GIT_TAG v1.10
        TIMEOUT 10
        CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_CURRENT_BINARY_DIR}/vendor/pugixml
)

# Prepare gtest
ExternalProject_Get_Property(pugixml_src install_dir)
set(PUGIXML_INCLUDE_DIR ${install_dir}/include)
set(PUGIXML_LIBRARY_PATH ${install_dir}/lib/libpugixml.a)
file(MAKE_DIRECTORY ${PUGIXML_INCLUDE_DIR})
add_library(pugixml STATIC IMPORTED)
set_property(TARGET pugixml PROPERTY IMPORTED_LOCATION ${PUGIXML_LIBRARY_PATH})
set_property(TARGET pugixml APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${PUGIXML_INCLUDE_DIR})

# Get googletest
ExternalProject_Add(
        libxml++_src
        PREFIX "vendor/libxml++"
        GIT_REPOSITORY "https://github.com/GNOME/libxmlplusplus.git"
        GIT_TAG master
        CONFIGURE_COMMAND ${CMAKE_CURRENT_BINARY_DIR}/vendor/libxml++/src/libxml++_src/autogen.sh --without-python --prefix=${CMAKE_CURRENT_BINARY_DIR}/libxml++}
        CC=${CMAKE_C_COMPILER}
        CXX=${CMAKE_CXX_COMPILER}
        CFLAGS=${LIBXML2_CFLAGS}
        CXXFLAGS=${LIBXML2_CXXFLAGS}
        BUILD_COMMAND make -j ${CPU_COUNT} all
        INSTALL_COMMAND make install
        TIMEOUT 10
)

# Prepare gtest
ExternalProject_Get_Property(libxml++_src install_dir)
set(PUGIXML_INCLUDE_DIR ${install_dir}/include)
set(PUGIXML_LIBRARY_PATH ${install_dir}/lib/libxml++.a)
file(MAKE_DIRECTORY ${PUGIXML_INCLUDE_DIR})
add_library(libxml++ STATIC IMPORTED)
set_property(TARGET pugixml PROPERTY IMPORTED_LOCATION ${PUGIXML_LIBRARY_PATH})
set_property(TARGET pugixml APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${PUGIXML_INCLUDE_DIR})

# Get googletest
#ExternalProject_Add(
#        libxml2_src
#        PREFIX "vendor/libxml2"
#        GIT_REPOSITORY "https://github.com/GNOME/libxml2.git"
#        GIT_TAG master
#        TIMEOUT 10
#        CMAKE_ARGS      -DCMAKE_INSTALL_PREFIX:PATH=${CMAKE_CURRENT_BINARY_DIR}/vendor/libxml2

#)

# Prepare gtest
#ExternalProject_Get_Property(libxml2_src install_dir)
#set(LIBXML2_INCLUDE_DIR ${install_dir}/include)
#set(LIBXML2_LIBRARY_PATH ${install_dir}/lib/libxml2.a)
#file(MAKE_DIRECTORY ${LIBXML2_INCLUDE_DIR})
#add_library(libxml2 STATIC IMPORTED)
#set_property(TARGET libxml2 PROPERTY IMPORTED_LOCATION ${LIBXML2_LIBRARY_PATH})
#set_property(TARGET libxml2 APPEND PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${LIBXML2_INCLUDE_DIR})
