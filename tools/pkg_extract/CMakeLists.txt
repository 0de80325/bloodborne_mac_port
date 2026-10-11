# Host tool (native arm64 is fine): unpacks fake-signed PS4 packages.
cmake_minimum_required(VERSION 3.25)
project(pkg_extract CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ZLIB REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CRYPTOPP REQUIRED IMPORTED_TARGET libcryptopp)
add_executable(pkg_extract src/main.cpp src/core/file_format/pkg.cpp
    src/core/file_format/pkg_type.cpp src/core/crypto/crypto.cpp)
target_include_directories(pkg_extract PRIVATE src ${CMAKE_CURRENT_SOURCE_DIR}/../../gpu/shadps4)
target_link_libraries(pkg_extract PRIVATE ZLIB::ZLIB PkgConfig::CRYPTOPP)
