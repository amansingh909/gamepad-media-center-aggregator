# Laptop harness for the torrent engine and server, outside GMCA's app build:
#   docker build -t gmca-torrent-harness tests/torrent
#   docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src gmca-torrent-harness \
#     bash -c 'cmake -S tests/torrent -B build-torrent && cmake --build build-torrent -j$(nproc)'
cmake_minimum_required(VERSION 3.16)
project(torrent_harness CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(LibtorrentRasterbar CONFIG REQUIRED)
find_package(Threads REQUIRED)

set(GMCA_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
file(GLOB TORRENT_SRC CONFIGURE_DEPENDS ${GMCA_ROOT}/app/src/torrent/*.cpp)

add_executable(torrent_serve torrent_serve.cpp ${TORRENT_SRC})
target_include_directories(torrent_serve PRIVATE ${GMCA_ROOT}/app/include)
target_compile_options(torrent_serve PRIVATE -Wall)
target_link_libraries(torrent_serve PRIVATE LibtorrentRasterbar::torrent-rasterbar Threads::Threads)
