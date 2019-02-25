#CFLAGS = -Wall -std=c11 -fPIC -pthread -DHAVE_SRTP_2=1 -O3 `pkg-config --cflags glib-2.0`
CFLAGS = -Wall -std=c11 -fPIC -pthread -DHAVE_SRTP_2=1 `pkg-config --cflags glib-2.0`
LARGS = -fPIC -shared -pthread
LIBS = -ljansson -lopus -luuid -lsrtp2 -logg
OBJECTS = Audio.o Config.o Lobbies.o Messaging.o Recording.o Sessions.o StreamLobby.o
CC = gcc

build_so: $(OBJECTS) StreamLobby.so

StreamLobby.so : $(OBJECTS)
	$(CC) $(LARGS) $(OBJECTS) `pkg-config --libs glib-2.0` $(LIBS) -o StreamLobby.so

Audio.o : src/Audio.h src/Audio.c
	$(CC) -c $(CFLAGS) src/Audio.c -o Audio.o

Config.o : src/Config.h src/Config.c
	$(CC) -c $(CFLAGS) src/Config.c -o Config.o

Lobbies.o : src/Lobbies.h src/Lobbies.c
	$(CC) -c $(CFLAGS) src/Lobbies.c -o Lobbies.o

Messaging.o : src/Messaging.h src/Messaging.c
	$(CC) -c $(CFLAGS) src/Messaging.c -o Messaging.o

Recording.o : src/Recording.h src/Recording.c
	$(CC) -c $(CFLAGS) src/Recording.c -o Recording.o

Sessions.o : src/Sessions.h src/Sessions.c
	$(CC) -c $(CFLAGS) src/Sessions.c -o Sessions.o

StreamLobby.o : src/StreamLobby.h src/StreamLobby.c
	$(CC) -c $(CFLAGS) src/StreamLobby.c -o StreamLobby.o


debug: CFLAGS += -g -Og -DDEBUG
debug: LARGS += -g -rdynamic
debug: build_so

.PHONY : clean
clean :
	rm $(OBJECTS) StreamLobby.so
