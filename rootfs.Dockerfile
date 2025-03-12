################################
# Busybox stage
FROM --platform=linux/riscv64 riscv64/busybox:1.37.0-musl AS busybox-stage

################################
# Rootfs stage
FROM --platform=linux/riscv64 riscv64/alpine:3.21.0 AS toolchain-stage

# Update and install development packages
RUN apk update && \
    apk upgrade && \
    apk add build-base pkgconf git wget

# Build other packages inside /root
WORKDIR /root

################################
# Build xhalt
FROM --platform=linux/riscv64 toolchain-stage AS xhalt-stage
RUN apk add libseccomp-dev
RUN wget -O xhalt.c https://raw.githubusercontent.com/cartesi/machine-emulator-tools/158948a343e792c181a8cee6964cea122c644c52/sys-utils/xhalt/xhalt.c && \
    mkdir -p /pkg/usr/sbin/ && \
    gcc xhalt.c -Os -s -o /pkg/usr/sbin/xhalt && \
    strip /pkg/usr/sbin/xhalt

################################
# Download packages
FROM --platform=linux/riscv64 riscv64/alpine:3.21.0 AS rootfs-stage

# Update packages
RUN echo "@testing https://dl-cdn.alpinelinux.org/alpine/edge/testing" >> /etc/apk/repositories && \
    apk update && \
    apk upgrade

# Install development utilities
RUN apk add \
    bash bash-completion \
    neovim \
    tree-sitter-lua tree-sitter-c tree-sitter-javascript tree-sitter-python tree-sitter-json tree-sitter-bash \
    tmux \
    htop ncdu vifm \
    duf@testing \
    strace dmesg \
    lua5.4 \
    quickjs \
    mruby \
    jq \
    bc \
    sqlite \
    micropython@testing \
    tcc@testing tcc-libs@testing tcc-libs-static@testing tcc-dev@testing musl-dev \
    make \
    cmatrix

# Overwrite busybox
COPY --from=busybox-stage /bin/busybox /bin/busybox
COPY --from=xhalt-stage /pkg/usr /usr

# Install init
ADD --chmod=755 https://raw.githubusercontent.com/cartesi/machine-emulator-tools/refs/heads/main/sys-utils/cartesi-init/cartesi-init /usr/sbin/cartesi-init
COPY skel /
RUN rm -rf /var/cache/apk && \
    rm -f /usr/lib/*.a && \
    ln -sf lua5.4 /usr/bin/lua
