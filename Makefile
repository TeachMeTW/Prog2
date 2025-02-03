#
# Makefile for a Poll-based Client/Server using PDU
# Produces two executables: cclient and server
#

CC = gcc
CFLAGS = -g -Wall -std=gnu99
LIBS =

# Common object files used by both client and server
COMMON_OBJS = networks.o gethostbyname.o pollLib.o safeUtil.o pdu.o

# Additional object file(s) for the server
SERVER_OBJS = handleTable.o

all: cclient server

# Build the client executable
cclient: cclient.c $(COMMON_OBJS)
	$(CC) $(CFLAGS) -o cclient cclient.c $(COMMON_OBJS) $(LIBS)

# Build the server executable
server: server.c $(COMMON_OBJS) $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o server server.c $(COMMON_OBJS) $(SERVER_OBJS) $(LIBS)

# Compile object files
networks.o: networks.c networks.h gethostbyname.h
	$(CC) $(CFLAGS) -c networks.c

gethostbyname.o: gethostbyname.c gethostbyname.h
	$(CC) $(CFLAGS) -c gethostbyname.c

pollLib.o: pollLib.c pollLib.h safeUtil.h
	$(CC) $(CFLAGS) -c pollLib.c

safeUtil.o: safeUtil.c safeUtil.h
	$(CC) $(CFLAGS) -c safeUtil.c

pdu.o: pdu.c pdu.h safeUtil.h
	$(CC) $(CFLAGS) -c pdu.c

handleTable.o: handleTable.c handleTable.h
	$(CC) $(CFLAGS) -c handleTable.c

# Utility targets
clean:
	rm -f *.o cclient server

cleano:
	rm -f *.o
