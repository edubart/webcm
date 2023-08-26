all: cartesi-term.js

cartesi-term.js: *.nelua
	nelua cartesi-term.nelua --cc=emcc -r -o docs/cartesi-term.js

test: cartesi-term.js
	emrun docs/index.html

clean:
	rm -f docs/cartesi-term.js docs/cartesi-term.wasm
