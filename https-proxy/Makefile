CXXFLAGS=-std=gnu++23 -Wall -Wextra -Os -fno-rtti -fno-exceptions -DBOOST_NO_EXCEPTIONS -flto -ffunction-sections -fdata-sections -fno-strict-aliasing -fno-strict-overflow
LDFLAGS=-lssl -lcrypto -static-libstdc++ -s -flto -Wl,--gc-sections -Wl,--as-needed

https-proxy: https-proxy.cpp cert_store.cpp *.hpp
	g++ https-proxy.cpp cert_store.cpp -o $@ $(CXXFLAGS) $(LDFLAGS)

lint:
	clang-tidy *.cpp *.hpp -- $(CXXFLAGS)

format:
	clang-format -i *.cpp *.hpp

clean:
	rm -f https-proxy
