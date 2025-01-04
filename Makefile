EMCC_CFLAGS=-Oz -g0 -std=c23 \
   	-L. -I. -lcartesi \
    --js-library=emscripten-pty.js \
    -Wall -Wextra -Wno-unused-function \
    -sASYNCIFY \
    -sFORCE_FILESYSTEM=1 \
   	-sSTACK_SIZE=4MB \
   	-sTOTAL_MEMORY=384MB \
   	-sNO_DISABLE_EXCEPTION_CATCHING
SKEL_FILES=$(shell find skel -type f)

all: linux.bin.zz rootfs.ext2.zz webcm.mjs

test: webcm.mjs
	emrun index.html

shell: rootfs.ext2 linux.bin
	cartesi-machine \
		--ram-image=linux.bin \
		--flash-drive=label:root,filename:rootfs.ext2 \
		--no-init-splash \
		--user=root \
		--network \
		-it "exec ash -l"

webcm.mjs: webcm.c libcartesi.a rootfs.ext2.zz linux.bin.zz emscripten-pty.js
	emcc webcm.c -o webcm.mjs $(EMCC_CFLAGS)

rootfs.ext2: rootfs.Dockerfile $(SKEL_FILES)
	docker build --tag webcm/rootfs --file $< --progress plain .
	docker create --platform=linux/riscv64 --name webcm-copy-rootfs webcm/rootfs
	docker cp webcm-copy-rootfs:/rootfs.ext2 .
	docker rm webcm-copy-rootfs
	touch rootfs.ext2

emscripten-pty.js:
	wget -O emscripten-pty.js https://raw.githubusercontent.com/mame/xterm-pty/refs/tags/v0.10.1/emscripten-pty.js

linux.bin:
	wget -O linux.bin https://github.com/cartesi/image-kernel/releases/download/v0.19.1/linux-6.5.9-ctsi-1-v0.19.1.bin

zz: linux.bin.zz rootfs.ext2.zz

%.zz: %
	cat $< | pigz -cz -11 > $@

clean:
	rm -f webcm.mjs webcm.wasm rootfs.ext2 rootfs.ext2.zz

distclean: clean
	rm -f linux.bin emscripten-pty.js
