
CXXFLAGS ?= -O2 -ggdb
CXXFLAGS += -std=c++11
SERVER_OBJS=supersonic.cc util.cc userdata.cc
CLIENT_OBJS=scanner.cc util.cc

all:	supersonic-server supersonic-scanner


supersonic-scanner:	$(CLIENT_OBJS)
	g++ $(CXXFLAGS) -o supersonic-scanner $(CLIENT_OBJS) -lsqlite3 -ltag -lcrypto -lpthread

supersonic-server:	$(SERVER_OBJS)
	g++ $(CXXFLAGS) -o supersonic-server $(SERVER_OBJS) -lsqlite3 -lfcgi++ -lcrypto -lfcgi -lpthread

clean:
	rm -f supersonic-scanner supersonic-server

