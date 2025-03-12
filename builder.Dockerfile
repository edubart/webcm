FROM archlinux:base-devel

RUN pacman -Syyu --noconfirm && \
    pacman -S --noconfirm git wget vim emscripten lua libslirp pigz

# Build xgenext2fs
RUN <<EOF
set -e
wget -O genext2fs-1.5.6.tar.gz https://github.com/cartesi/genext2fs/archive/refs/tags/v1.5.6.tar.gz
tar xzvf genext2fs-1.5.6.tar.gz
cd genext2fs-1.5.6
./autogen.sh
./configure --prefix=/usr
make -j$(nproc)
make install
EOF

# Download cartesi machine
RUN <<EOF
set -e
git clone --branch v0.19.0-test2 --depth 1 https://github.com/cartesi/machine-emulator.git
cd machine-emulator
wget https://github.com/cartesi/machine-emulator/releases/download/v0.19.0-test2/add-generated-files.diff
patch -Np1 < add-generated-files.diff
make bundle-boost
EOF

# Build cartesi-machine and install in the system
RUN <<EOF
set -e
cd machine-emulator
make -j$(nproc)
make install PREFIX=/usr
EOF

# Build libcartesi.a for WebAssembly
RUN <<EOF
set -e
mkdir -p /root/.cache/emscripten
export PATH=$PATH:/usr/lib/emscripten
cd machine-emulator
make -C src clean
make -C src -j$(nproc) libcartesi.a \
    SO_EXT=wasm \
    CC=emcc \
    CXX=em++ \
    AR="emar rcs" \
    LUA_LIB= LUA_INC= \
    OPTFLAGS="-O3 -g0" \
    slirp=no
make install-headers install-static-libs \
    STRIP=emstrip \
    EMU_TO_LIB_A="src/libcartesi.a" \
    PREFIX=/opt/emscripten-cartesi-machine
EOF

ENV PATH="$PATH:/usr/lib/emscripten"
