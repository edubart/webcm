################################
# Toolchain stage
FROM alpine:3.22.2@sha256:4b7ce07002c69e8f3d704a9c5d6fd3053be500b7f1c69fc0d80990c2ad8dd412 AS alpine-base

# Update system
RUN apk update && \
    apk upgrade -aU

################################
# Toolchain stage
FROM alpine-base AS toolchain-stage

# Update and install development packages
RUN apk add build-base pkgconf git wget patchelf

# Build other packages inside /root
WORKDIR /root

################################
# Build tools

# Build xhalt (tool used by init system to poweroff the machine)
FROM toolchain-stage AS xhalt-stage
RUN apk add libseccomp-dev
RUN wget -O xhalt.c https://raw.githubusercontent.com/cartesi/machine-guest-tools/refs/tags/v0.17.2/sys-utils/xhalt/xhalt.c
RUN gcc xhalt.c -Os -s -o xhalt
RUN mkdir -p /pkg/usr/sbin && \
    cp xhalt /pkg/usr/sbin/ && \
    strip /pkg/usr/sbin/xhalt

# Build https-proxy (proxy used to provide networking in the browser)
FROM toolchain-stage AS proxy-stage
RUN apk add boost-dev openssl-dev
COPY https-proxy https-proxy
RUN make -C https-proxy
RUN mkdir -p /pkg/usr/sbin /pkg/etc/ssl/webcm /pkg/etc/ssl/certs /pkg/usr/local/share/ca-certificates && \
    cp https-proxy/https-proxy /pkg/usr/sbin/https-proxy && \
    strip /pkg/usr/sbin/https-proxy

# Build gcompat (tool to run GLIBC programs)
FROM toolchain-stage AS gcompat-stage
RUN git clone --revision=d8cf7fbd072a379b9b16991539ba03bbbab4bd9c --depth=1 https://git.adelielinux.org/adelie/gcompat.git
RUN <<EOF cat >> gcompat/gcompat.patch
diff --git a/libgcompat/resolv.c b/libgcompat/resolv.c
index c63d074..35fe616 100644
--- a/libgcompat/resolv.c
+++ b/libgcompat/resolv.c
@@ -13,6 +13,10 @@

 #include "alias.h" /* weak_alias */

+int __res_init() {
+	return res_init();
+}
+
 int __res_ninit(res_state statp)
 {
 	int rc;

EOF
RUN cd gcompat && git apply gcompat.patch
RUN make -C gcompat -j$(nproc) LINKER_PATH=/lib/ld-musl-riscv64.so.1 LOADER_NAME=ld-linux-riscv64-lp64d.so.1
RUN make -C gcompat install LINKER_PATH=/lib/ld-musl-riscv64.so.1 LOADER_NAME=ld-linux-riscv64-lp64d.so.1 DESTDIR=/pkg

################################
# Download packages
FROM alpine-base AS rootfs-stage

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
    git \
    cmatrix \
    curl wget \
    darkhttpd \
    libatomic

# Remove unneeded files to shrink image size
RUN rm -rf /var/cache/apk /usr/lib/libc.a

# Install init system and base skel
ADD --chmod=755 https://raw.githubusercontent.com/cartesi/machine-guest-tools/refs/tags/v0.17.2/sys-utils/cartesi-init/cartesi-init /usr/sbin/cartesi-init
COPY --from=xhalt-stage /pkg /
COPY --from=proxy-stage /pkg /
COPY --from=gcompat-stage /pkg /
COPY skel /
RUN ln -sf lua5.4 /usr/bin/lua
