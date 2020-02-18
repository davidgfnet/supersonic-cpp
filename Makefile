
CXXFLAGS ?= -O2 -ggdb
CXXFLAGS += -std=c++11
SERVER_OBJS=supersonic.cc util.cc
CLIENT_OBJS=scanner.cc

all:	supersonic-server supersonic-scanner


supersonic-scanner:	$(CLIENT_OBJS)
	g++ $(CXXFLAGS) -o supersonic-scanner $(CLIENT_OBJS) -lsqlite3 -ltag -lcrypto -lpthread

supersonic-server:	$(SERVER_OBJS)
	g++ $(CXXFLAGS) -o supersonic-server $(SERVER_OBJS) -lsqlite3 -lfcgi++ -lfcgi -lpthread

clean:
	rm -f supersonic-scanner supersonic-server

