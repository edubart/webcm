################################
# Toolchain stage
FROM --platform=linux/riscv64 riscv64/alpine:3.22.2 AS toolchain-stage

# Update and install development packages
RUN apk update && \
    apk upgrade && \
    apk add build-base pkgconf git wget

# Build other packages inside /root
WORKDIR /root

################################
# Build tools
FROM --platform=linux/riscv64 toolchain-stage AS tools-stage

# Build xhalt (tool used by init system to poweroff the machine)
RUN apk add libseccomp-dev
RUN wget -O xhalt.c https://raw.githubusercontent.com/cartesi/machine-guest-tools/refs/tags/v0.17.2/sys-utils/xhalt/xhalt.c && \
    mkdir -p /pkg/usr/sbin/ && \
    gcc xhalt.c -Os -s -o /pkg/usr/sbin/xhalt && \
    strip /pkg/usr/sbin/xhalt

################################
# Download packages
FROM --platform=linux/riscv64 riscv64/alpine:3.22.2 AS rootfs-stage

# Update packages
RUN apk update && \
    apk upgrade

# Install development utilities
RUN apk add \
    bash bash-completion \
    neovim \
    tree-sitter-lua tree-sitter-c tree-sitter-javascript tree-sitter-python tree-sitter-json tree-sitter-bash \
    tmux \
    htop ncdu vifm \
    strace dmesg \
    lua5.4 \
    quickjs \
    mruby \
    jq \
    bc \
    sqlite \
    micropython \
    tcc tcc-libs tcc-libs-static tcc-dev musl-dev \
    make \
    cmatrix

# Install init system and base skel
ADD --chmod=755 https://raw.githubusercontent.com/cartesi/machine-guest-tools/refs/tags/v0.17.2/sys-utils/cartesi-init/cartesi-init /usr/sbin/cartesi-init
COPY --from=tools-stage /pkg/usr /usr
COPY skel /
RUN ln -sf lua5.4 /usr/bin/lua

# Remove unneeded files
RUN rm -rf /var/cache/apk && rm -f /usr/lib/*.a
