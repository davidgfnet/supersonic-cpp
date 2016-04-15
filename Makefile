

all:
	g++ -o scanner scanner.cpp -std=c++11 -lsqlite3 -ltag
	g++ -o supersonic supersonic.cpp -std=c++11 -lsqlite3 -lcxxhttpserver -lgnutls -lgcrypt

