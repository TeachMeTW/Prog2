/******************************************************************************
 * server.c
 *
 * Chat server program.
 *
 * Usage: chatServer [optional port-number]
 *
 * This server:
 *  - Uses poll() (via pollLib) to accept new connections and process
 *    data from connected clients.
 *  - Processes client packets:
 *      • Registration (flag=1): check for duplicate handle and add to table.
 *      • Message (flag=5): forward %M messages.
 *      • Broadcast (flag=4): forward %B messages.
 *      • Multicast (flag=6): forward %C messages (and send error packets with flag=7 for each invalid dest).
 *      • List request (flag=10): send a flag=11 packet (with count), then one flag=12 per handle, then flag=13.
 *
 * Client–handle/state information is stored in a separate “handle table” module.
 *
 * Author: Robin Simpson
 * Lab Section: 3pm
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>     // For inet_ntop()
#include <sys/socket.h>
#include <netinet/in.h>
#include "pdu.h"           // Protocol Data Unit functions
#include "networks.h"      // Networking setup and helper functions
#include "pollLib.h"       // Polling functionality for multiple sockets
#include "handleTable.h"   // Data structure for mapping client handles to sockets

#define MAXBUF    1400    // Maximum buffer size for receiving data
#define MAX_HANDLE 100    // Maximum allowed length for a client handle


/*
 * This function returns a string that identifies the client connected on the socket 'sock'.
 * If the client has registered a handle (username), that handle is returned along with the socket number.
 * Otherwise, it returns a string containing the client's IP address and port number along with the socket number.
 * The returned string is stored in a static buffer and should be used immediately.
 */
const char *getClientIdentifier(int sock) {
    static char idStr[256];  // Static buffer to hold the identifier string
    char *handle = lookupHandleBySocket(sock); // Look up the registered handle for this socket

    if (handle != NULL) {
        // If a handle exists, format the identifier with the handle and the socket number
        snprintf(idStr, sizeof(idStr), "%s (socket %d)", handle, sock);
    } else {
        // Otherwise, retrieve the client's IP and port using getpeername
        struct sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        char ipStr[INET6_ADDRSTRLEN] = "(unknown)";
        int port = 0;
        
        // Try to get the peer address for the socket
        if (getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0) {
            if (addr.ss_family == AF_INET6) {
                struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
                inet_ntop(AF_INET6, &s->sin6_addr, ipStr, sizeof(ipStr));
                port = ntohs(s->sin6_port);
            } else if (addr.ss_family == AF_INET) {
                struct sockaddr_in *s = (struct sockaddr_in *)&addr;
                inet_ntop(AF_INET, &s->sin_addr, ipStr, sizeof(ipStr));
                port = ntohs(s->sin_port);
            }
        }
        // Format the identifier using the IP address, port, and socket number
        snprintf(idStr, sizeof(idStr), "%s:%d (socket %d)", ipStr, port, sock);
    }
    return idStr;
}

/*
 * This function fills the provided buffer 'ipBuf' with the client's IP address and sets the integer pointed to by 'port'
 * to the client's port number. It uses the socket 'sock' to determine the peer's address using getpeername.
 */
void getIPAndPort(int sock, char *ipBuf, int bufLen, int *port) {
    struct sockaddr_storage addr;  // Generic socket address storage
    socklen_t addr_len = sizeof(addr);  // Size of the address structure
    
    // Retrieve the address of the peer connected to the socket
    if (getpeername(sock, (struct sockaddr *)&addr, &addr_len) == 0) {
        // Check if the address is IPv6
        if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *s = (struct sockaddr_in6 *)&addr;
            // Convert the IPv6 address to a human-readable format and store it in ipBuf
            inet_ntop(AF_INET6, &s->sin6_addr, ipBuf, bufLen);
            // Convert the port number from network byte order to host byte order
            *port = ntohs(s->sin6_port);
        } else if (addr.ss_family == AF_INET) {
            struct sockaddr_in *s = (struct sockaddr_in *)&addr;
            // Convert the IPv4 address to a human-readable format and store it in ipBuf
            inet_ntop(AF_INET, &s->sin_addr, ipBuf, bufLen);
            // Convert the port number from network byte order to host byte order
            *port = ntohs(s->sin_port);
        }
    } else {
        // If getpeername fails, set ipBuf to "(unknown)" and port to 0
        strncpy(ipBuf, "(unknown)", bufLen);
        *port = 0;
    }
}


/* Function prototypes for processing different packet types */
void processClientSocket(int sock);
void processRegistration(int sock, uint8_t *buffer, int len);
void processBroadcast(int sock, uint8_t *buffer, int len);
void processMessage(int sock, uint8_t *buffer, int len);
void processMulticast(int sock, uint8_t *buffer, int len);
void processListRequest(int sock, uint8_t *buffer, int len);
void sendErrorPacket(int sock, const char *destHandle);

int main(int argc, char *argv[]) {
    int port = 0;  // Default port (0 means that tcpServerSetup() may choose a random available port)

    /* Check if the command line arguments are valid.
       If more than one argument (besides the program name) is provided, display usage and exit. */
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [optional port number]\n", argv[0]);
        exit(1);
    }
    /* If a port number is provided, convert it from string to integer. */
    if (argc == 2)
        port = atoi(argv[1]);

    /* Set up the listening TCP socket. tcpServerSetup() binds and listens on the given port.
       If port==0, the system assigns an ephemeral port. */
    int listenSock = tcpServerSetup(port);

    /* Initialize the poll set and add the listening socket to it.
       The poll set will be used to check for activity on multiple sockets concurrently. */
    setupPollSet();
    addToPollSet(listenSock);

    /* Initialize the handle table that maps client handles (usernames) to their socket descriptors.
       This is used to track client registrations and route messages. */
    initHandleTable();

    /* Main program loop: runs indefinitely, handling incoming connections and client messages */
    while (1) {
        /* pollCall() blocks until there is activity on one of the sockets.
           It returns the socket descriptor that is ready for I/O. */
        int ready = pollCall(-1);

        /* If the ready socket is the listening socket, then a new client is trying to connect */
        if (ready == listenSock) {
            /* Accept the new client connection. The second parameter is a timeout (0 for blocking accept).
               We pass a debug flag of 1 so that tcpAccept() prints out the client's IP and port. */
            int clientSock = tcpAccept(listenSock, 1);
            /* Do not print the accepted connection details here because the client name is not known yet.
               The accepted connection details will be printed after registration. */
            addToPollSet(clientSock);
        } else {
            /* Otherwise, the ready socket belongs to an already-connected client.
               Process the incoming data from that client. */
            processClientSocket(ready);
        }
    }
    return 0;
}

/*
 * processClientSocket:
 *   Reads data from a client socket and processes the packet based on its flag.
 *   If the client disconnects (recvPDU returns <= 0), removes the client from the poll set
 *   and from the handle table.
 */
void processClientSocket(int sock) {
    uint8_t buf[MAXBUF];  // Buffer to hold the incoming data
    int len = recvPDU(sock, buf, MAXBUF);
    if (len <= 0) {
        /* If len is <= 0, either the client has closed the connection or an error occurred.
           Retrieve the client's handle (if registered) for logging purposes,
           then remove the client's information from the handle table and poll set. */
        char *handle = lookupHandleBySocket(sock);
        if (handle != NULL)
            printf("\n[INFO] Client %s disconnected.\n", handle);
        else
            printf("\n[INFO] Client on socket %d disconnected.\n", sock);
        removeHandleBySocket(sock);
        removeFromPollSet(sock);
        close(sock);
        return;
    }

    /* The first byte of the packet is the flag indicating the type of message. */
    uint8_t flag = buf[0];
    switch (flag) {
        case 1:
            /* Registration packet: client wants to register a handle. */
            // printf("[INFO] %s is attempting registration.\n", getClientIdentifier(sock));
            processRegistration(sock, buf, len);
            break;
        case 4:
            /* Broadcast packet: client is sending a message to all other clients. */
            // printf("[INFO] %s is broadcasting a message.\n", getClientIdentifier(sock));
            processBroadcast(sock, buf, len);
            break;
        case 5:
            /* Private message packet: message intended for one recipient. */
            processMessage(sock, buf, len);
            break;
        case 6:
            /* Multicast packet: message intended for multiple recipients. */
            // printf("[INFO] %s is sending a multicast message.\n", getClientIdentifier(sock));
            processMulticast(sock, buf, len);
            break;
        case 10:
            /* List request packet: client is requesting a list of all registered handles. */
            printf("\n[INFO] %s is requesting the client list.\n", getClientIdentifier(sock));
            processListRequest(sock, buf, len);
            break;
        default:
            /* For any unknown flag, the server simply ignores the packet. */
            printf("[WARN] Unknown flag %d from %s. Packet ignored.\n", flag, getClientIdentifier(sock));
            break;
    }
}

/*
 * processRegistration:
 *   Processes a registration packet from a client.
 *   The packet format is: [flag=1][handle_length (1 byte)][handle (string)]
 *
 *   The function checks:
 *     - That the packet length is sufficient.
 *     - That the handle does not exceed the maximum allowed length.
 *     - That the handle is not already registered (i.e., no duplicate).
 *   It sends back an error (flag=3) if the handle is too long or a duplicate.
 *   Otherwise, it adds the handle to the table and sends a confirmation (flag=2).
 */
void processRegistration(int sock, uint8_t *buffer, int len) {
    /* Check that the packet has at least 2 bytes (flag and handle length) */
    if (len < 2) return;
    uint8_t hlen = buffer[1];  // The length of the handle string

    /* Ensure the packet is long enough to contain the entire handle */
    if (len < 2 + hlen) return;
    char handle[MAX_HANDLE+1];  // Buffer to store the handle; +1 for the null terminator

    /* If the handle length exceeds the maximum allowed length, send an error and close the connection */
    if (hlen > MAX_HANDLE) {
        uint8_t resp = 3; // Error code for "handle too long" or duplicate handle error
        sendPDU(sock, &resp, 1);
        printf("[WARN] %s attempted registration with a too-long handle.\n", getClientIdentifier(sock));
        close(sock);
        removeFromPollSet(sock);
        return;
    }
    /* Copy the handle from the packet and ensure it is null-terminated */
    memcpy(handle, buffer + 2, hlen);
    handle[hlen] = '\0';

    /* Check if the handle is already in use by another client */
    if (lookupSocketByHandle(handle) != -1) {
        uint8_t resp = 3; // Duplicate handle error
        sendPDU(sock, &resp, 1);
        printf("[WARN] %s attempted registration with duplicate handle '%s'.\n", getClientIdentifier(sock), handle);
        close(sock);
        removeFromPollSet(sock);
        return;
    }

    /* Add the handle and its corresponding socket to the handle table */
    addHandle(handle, sock);
    {
        uint8_t resp = 2; // Registration accepted response code
        sendPDU(sock, &resp, 1);
    }
    {
        char ipStr[INET6_ADDRSTRLEN];
        int clientPort;
        getIPAndPort(sock, ipStr, sizeof(ipStr), &clientPort);
        printf("Client: %s has joined the chat!\n\n", handle);
    }
}

/*
 * processBroadcast:
 *   Processes a broadcast packet.
 *   Packet format: [flag=4][sender_handle_length (1 byte)][sender handle][text message]
 *
 *   The server forwards this packet to every client except the one who sent it.
 */
void processBroadcast(int sock, uint8_t *buffer, int len) {
    int off = 1;  // Start offset after the flag byte
    /* Check that there is at least one byte for the sender handle length */
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};

    /* Ensure that the packet contains the complete sender handle */
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;

    printf("\n[INFO] Client '%s' (socket %d) is broadcasting a message.\n", sender, sock);

    /* Forward the broadcast packet to each client except the sender */
    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        if (entry->socket != sock)
            sendPDU(entry->socket, buffer, len);
        entry = entry->next;
    }
    /* Extract the text message from the packet (after the sender handle) */
    char *msg = (char *)(buffer + off);
    char ipStr[INET6_ADDRSTRLEN];
    int port;
    getIPAndPort(sock, ipStr, sizeof(ipStr), &port);
    printf("Received packet from %s from socket %d (IP %s, port %d). Message has length %d with data: %s\n",
           sender, sock, ipStr, port, len, msg);
}

/*
 * processMessage:
 *   Processes a private (direct) message packet.
 *   Packet format: [flag=5][sender_handle_length][sender handle]
 *                  [destination_count (should be 1)][dest_handle_length][dest handle][text message]
 *
 *   The server checks that there is exactly one destination. It then looks up the destination's
 *   socket and forwards the message. If the destination handle is not found, an error packet is sent
 *   back to the sender.
 */
void processMessage(int sock, uint8_t *buffer, int len) {
    int off = 1;  // Start offset after the flag byte
    /* Retrieve the sender handle length */
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};

    /* Ensure the packet contains the full sender handle */
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;

    /* Retrieve the number of destination handles (should be exactly 1 for private messages) */
    if (len < off + 1) return;
    uint8_t destCount = buffer[off++];
    if (destCount != 1) return;  // If not exactly one destination, ignore the packet

    /* Retrieve the destination handle */
    if (len < off + 1) return;
    uint8_t dhLen = buffer[off++];
    char destHandle[MAX_HANDLE+1] = {0};
    if (len < off + dhLen) return;
    memcpy(destHandle, buffer + off, dhLen);
    destHandle[dhLen] = '\0';
    off += dhLen;

    printf("\n[INFO] Client '%s' (socket %d) is sending a private message to '%s'.\n", sender, sock, destHandle);

    /* Forward the message if the destination exists, otherwise send an error packet */
    int destSock = lookupSocketByHandle(destHandle);
    if (destSock == -1)
        sendErrorPacket(sock, destHandle);
    else
        sendPDU(destSock, buffer, len);

    /* Extract the text message from the packet (after the destination handle) */
    char *msg = (char *)(buffer + off);
    char ipStr[INET6_ADDRSTRLEN];
    int port;
    getIPAndPort(sock, ipStr, sizeof(ipStr), &port);
    printf("Received packet from %s from socket %d (IP %s, port %d). Message has length %d with data: %s\n",
           sender, sock, ipStr, port, len, msg);
}

/*
 * processMulticast:
 *   Processes a multicast message packet.
 *   Packet format: [flag=6][sender_handle_length][sender handle]
 *                  [number_of_destinations (1 byte)]
 *                  For each destination:
 *                     [dest_handle_length (1 byte)][dest handle]
 *                  [text message]
 *
 *   For each destination, the server attempts to look up the destination handle.
 *   If found, the message is forwarded. If not, an error packet is sent back to the sender.
 *   Note: The text message is sent in its entirety with every forwarded packet.
 */
void processMulticast(int sock, uint8_t *buffer, int len) {
    int off = 1;  // Start offset after the flag byte
    /* Retrieve sender handle length and sender handle */
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;

    /* Get the number of destination handles */
    if (len < off + 1) return;
    uint8_t numDest = buffer[off++];

    printf("\n[INFO] Client '%s' (socket %d) is sending a multicast message to %d destination(s).\n", sender, sock, numDest);

    /* Loop through each destination */
    for (int i = 0; i < numDest; i++) {
        if (len < off + 1) return;
        uint8_t dlen = buffer[off++];
        char destHandle[MAX_HANDLE+1] = {0};
        if (len < off + dlen) return;
        memcpy(destHandle, buffer + off, dlen);
        destHandle[dlen] = '\0';
        off += dlen;

        int destSock = lookupSocketByHandle(destHandle);
        if (destSock == -1) {
            printf("[WARN] Destination '%s' not found for multicast message from '%s'.\n", destHandle, sender);
            sendErrorPacket(sock, destHandle);
        } else {
            sendPDU(destSock, buffer, len);
        }
    }
    /* Extract the text message from the packet (after the sender handle) */
    char *msg = (char *)(buffer + off);
    char ipStr[INET6_ADDRSTRLEN];
    int port;
    getIPAndPort(sock, ipStr, sizeof(ipStr), &port);
    printf("Received packet from %s from socket %d (IP %s, port %d). Message has length %d with data: %s\n",
           sender, sock, ipStr, port, len, msg);
}

/*
 * processListRequest:
 *   Processes a list request packet from a client.
 *   The client sends a packet with flag=10 to request the list of all connected client handles.
 *
 *   The server responds with three parts:
 *     1. A packet with flag=11 containing a 4-byte count (number of registered handles).
 *     2. One packet per handle, each with flag=12 containing:
 *          [handle_length (1 byte)][handle]
 *     3. A final packet with flag=13 to mark the end of the list.
 */
void processListRequest(int sock, uint8_t *buffer, int len) {
    uint32_t count = getHandleCount();
    uint32_t count_net = htonl(count);
    uint8_t resp[1 + 4];
    resp[0] = 11;  // Flag for "list count" packet
    memcpy(resp + 1, &count_net, 4);
    sendPDU(sock, resp, sizeof(resp));

    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        uint8_t pkt[1 + 1 + MAX_HANDLE];  // Buffer for the packet: flag + handle length + handle string
        int off = 0;
        pkt[off++] = 12;  // Flag for "list handle" packet
        uint8_t hlen = (uint8_t) strlen(entry->handle);
        pkt[off++] = hlen;
        memcpy(pkt + off, entry->handle, hlen);
        off += hlen;
        sendPDU(sock, pkt, off);
        entry = entry->next;
    }
    uint8_t finish = 13;
    sendPDU(sock, &finish, 1);
}

/*
 * sendErrorPacket:
 *   Constructs and sends an error packet back to a client when a destination handle is invalid.
 *   Error packet format: [flag=7][dest_handle_length (1 byte)][dest handle]
 */
void sendErrorPacket(int sock, const char *destHandle) {
    uint8_t pkt[1 + 1 + MAX_HANDLE];
    int off = 0;
    pkt[off++] = 7;
    uint8_t hlen = (uint8_t) strlen(destHandle);
    pkt[off++] = hlen;
    memcpy(pkt + off, destHandle, hlen);
    off += hlen;
    sendPDU(sock, pkt, off);
    printf("\n[INFO] Sent error packet to %s: destination handle '%s' not found.\n", getClientIdentifier(sock), destHandle);
}
