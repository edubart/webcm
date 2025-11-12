EMCC_CFLAGS=-Oz -g0 -std=gnu++23 \
	-I/opt/emscripten-cartesi-machine/include \
	-L/opt/emscripten-cartesi-machine/lib \
   	-lcartesi \
    --js-library=emscripten-pty.js \
    -Wall -Wextra -Wno-unused-function -Wno-c23-extensions \
    -sASYNCIFY \
    -sFETCH \
   	-sSTACK_SIZE=4MB \
   	-sTOTAL_MEMORY=768MB \
   	-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,UTF8ToString,stringToUTF8
PIGZ_LEVEL ?= 11
SKEL_FILES=$(shell find skel -type f)
SRC_FILES=$(shell find https-proxy -type f -name '*.cpp' -o -name '*.hpp' -o -name Makefile)

all: builder rootfs.ext2 webcm.mjs

test: # Test
	emrun index.html

builder: builder.Dockerfile ## Build WASM cross compiler docker image
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@echo "Skipping builder target (already inside builder container)"
else
	docker build --tag webcm/builder --file $< --progress plain .
endif

webcm.wasm webcm.mjs: webcm.cpp rootfs.ext2.zz linux.bin.zz emscripten-pty.js
ifeq ($(IS_WASM_TOOLCHAIN),true)
	em++ webcm.cpp -o webcm.mjs $(EMCC_CFLAGS)
else
	@mkdir -p .cache
	docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --env=EM_CACHE=/mnt/.cache --env=PIGZ_LEVEL=$(PIGZ_LEVEL) --rm -it webcm/builder make webcm.mjs
endif

gh-pages: index.html webcm.mjs webcm.wasm webcm.mjs favicon.svg
	mkdir -p $@
	cp $^ $@/

rootfs.ext2: rootfs.tar builder
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --rm webcm/builder \
	    xgenext2fs \
	    --faketime \
	    --allow-holes \
	    --size-in-blocks 98304 \
	    --block-size 4096 \
	    --bytes-per-inode 4096 \
	    --volume-label rootfs \
	    --tarball $< $@
endif

rootfs.tar: rootfs.Dockerfile $(SKEL_FILES) $(SRC_FILES)
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	@mkdir -p .buildx-cache
	docker buildx build --progress plain --cache-from type=local,src=.buildx-cache --output type=tar,dest=$@ --file rootfs.Dockerfile .
endif

emscripten-pty.js: builder
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	@if [ ! -f $@ ]; then \
		docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --rm webcm/builder \
		    wget -O emscripten-pty.js https://raw.githubusercontent.com/mame/xterm-pty/f284cab414d3e20f27a2f9298540b559878558db/emscripten-pty.js; \
	else \
		echo "$@ already exists, skipping download"; \
	fi
endif

linux.bin: builder ## Download linux.bin
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	@if [ ! -f $@ ]; then \
		docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --rm webcm/builder \
		    wget -O linux.bin https://github.com/cartesi/machine-linux-image/releases/download/v0.20.0/linux-6.5.13-ctsi-1-v0.20.0.bin; \
	else \
		echo "$@ already exists, skipping download"; \
	fi
endif

%.zz: % builder
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	docker run --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp --rm webcm/builder \
	    sh -c "cat $< | pigz -cz -$(PIGZ_LEVEL) > $@"
endif

clean: ## Remove built files
	rm -f webcm.mjs webcm.wasm rootfs.tar rootfs.ext2 rootfs.ext2.zz linux.bin.zz

distclean: clean ## Remove built files and downloaded files
	rm -f linux.bin emscripten-pty.js

shell: rootfs.ext2 linux.bin # For debugging
	/usr/bin/cartesi-machine \
		--ram-image=linux.bin \
		--flash-drive=label:root,filename:rootfs.ext2 \
		--no-init-splash \
		--network \
		--user=root \
		-it "exec ash -l"

serve: ## Serve a web server
	python -m http.server 8080

help: ## Show this help
	@sed \
		-e '/^[a-zA-Z0-9_\-]*:.*##/!d' \
		-e 's/:.*##\s*/:/' \
		-e 's/^\(.\+\):\(.*\)/$(shell tput setaf 6)\1$(shell tput sgr0):\2/' \
		$(MAKEFILE_LIST) | column -c2 -t -s :
