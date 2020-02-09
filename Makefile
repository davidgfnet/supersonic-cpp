

all:
	g++ -ggdb -O2 -o scanner scanner.cpp -std=c++11 -lsqlite3 -ltag -lcrypto -lpthread
	g++ -ggdb -O2 -o supersonic supersonic.cpp util.cc -std=c++11 -lsqlite3 -lgcrypt -lfcgi++ -lfcgi -lpthread

