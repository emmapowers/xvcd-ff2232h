cmake_minimum_required(VERSION 3.14)
project(xvcd-ft2232h LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FTDI REQUIRED IMPORTED_TARGET libftdi1)
pkg_check_modules(USB REQUIRED IMPORTED_TARGET libusb-1.0)

add_executable(xvcd-ft2232h
    src/xvcServer.c
    src/ftdi_xvc_core.c
)

target_include_directories(xvcd-ft2232h PRIVATE src)

target_link_libraries(xvcd-ft2232h PRIVATE
    PkgConfig::FTDI
    PkgConfig::USB
)
