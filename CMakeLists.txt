# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(pantum-open C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(PANTUM_BUILD_BACKEND "Build the SANE scanner backend" ON)
option(PANTUM_BUILD_FILTER "Build the CUPS print filter" ON)

find_package(PkgConfig REQUIRED)

# Default to lib/ rather than letting GNUInstallDirs guess lib64: SANE backends
# and CUPS filters are looked up under <prefix>/lib on the distributions we
# target.
if(NOT CMAKE_INSTALL_LIBDIR)
  set(CMAKE_INSTALL_LIBDIR lib)
endif()
include(GNUInstallDirs)

# ------------------------------------------------------------------ backend

if(PANTUM_BUILD_BACKEND)
  pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

  # Only sane.h and saneopts.h are needed; the backend is a dlopen plugin for
  # SANE's "dll" meta backend and must not link against libsane itself.
  find_path(SANE_INCLUDE_DIR sane/sane.h)
  if(NOT SANE_INCLUDE_DIR)
    message(FATAL_ERROR "sane/sane.h not found - install the SANE headers")
  endif()

  set(PANTUM_CONFIG_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/sane.d"
      CACHE STRING "Directory searched for pantum.conf at runtime")

  add_library(sane-pantum SHARED src/pantum.c)

  # The dll loader dlopens "libsane-<name>.so.<major>", so the file name and
  # soversion together have to spell libsane-pantum.so.1.
  set_target_properties(sane-pantum PROPERTIES
    OUTPUT_NAME sane-pantum
    PREFIX "lib"
    SOVERSION 1)

  target_include_directories(sane-pantum PRIVATE
    ${SANE_INCLUDE_DIR}
    "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(sane-pantum PRIVATE
    _DEFAULT_SOURCE
    PANTUM_CONFIG_DIR="${PANTUM_CONFIG_DIR}")
  target_compile_options(sane-pantum PRIVATE -Wall -Wextra)
  target_link_libraries(sane-pantum PRIVATE PkgConfig::LIBUSB)

  # pow()/floor() build the tone curve; on glibc they live in libm.
  find_library(M_LIBRARY m)
  if(M_LIBRARY)
    target_link_libraries(sane-pantum PRIVATE ${M_LIBRARY})
  endif()

  install(TARGETS sane-pantum LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/sane")
  install(FILES etc/sane.d/pantum.conf
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/sane.d")
  install(FILES etc/sane.d/dll.d/pantum
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/sane.d/dll.d")
  # Claims the scanner for SANE, the way sane-backends does for the devices it
  # supports: without it the raw USB node stays root-only.
  install(FILES etc/udev/rules.d/60-pantum-open.rules
          DESTINATION "${CMAKE_INSTALL_SYSCONFDIR}/udev/rules.d")
endif()

# ------------------------------------------------------------------- filter

if(PANTUM_BUILD_FILTER)
  pkg_check_modules(CUPS REQUIRED IMPORTED_TARGET cups)

  # cupsRasterOpen() lives in libcupsimage on CUPS 1.x and in libcups from
  # CUPS 2.2 onwards; link the extra library only where it exists.
  find_library(CUPSIMAGE_LIBRARY NAMES cupsimage HINTS ${CUPS_LIBRARY_DIRS})

  # jbig-kit ships no pkg-config file, so look the pieces up by hand.
  find_path(JBIG_INCLUDE_DIR jbig.h)
  find_library(JBIG_LIBRARY NAMES jbig)
  if(NOT JBIG_INCLUDE_DIR OR NOT JBIG_LIBRARY)
    message(FATAL_ERROR "jbig-kit not found - install jbigkit (libjbig + jbig.h)")
  endif()

  add_executable(rastertopantum src/rastertopantum.c)
  target_include_directories(rastertopantum PRIVATE ${JBIG_INCLUDE_DIR})
  target_compile_definitions(rastertopantum PRIVATE _DEFAULT_SOURCE)
  target_compile_options(rastertopantum PRIVATE -Wall -Wextra)
  target_link_libraries(rastertopantum PRIVATE PkgConfig::CUPS ${JBIG_LIBRARY})
  if(CUPSIMAGE_LIBRARY)
    target_link_libraries(rastertopantum PRIVATE ${CUPSIMAGE_LIBRARY})
  endif()

  # cupsd looks for filters under <serverbin>/filter and for bundled PPDs
  # under <datadir>/cups/model.
  set(CUPS_FILTER_DIR "${CMAKE_INSTALL_LIBDIR}/cups/filter"
      CACHE STRING "Where the CUPS filter is installed")
  set(CUPS_MODEL_DIR "${CMAKE_INSTALL_DATAROOTDIR}/cups/model"
      CACHE STRING "Where the PPD is installed")

  install(TARGETS rastertopantum RUNTIME DESTINATION "${CUPS_FILTER_DIR}")
  install(FILES ppd/Pantum-M6500-open.ppd DESTINATION "${CUPS_MODEL_DIR}")
endif()
