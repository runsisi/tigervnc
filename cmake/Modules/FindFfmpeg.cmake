find_package(PkgConfig)

if (PKG_CONFIG_FOUND)
	pkg_check_modules(AVCODEC libavcodec)
  pkg_check_modules(AVFILTER libavfilter)
	pkg_check_modules(AVUTIL libavutil)
	pkg_check_modules(SWSCALE libswscale)
else()
	find_path(AVCODEC_INCLUDE_DIRS NAMES avcodec.h PATH_SUFFIXES libavcodec)
	find_library(AVCODEC_LIBRARIES NAMES avcodec)
	find_package_handle_standard_args(AVCODEC DEFAULT_MSG AVCODEC_LIBRARIES AVCODEC_INCLUDE_DIRS)

  # libavfilter
  find_path(AVFILTER_INCLUDE_DIRS NAMES avfilter.h PATH_SUFFIXES libavfilter)
  find_library(AVFILTER_LIBRARIES NAMES avfilter)
  find_package_handle_standard_args(AVFILTER DEFAULT_MSG AVFILTER_LIBRARIES AVFILTER_INCLUDE_DIRS)

	find_path(AVUTIL_INCLUDE_DIRS NAMES avutil.h PATH_SUFFIXES libavutil)
	find_library(AVUTIL_LIBRARIES NAMES avutil)
	find_package_handle_standard_args(AVUTIL DEFAULT_MSG AVUTIL_LIBRARIES AVUTIL_INCLUDE_DIRS)
	find_path(SWSCALE_INCLUDE_DIRS NAMES swscale.h PATH_SUFFIXES libswscale)
	find_library(SWSCALE_LIBRARIES NAMES swscale)
	find_package_handle_standard_args(SWSCALE DEFAULT_MSG SWSCALE_LIBRARIES SWSCALE_INCLUDE_DIRS)
endif()
