include(ExternalProject)
find_package(Git REQUIRED)
find_package(Threads REQUIRED)

# Get perfevent
ExternalProject_Add(
        perfevent
        PREFIX "vendor/perfevent"
        GIT_REPOSITORY "https://github.com/viktorleis/perfevent.git"
        GIT_TAG master
        TIMEOUT 10
        CONFIGURE_COMMAND ""
        BUILD_COMMAND ""
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
)

# Prepare perfevent
ExternalProject_Get_Property(perfevent install_dir)
set(PERFEVENT_INCLUDE_DIR ${install_dir}/include)
file(MAKE_DIRECTORY ${PERFEVENT_INCLUDE_DIR})
file(COPY ${install_dir}/src/perfevent DESTINATION ${PERFEVENT_INCLUDE_DIR})
#include_directories(${PERFEVENT_INCLUDE_DIR})