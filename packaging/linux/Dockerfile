# Portable Linux x86_64 build (Steam Deck / desktop). Ubuntu 22.04's glibc keeps the binary
# runnable on newer distributions, SteamOS included. SDL3 is built from source and linked
# statically; its X11/Wayland/audio back ends are loaded at run time.
#   docker build --platform linux/amd64 -f packaging/linux/Dockerfile -t laserlords-linux .
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git ca-certificates pkg-config \
    libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxss-dev libxfixes-dev libxtst-dev \
    libxkbcommon-dev libwayland-dev wayland-protocols libdecor-0-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
    libgl1-mesa-dev libegl1-mesa-dev libgles2-mesa-dev libdrm-dev libgbm-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
RUN cmake -S . -B /build -DCMAKE_BUILD_TYPE=Release -DLL_BUNDLED_SDL=ON \
    && cmake --build /build -j"$(nproc)"
