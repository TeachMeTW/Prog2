/******************************************************************************
* myServer.c
*
* Modified to:
*   - Loop indefinitely until ^C
*   - Use poll() to handle multiple clients
*   - Assign each new client an incrementing ID: Client 1, Client 2, ...
*   - On receiving data, print:
*         "Message received on socket X from client Y, length: Z, Data: ..."
*   - Use recvPDU() and sendPDU() for all I/O
*
* Original code by Prof. Smith, updated here for Lab Sockets Programming
*
*****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdint.h>

#include "networks.h"
#include "safeUtil.h"
#include "pollLib.h"
#include "pdu.h"

#define MAXBUF 1024
#define DEBUG_FLAG 1

// forward declarations
int checkArgs(int argc, char *argv[]);
static void serverControl(int mainServerSocket);
static void addNewSocket(int mainServerSocket);
static void processClient(int clientSocket);

// We'll keep track of each client's "ID" in this map.
//   clientIDMap[socketFD] = the unique ID we assigned to that socket.
static int clientIDMap[1024];

// We'll increment this for each new connection: Client 1, Client 2, etc.
static int nextClientID = 1;

int main(int argc, char *argv[])
{
    int mainServerSocket = 0;
    int portNumber = 0;

    portNumber = checkArgs(argc, argv);

    // create the server socket
    mainServerSocket = tcpServerSetup(portNumber);
    printf("Server listening forever... ^C to quit\n");

    // run a poll-based loop to handle multiple clients
    serverControl(mainServerSocket);

    // never gets here unless you break out
    close(mainServerSocket);
    return 0;
}

/*
 * serverControl():
 *   - sets up poll with the mainServerSocket
 *   - in an infinite loop:
 *       pollCall()
 *       if ready FD == mainServerSocket => accept new client => addToPollSet
 *       else => processClient(ready FD)
 */
static void serverControl(int mainServerSocket)
{
    int readyFd = 0;

    setupPollSet();
    addToPollSet(mainServerSocket);

    while (1)
    {
        readyFd = pollCall(-1);
        if (readyFd < 0)
        {
            // ignoring poll errors
            continue;
        }

        if (readyFd == mainServerSocket)
        {
            addNewSocket(mainServerSocket);
        }
        else
        {
            // existing client has data
            processClient(readyFd);
        }
    }
}

/*
 * addNewSocket():
 *   - accept a new client
 *   - assign them a unique ID
 *   - print "Client N accepted. IP/Port..."
 *   - add to poll set
 */
static void addNewSocket(int mainServerSocket)
{
    struct sockaddr_in6 clientAddress;
    socklen_t addrLen = sizeof(clientAddress);

    // do accept() yourself, so we can gather IP info
    int clientSocket = accept(mainServerSocket, (struct sockaddr*)&clientAddress, &addrLen);
    if (clientSocket < 0)
    {
        perror("accept");
        return;
    }

    // figure out the IP/port in text form
    char ipString[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &clientAddress.sin6_addr, ipString, sizeof(ipString));
    int clientPort = ntohs(clientAddress.sin6_port);

    // assign an ID to this new client
    clientIDMap[clientSocket] = nextClientID;

    // print "Client <ID> accepted..."
    printf("Client %d accepted.  Client IP: %s Client Port Number: %d\n",
           nextClientID, ipString, clientPort);

    // increment for next client
    nextClientID++;

    // now add it to the poll set
    addToPollSet(clientSocket);
}

/*
 * processClient():
 *   - uses recvPDU() to read data
 *   - if ret=0 => client closed => removeFromPollSet, close()
 *   - else => print "Message received on socket X from client Y, length..., Data..."
 *   - echo it back
 */
static void processClient(int clientSocket)
{
    uint8_t dataBuffer[MAXBUF];
    int recvLen = recvPDU(clientSocket, dataBuffer, MAXBUF);

    if (recvLen < 0)
    {
        fprintf(stderr, "Error on recvPDU for socket %d\n", clientSocket);
        removeFromPollSet(clientSocket);
        close(clientSocket);
        return;
    }
    else if (recvLen == 0)
    {
        // client closed 
        printf("Client %d closed connection (socket %d)\n", clientIDMap[clientSocket], clientSocket);
        removeFromPollSet(clientSocket);
        close(clientSocket);
        return;
    }

    // get that client's ID
    int clientID = clientIDMap[clientSocket];

    // we got a message => print
    // Example: "Message received on socket 5 from client 2, length: 4, Data: dab"
    printf("Message received on socket %d from client %d, length: %d, Data: %s\n",
           clientSocket, clientID, recvLen, dataBuffer);

    // echo it back
    int sent = sendPDU(clientSocket, dataBuffer, recvLen);
    if (sent < 0)
    {
        fprintf(stderr, "Error on sendPDU to client %d\n", clientID);
        removeFromPollSet(clientSocket);
        close(clientSocket);
    }
}

int checkArgs(int argc, char *argv[])
{
    // single optional argument => port number
    int portNumber = 0;

    if (argc > 2)
    {
        fprintf(stderr, "Usage: %s [optional port number]\n", argv[0]);
        exit(-1);
    }

    if (argc == 2)
    {
        portNumber = atoi(argv[1]);
    }

    return portNumber;
}
