/******************************************************************************
* myClient.c
*
* Modified to:
*   - Use poll() to handle both STDIN and server socket
*   - Use sendPDU() and recvPDU() for all I/O
*   - Print "Connected to Server <host>. The IP is <ip> with Port Number <port> as Client <ID>"
*   - The <ID> is given by argv[3] if present
*
* Original code by Prof. Smith, updated for Sockets lab
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
#include <errno.h>
#include <termios.h>  // for tcflush() on some systems

#include "networks.h"
#include "safeUtil.h"
#include "pollLib.h"
#include "pdu.h"

#define MAXBUF 1024
#define DEBUG_FLAG 0   // We'll handle our own print messages now

// forward declarations
static void checkArgs(int argc, char *argv[]);
static void clientControlLoop(int socketNum);
static void processSocketData(int socketNum);
static void processUserInput(int socketNum);

static int readFromStdin(uint8_t *buffer);

// We'll store the "client ID" from argv[3].
static int myClientID = 0;

/******************************************************************************
 * main()
 *****************************************************************************/
int main(int argc, char * argv[])
{
    int socketNum = 0;

    checkArgs(argc, argv);

    // parse the optional 3rd argument as an integer ID if present
    if (argc == 4)
    {
        myClientID = atoi(argv[3]);
    }

    // set up the TCP Client socket
    // but we want to manually print the "connected" line, so let's turn off debug
    socketNum = tcpClientSetup(argv[1], argv[2], DEBUG_FLAG);

    // Now let's do our own "Connected to Server ::1. The IP is ::1 ..." print
    // We'll do a small piece of code to retrieve the numeric server IP we connected to
    // or we can just re-print the hostname. We have in networks.c, but let's do getaddrinfo ourselves:
    struct sockaddr_in6 sockAddr;
    socklen_t len = sizeof(sockAddr);
    memset(&sockAddr, 0, sizeof(sockAddr));
    if (getpeername(socketNum, (struct sockaddr *)&sockAddr, &len) < 0)
    {
        perror("getpeername");
    }

    char ipString[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &sockAddr.sin6_addr, ipString, sizeof(ipString));
    int port = ntohs(sockAddr.sin6_port);

    // Print the custom line:
    // "Connected to Server <argv[1]>. The IP is ipString with Port Number port as Client myClientID"
    // If myClientID == 0 => user didn't pass a 3rd arg, we can omit or show 0
    printf("Connected to Server %s. The IP is %s with Port Number: %d",
           argv[1], ipString, port);
    if (myClientID > 0)
    {
        printf(" as Client %d\n", myClientID);
    }
    else
    {
        printf("\n");
    }

    // flush leftover input from stdin
    tcflush(STDIN_FILENO, TCIFLUSH);

    // Print the first prompt
    printf("Enter data to send: ");
    fflush(stdout);

    // launch poll-based control loop
    clientControlLoop(socketNum);

    // if we ever break out, close
    close(socketNum);

    return 0;
}

/******************************************************************************
 * clientControlLoop():
 *****************************************************************************/
static void clientControlLoop(int socketNum)
{
    int readyFd = 0;

    setupPollSet();
    addToPollSet(STDIN_FILENO);
    addToPollSet(socketNum);

    while (1)
    {
        readyFd = pollCall(-1);

        if (readyFd < 0)
        {
            continue;
        }
        else if (readyFd == STDIN_FILENO)
        {
            processUserInput(socketNum);
        }
        else if (readyFd == socketNum)
        {
            processSocketData(socketNum);
        }
        else
        {
            fprintf(stderr, "Unexpected FD %d\n", readyFd);
        }
    }
}

/******************************************************************************
 * processUserInput():
 *****************************************************************************/
static void processUserInput(int socketNum)
{
    uint8_t sendBuf[MAXBUF];
    int sendLen = readFromStdin(sendBuf);

    if (sendLen <= 1)
    {
        // user typed empty line => just re-print prompt
        printf("Enter data to send: ");
        fflush(stdout);
        return;
    }

    int sent = sendPDU(socketNum, sendBuf, sendLen);
    if (sent < 0)
    {
        perror("sendPDU call");
        exit(-1);
    }
}

/******************************************************************************
 * processSocketData():
 *****************************************************************************/
static void processSocketData(int socketNum)
{
    uint8_t buffer[MAXBUF];
    int recvLen = recvPDU(socketNum, buffer, MAXBUF);

    if (recvLen < 0)
    {
        fprintf(stderr, "recvPDU error.\n");
        exit(-1);
    }
    else if (recvLen == 0)
    {
        printf("\nServer has terminated\n");
        exit(0);
    }
    else
    {
        printf("\nReceived %d bytes back from server: %s\n", recvLen, buffer);
        printf("Enter data to send: ");
        fflush(stdout);
    }
}

/******************************************************************************
 * readFromStdin():
 *   read until newline or EOF, store in buffer, null-terminate
 *****************************************************************************/
static int readFromStdin(uint8_t *buffer)
{
    int inputLen = 0;
    int c = 0;

    while (inputLen < (MAXBUF - 1))
    {
        c = getchar();
        if (c == '\n' || c == EOF) break;
        buffer[inputLen++] = (uint8_t)c;
    }

    buffer[inputLen] = '\0';
    inputLen++;
    return inputLen;
}

/******************************************************************************
 * checkArgs():
 *   usage: myClient <host> <port> [optional clientID]
 *****************************************************************************/
static void checkArgs(int argc, char * argv[])
{
    if (argc < 3 || argc > 4)
    {
        fprintf(stderr, "usage: %s host-name port-number [clientID]\n", argv[0]);
        exit(1);
    }
}
