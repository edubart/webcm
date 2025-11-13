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
GUEST_SKEL_FILES=$(shell find skel -type f)
GUEST_SRC_FILES=$(shell find https-proxy -type f -name '*.cpp' -o -name '*.hpp' -o -name Makefile)
DOCKER_HOST_TAG=webcm/builder
DOCKER_HOST_RUN_FLAGS=
DOCKER_HOST_RUN=docker run --platform=linux/amd64 --volume=.:/mnt --workdir=/mnt --user=$(shell id -u):$(shell id -g) --env=HOME=/tmp $(DOCKER_HOST_RUN_FLAGS) --rm $(DOCKER_HOST_TAG)

all: rootfs.ext2 webcm.mjs ## Build everything

.webcm-builder: builder.Dockerfile ## Build WASM cross compiler docker image
ifneq ($(IS_WASM_TOOLCHAIN),true)
	docker build --platform=linux/amd64 --tag $(DOCKER_HOST_TAG) --file $< --progress plain .
	touch $@
endif

webcm.wasm webcm.mjs: DOCKER_HOST_RUN_FLAGS=--env=EM_CACHE=/mnt/.cache --env=PIGZ_LEVEL=$(PIGZ_LEVEL)
webcm.wasm webcm.mjs: webcm.cpp rootfs.ext2.zz linux.bin.zz emscripten-pty.js .cache
ifeq ($(IS_WASM_TOOLCHAIN),true)
	em++ webcm.cpp -o webcm.mjs $(EMCC_CFLAGS)
else
	$(DOCKER_HOST_RUN) make webcm.mjs
endif

gh-pages: index.html webcm.mjs webcm.wasm favicon.svg ## Build github pages directory
	mkdir -p $@
	cp $^ $@/

rootfs.ext2: rootfs.tar .webcm-builder ## Build rootfs.ext2
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	$(DOCKER_HOST_RUN) xgenext2fs \
	    --faketime \
	    --allow-holes \
	    --size-in-blocks 98304 \
	    --block-size 4096 \
	    --bytes-per-inode 4096 \
	    --volume-label rootfs \
	    --tarball $< $@
endif

rootfs.tar: rootfs.Dockerfile .buildx-cache $(GUEST_SKEL_FILES) $(GUEST_SRC_FILES) ## Build rootfs.tar
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	docker buildx build --platform=linux/riscv64 --progress plain --cache-from type=local,src=.buildx-cache --output type=tar,dest=$@ --file rootfs.Dockerfile .
endif

emscripten-pty.js: .webcm-builder ## Download emscripten-pty.js dependency
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	$(DOCKER_HOST_RUN) wget -O emscripten-pty.js https://raw.githubusercontent.com/mame/xterm-pty/f284cab414d3e20f27a2f9298540b559878558db/emscripten-pty.js
	touch $@
endif

linux.bin: .webcm-builder ## Download linux.bin dependency
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	$(DOCKER_HOST_RUN) wget -O linux.bin https://github.com/cartesi/machine-linux-image/releases/download/v0.20.0/linux-6.5.13-ctsi-1-v0.20.0.bin
	touch $@
endif

%.zz: % .webcm-builder
ifeq ($(IS_WASM_TOOLCHAIN),true)
	@test -f $@ || (echo "Error: $@ not found. This should be built on the host." && exit 1)
else
	$(DOCKER_HOST_RUN) sh -c "cat $< | pigz -cz -$(PIGZ_LEVEL) > $@"
endif

.buildx-cache .cache: ## Create cache directories
	mkdir -p $@

clean: ## Remove built files
	rm -rf webcm.mjs webcm.wasm rootfs.tar rootfs.ext2 rootfs.ext2.zz linux.bin.zz

distclean: clean ## Remove built files, downloaded files and cached files
	rm -rf linux.bin emscripten-pty.js .cache .buildx-cache .webcm-builder

shell: DOCKER_HOST_RUN_FLAGS=-it
shell: rootfs.ext2 linux.bin ## Spawn a cartesi machine shell using the built rootfs
	$(DOCKER_HOST_RUN) cartesi-machine \
		--ram-image=/mnt/linux.bin \
		--flash-drive=label:root,filename:/mnt/rootfs.ext2 \
		--no-init-splash \
		--network \
		--user=root \
		-it "exec ash -l"

serve: webcm.mjs webcm.wasm ## Serve a web server
	python -m http.server 8080

help: ## Show this help
	@sed \
		-e '/^[a-zA-Z0-9_\-]*:.*##/!d' \
		-e 's/:.*##\s*/:/' \
		-e 's/^\(.\+\):\(.*\)/$(shell tput setaf 6)\1$(shell tput sgr0):\2/' \
		$(MAKEFILE_LIST) | column -c2 -t -s :
