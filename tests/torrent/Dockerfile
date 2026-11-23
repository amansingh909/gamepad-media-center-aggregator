# Linux environment for the torrent engine harness (see CMakeLists.txt here).
FROM ubuntu:24.04
RUN apt-get update \
 && apt-get install -y --no-install-recommends g++ cmake make libtorrent-rasterbar-dev libboost-dev ca-certificates \
 && rm -rf /var/lib/apt/lists/*
