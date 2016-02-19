

all:
	g++ -o scanner scanner.cpp -std=c++11 -lsqlite3 -lid3tag -lcrypto
	g++ -o supersonic supersonic.cpp -std=c++11 -lsqlite3 -lcxxhttpserver

