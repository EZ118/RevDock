if(TARGET Freetype::Freetype)
    set(FREETYPE_FOUND TRUE)
    get_target_property(_ft_inc Freetype::Freetype INTERFACE_INCLUDE_DIRECTORIES)
    if(NOT _ft_inc)
        set(_ft_inc "${CMAKE_SOURCE_DIR}/lib/freetype/include")
    endif()
    set(FREETYPE_INCLUDE_DIRS "${_ft_inc}")
    set(FREETYPE_LIBRARIES Freetype::Freetype)
    set(FREETYPE_VERSION_STRING "2.14.3")
    return()
endif()

# Fallback for system packages when the vendored target is unavailable.
find_package(Freetype QUIET)
