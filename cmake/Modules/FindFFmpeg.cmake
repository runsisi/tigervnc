#
# runsisi AT hust.edu.cn
#

list(APPEND components avcodec avdevice avfilter avformat avutil)
list(APPEND components swresample)
list(APPEND components swscale)

foreach(c ${components})
    string(TOUPPER ${c} C)

    find_path(
        LIB${C}_INCLUDE_DIR
        NAMES
            ${c}.h
        HINTS
            ${FFMPEG_ROOT}/lib${c}
    )

    find_library(
        LIB${C}_LIBRARY
        NAMES
            "${CMAKE_STATIC_LIBRARY_PREFIX}${c}${CMAKE_STATIC_LIBRARY_SUFFIX}"
            lib${c}
        HINTS
            ${FFMPEG_ROOT}/lib${c}
    )

    get_filename_component(LIB${C}_INCLUDE_DIR "${LIB${C}_INCLUDE_DIR}" DIRECTORY)

    add_library(ffmpeg::${c} UNKNOWN IMPORTED)
    set_target_properties(
        ffmpeg::${c}
        PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${LIB${C}_INCLUDE_DIR}"
        IMPORTED_LINK_INTERFACE_LANGUAGES "C"
        IMPORTED_LOCATION "${LIB${C}_LIBRARY}"
    )
    list(APPEND FFMPEG_COMPONENTS ffmpeg::${c})
    list(APPEND FFMPEG_ARCHIVES "${LIB${C}_LIBRARY}")

    list(APPEND FFMPEG_INCLUDE_DIR "${LIB${C}_INCLUDE_DIR}")
    list(APPEND FFMPEG_LIBRARY "${LIB${C}_LIBRARY}")

    list(APPEND FFMPEG_VARS LIB${C}_INCLUDE_DIR)
    list(APPEND FFMPEG_VARS LIB${C}_LIBRARY)
endforeach()

list(REMOVE_DUPLICATES FFMPEG_INCLUDE_DIR)
list(REMOVE_DUPLICATES FFMPEG_LIBRARY)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
    FFmpeg
    DEFAULT_MSG
    ${FFMPEG_VARS}
)

mark_as_advanced(
    FFMPEG_COMPONENTS
    FFMPEG_ARCHIVES
    FFMPEG_INCLUDE_DIR
    FFMPEG_LIBRARY
    ${FFMPEG_VARS}
    FFMPEG_VARS
)

if(FFmpeg_FOUND)
    set(FFMPEG_INCLUDE_DIRS "${FFMPEG_INCLUDE_DIR}")
    set(FFMPEG_LIBRARIES "${FFMPEG_LIBRARY}")
endif()

if(FFmpeg_FOUND AND NOT (TARGET FFmpeg::FFmpeg))
    add_library(FFmpeg::FFmpeg INTERFACE IMPORTED)
    add_dependencies(FFmpeg::FFmpeg ${FFMPEG_COMPONENTS})
    set_target_properties(
        FFmpeg::FFmpeg
        PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES
        ${FFMPEG_INCLUDE_DIRS}
        INTERFACE_LINK_LIBRARIES
        "-Wl,--start-group $<JOIN:${FFMPEG_ARCHIVES}, > -Wl,--end-group"
    )
endif()
