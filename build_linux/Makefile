all: libsoque.so soque

libsoque.so:
	g++ -std=c++11 -fPIC -shared -lpthread -g -O2 -Wall -Werror -Wno-unused-function ../src/soque.cpp -o libsoque.so

soque:
	gcc -I../src -g -O2 -Wall -Werror -Wno-unused-function ../examples/soque_test.c -ldl -o soque

install: libsoque.so soque
	cp libsoque.so /usr/lib/libsoque.so
	cp soque /usr/bin/soque

cleanup:
	if test -e libsoque.so; then unlink libsoque.so; fi
	if test -e soque; then unlink soque; fi
	if test -e /usr/lib/libsoque.so; then unlink /usr/lib/libsoque.so; fi
	if test -e /usr/bin/soque; then unlink /usr/bin/soque; fi
