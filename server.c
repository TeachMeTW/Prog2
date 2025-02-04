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
#include "pdu.h"           // Protocol Data Unit functions
#include "networks.h"      // Networking setup and helper functions
#include "pollLib.h"       // Polling functionality for multiple sockets
#include "handleTable.h"   // Data structure for mapping client handles to sockets

#define MAXBUF    1400    // Maximum buffer size for receiving data
#define MAX_HANDLE 100    // Maximum allowed length for a client handle

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
            /* Accept the new client connection. The second parameter is a timeout (0 for blocking accept). */
            int clientSock = tcpAccept(listenSock, 0);
            /* Add the new client socket to the poll set so that we can monitor it for incoming data */
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
            printf("Client %s disconnected.\n", handle);
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
            processRegistration(sock, buf, len);
            break;
        case 4:
            /* Broadcast packet: client is sending a message to all other clients. */
            processBroadcast(sock, buf, len);
            break;
        case 5:
            /* Private message packet: message intended for one recipient. */
            processMessage(sock, buf, len);
            break;
        case 6:
            /* Multicast packet: message intended for multiple recipients. */
            processMulticast(sock, buf, len);
            break;
        case 10:
            /* List request packet: client is requesting a list of all registered handles. */
            processListRequest(sock, buf, len);
            break;
        default:
            /* For any unknown flag, the server simply ignores the packet. */
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
    printf("Client registered: %s\n", handle);
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

    /* Iterate over the handle table and forward the broadcast packet to each client except the sender */
    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        if (entry->socket != sock)
            sendPDU(entry->socket, buffer, len);
        entry = entry->next;
    }
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

    /* Next, retrieve the number of destination handles (should be exactly 1 for private messages) */
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

    /* Look up the destination socket based on the handle */
    int destSock = lookupSocketByHandle(destHandle);
    if (destSock == -1)
        /* If the destination handle is not found, send an error packet back to the sender */
        sendErrorPacket(sock, destHandle);
    else
        /* Otherwise, forward the entire message packet to the destination client */
        sendPDU(destSock, buffer, len);
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

    /* Loop through each destination */
    for (int i = 0; i < numDest; i++) {
        if (len < off + 1) return;
        uint8_t dlen = buffer[off++];
        char destHandle[MAX_HANDLE+1] = {0};
        if (len < off + dlen) return;
        memcpy(destHandle, buffer + off, dlen);
        destHandle[dlen] = '\0';
        off += dlen;

        /* Lookup the destination socket using the handle */
        int destSock = lookupSocketByHandle(destHandle);
        if (destSock == -1)
            /* If the destination does not exist, send an error packet to the sender */
            sendErrorPacket(sock, destHandle);
        else
            /* Otherwise, forward the entire multicast packet to the destination */
            sendPDU(destSock, buffer, len);
    }
    /* The text message follows the destination handles in the packet.
       There is no additional processing needed for the text message here,
       because the entire packet (including the text) was forwarded above. */
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
    /* Get the total number of registered handles */
    uint32_t count = getHandleCount();
    /* Convert the count to network byte order (big endian) */
    uint32_t count_net = htonl(count);
    uint8_t resp[1 + 4];
    resp[0] = 11;  // Flag for "list count" packet
    memcpy(resp + 1, &count_net, 4);
    /* Send the count packet */
    sendPDU(sock, resp, sizeof(resp));

    /* Iterate over all entries in the handle table and send each one in its own packet */
    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        uint8_t pkt[1 + 1 + MAX_HANDLE];  // Buffer for the packet: flag + handle length + handle string
        int off = 0;
        pkt[off++] = 12;  // Flag for "list handle" packet
        uint8_t hlen = (uint8_t) strlen(entry->handle);
        pkt[off++] = hlen;  // Length of the handle
        memcpy(pkt + off, entry->handle, hlen);
        off += hlen;
        /* Send the handle packet */
        sendPDU(sock, pkt, off);
        entry = entry->next;
    }
    /* Finally, send a packet with flag=13 to signal the end of the list */
    uint8_t finish = 13;
    sendPDU(sock, &finish, 1);
}

/*
 * sendErrorPacket:
 *   Constructs and sends an error packet back to a client when a destination handle is invalid.
 *   Error packet format: [flag=7][dest_handle_length (1 byte)][dest handle]
 */
void sendErrorPacket(int sock, const char *destHandle) {
    uint8_t pkt[1 + 1 + MAX_HANDLE];  // Buffer for the error packet
    int off = 0;
    pkt[off++] = 7;  // Flag for "error" packet
    uint8_t hlen = (uint8_t) strlen(destHandle);
    pkt[off++] = hlen;  // Length of the destination handle
    memcpy(pkt + off, destHandle, hlen);
    off += hlen;
    /* Send the error packet to the client */
    sendPDU(sock, pkt, off);
}
