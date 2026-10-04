# zen_platform and iGUI, pinned to a commit and fetched at configure time. A plain variable, so that moving a pin
# here takes effect on the next configure instead of losing to the value an older configure left in the cache.
include(FetchContent)

set(MCPCHAT_ZEN_PLATFORM_TAG 9c6804885b9058526e91e71928ad686122c160be)
set(MCPCHAT_IGUI_TAG 7ddc1ea1726b962e09395097c088fa7fa0b2c50a)
# iGUI's submodule names a containers commit missing from its remote, so containers is fetched on its own.
set(MCPCHAT_CONTAINERS_TAG e44adee6265b60ead8304e0826e33d6f35ea4754)

if(MCPCHAT_BUILD_GUI)
    set(PLATFORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(PLATFORM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(zen_platform
        GIT_REPOSITORY https://github.com/akadjoker/zen_plataform.git
        GIT_TAG ${MCPCHAT_ZEN_PLATFORM_TAG})

    FetchContent_Declare(containers
        GIT_REPOSITORY https://github.com/akadjoker/containers.git
        GIT_TAG ${MCPCHAT_CONTAINERS_TAG}
        SOURCE_SUBDIR none)
    FetchContent_MakeAvailable(containers)

    set(IGUI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(IGUI_BUILD_RETAINED OFF CACHE BOOL "" FORCE)
    set(IGUI_BUILD_LEGACY OFF CACHE BOOL "" FORCE)
    set(IGUI_BUILD_RAYLIB_BACKEND OFF CACHE BOOL "" FORCE)
    set(IGUI_BUILD_FONT ON CACHE BOOL "" FORCE)
    FetchContent_Declare(igui
        GIT_REPOSITORY https://github.com/akadjoker/iGUI.git
        GIT_TAG ${MCPCHAT_IGUI_TAG}
        GIT_SUBMODULES ""
        PATCH_COMMAND ${CMAKE_COMMAND} -E copy_directory ${containers_SOURCE_DIR}/include
                      <SOURCE_DIR>/third_party/containers/include)

    FetchContent_MakeAvailable(zen_platform igui)
endif()
