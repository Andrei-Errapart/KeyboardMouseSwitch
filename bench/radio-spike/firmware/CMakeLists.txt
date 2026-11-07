# Radio bench spike firmware for the nRF52840 Dongle (PCA10059).
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(kms_radio_bench)

target_sources(app PRIVATE
  src/main.c
  src/cmd.c
  src/rec.c
  src/stamp.c
  src/ident.c
  src/wdt.c
  src/link.c
)

# FW_VERSION: `git describe` of HEAD, "-dirty" when the firmware tree has
# local changes. Regenerated on every build.
set(FW_VERSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/fw_version)
add_custom_target(fw_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_CURRENT_SOURCE_DIR}
          -DOUT=${FW_VERSION_DIR}/fw_version.h
          -P ${CMAKE_CURRENT_SOURCE_DIR}/fw_version.cmake
  BYPRODUCTS ${FW_VERSION_DIR}/fw_version.h)
add_dependencies(app fw_version)
target_include_directories(app PRIVATE ${FW_VERSION_DIR})
