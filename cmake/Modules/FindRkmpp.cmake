find_package(PkgConfig)

if (PKG_CONFIG_FOUND)
  pkg_check_modules(RKMPP rockchip_mpp)
endif()
