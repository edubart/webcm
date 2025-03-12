EMCC_CFLAGS=-Oz -g0 -std=gnu23 \
	-I/opt/emscripten-cartesi-machine/include \
	-L/opt/emscripten-cartesi-machine/lib \
   	-lcartesi \
    --js-library=emscripten-pty.js \
    -Wall -Wextra -Wno-unused-function \
    -sASYNCIFY \
   	-sSTACK_SIZE=4MB \
   	-sTOTAL_MEMORY=384MB
SKEL_FILES=$(shell find skel -type f)

all: builder rootfs.tar # Compile everything inside a Docker environment
	docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --rm -it webcm/builder make webcm.mjs

test: # Test
	emrun index.html

builder: builder.Dockerfile
	docker build --tag webcm/builder --file $< --progress plain .

webcm.mjs: webcm.c rootfs.ext2.zz linux.bin.zz emscripten-pty.js
	emcc webcm.c -o webcm.mjs $(EMCC_CFLAGS)

rootfs.ext2: rootfs.tar
	xgenext2fs \
	    --faketime \
	    --allow-holes \
	    --size-in-blocks 32768 \
	    --block-size 4096 \
	    --bytes-per-inode 4096 \
	    --volume-label rootfs \
	    --tarball $< $@

rootfs.tar: rootfs.Dockerfile $(SKEL_FILES)
	docker buildx build --progress plain --output type=tar,dest=$@ --file rootfs.Dockerfile .

emscripten-pty.js:
	wget -O emscripten-pty.js https://raw.githubusercontent.com/mame/xterm-pty/refs/tags/v0.10.1/emscripten-pty.js

linux.bin: ## Download linux.bin
	wget -O linux.bin https://github.com/cartesi/machine-linux-image/releases/download/v0.20.0/linux-6.5.13-ctsi-1-v0.20.0.bin

%.zz: %
	cat $< | pigz -cz -11 > $@

clean: ## Remove built files
	rm -f webcm.mjs webcm.wasm rootfs.tar rootfs.ext2 rootfs.ext2.zz linux.bin.zz

distclean: clean ## Remove built files and downloaded files
	rm -f linux.bin emscripten-pty.js

shell: rootfs.ext2 linux.bin # For debugging
	cartesi-machine \
		--ram-image=linux.bin \
		--flash-drive=label:root,filename:rootfs.ext2 \
		--no-init-splash \
		--user=root \
		--network \
		-it "exec ash -l"

help: ## Show this help
	@sed \
		-e '/^[a-zA-Z0-9_\-]*:.*##/!d' \
		-e 's/:.*##\s*/:/' \
		-e 's/^\(.\+\):\(.*\)/$(shell tput setaf 6)\1$(shell tput sgr0):\2/' \
		$(MAKEFILE_LIST) | column -c2 -t -s :
