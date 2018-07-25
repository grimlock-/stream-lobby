CFLAGS = -Wall -std=c11 -fPIC -pthread -DHAVE_SRTP_2=1 -O3
LARGS = -fPIC -shared -pthread
LIBS = -ljansson -lopus -luuid -lsrtp2 -logg

build_so: streamLobby.o streamLobby.so

streamLobby.so : streamLobby.o
	gcc $(LARGS) streamLobby.o `pkg-config --libs glib-2.0` $(LIBS) -o streamLobby.so

streamLobby.o : src/streamLobby.c
	gcc -c $(CFLAGS) `pkg-config --cflags glib-2.0` src/streamLobby.c

debug: CFLAGS += -g -Og -DDEBUG
debug: LARGS += -g -rdynamic
debug: build_so

.PHONY : clean
clean :
	rm streamLobby.o streamLobby.so
