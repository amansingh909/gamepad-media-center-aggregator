# Desktop Linux GMCA build with the torrent engine (USE_TORRENT=ON), for building and smoke
# running the app on a laptop with no GMCA toolchain: Xvfb, Mesa and a D-Bus session let
# it open a window with no display. GLFW is fetched by borealis at configure time.
#   docker build -t gmca-desktop-torrent -f tests/torrent/desktop.Dockerfile tests/torrent
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
 && apt-get install -y --no-install-recommends build-essential cmake ninja-build git pkg-config ca-certificates \
    xorg-dev libdbus-1-dev libssl-dev libwayland-dev libxkbcommon-dev libwebp-dev libmpv-dev \
    libcurl4-openssl-dev libtorrent-rasterbar-dev libboost-dev \
    xvfb xauth libgl1-mesa-dri libglx-mesa0 dbus dbus-x11 curl procps \
 && rm -rf /var/lib/apt/lists/*
