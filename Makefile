#
# Makefile for a Poll-based Client/Server using PDU
# Produces two executables: cclient and server
#

CC = gcc
CFLAGS = -g -Wall -std=gnu99
LIBS =

OBJS = networks.o gethostbyname.o pollLib.o safeUtil.o pdu.o

all: cclient server

# Build the client
cclient: cclient.c $(OBJS)
	$(CC) $(CFLAGS) -o cclient cclient.c $(OBJS) $(LIBS)

# Build the server
server: server.c $(OBJS)
	$(CC) $(CFLAGS) -o server server.c $(OBJS) $(LIBS)

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

# utility targets
clean:
	rm -f *.o cclient server

cleano:
	rm -f *.o
