all: libsoque.so soque_test

libsoque.so:
	g++ -Wall -Wl,--no-as-needed -pthread -std=c++11 -fPIC -shared -g -O2 -Werror -Wno-unused-function ../src/soque.cpp -o libsoque.so

soque_test:
	gcc -I../src -g -O2 -Wall -Werror -Wno-unused-function ../examples/soque_test.c -o soque_test -ldl

install: libsoque.so soque_test
	install -D libsoque.so /usr/lib/libsoque.so
	install -D soque_test /usr/bin/soque_test

cleanup:
	if test -e libsoque.so; then unlink libsoque.so; fi
	if test -e soque_test; then unlink soque_test; fi
	if test -e /usr/lib/libsoque.so; then unlink /usr/lib/libsoque.so; fi
	if test -e /usr/bin/soque_test; then unlink /usr/bin/soque_test; fi
