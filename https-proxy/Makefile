CXXFLAGS=-std=gnu++23 -Wall -Wextra -Os
HARDEN_CXXFLAGS=-fno-strict-aliasing -fno-strict-overflow -fhardened
LDFLAGS=-lssl -lcrypto -static-libstdc++ -s

https-proxy: https-proxy.cpp cert_store.cpp *.hpp
	g++ https-proxy.cpp cert_store.cpp -o $@ $(CXXFLAGS) $(HARDEN_CXXFLAGS) $(LDFLAGS)

lint:
	clang-tidy *.cpp *.hpp -- $(CXXFLAGS)

format:
	clang-format -i *.cpp *.hpp

clean:
	rm -f https-proxy
