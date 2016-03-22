all: libsoque soque

libsoque:
	g++ -std=c++11 -fPIC -shared -pthread -g -O2 -Wall -Werror -Wno-unused-function ../src/soque.cpp -o /usr/lib/libsoque.so

soque:
	gcc -I../src -g -O2 -Wall -Werror -Wno-unused-function ../src/soque_test.c -lsoque -o /usr/bin/soque
#gcc -I../src -g -O2 -Wall -Werror -Wno-unused-function ../src/soque_test.c -L. -lsoque -o /usr/bin/soque
