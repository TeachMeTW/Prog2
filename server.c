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
#include "pdu.h"
#include "networks.h"
#include "pollLib.h"
#include "handleTable.h"

#define MAXBUF    1400
#define MAX_HANDLE 100

/* Function prototypes */
void processClientSocket(int sock);
void processRegistration(int sock, uint8_t *buffer, int len);
void processBroadcast(int sock, uint8_t *buffer, int len);
void processMessage(int sock, uint8_t *buffer, int len);
void processMulticast(int sock, uint8_t *buffer, int len);
void processListRequest(int sock, uint8_t *buffer, int len);
void sendErrorPacket(int sock, const char *destHandle);

int main(int argc, char *argv[]) {
    int port = 0;
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [optional port number]\n", argv[0]);
        exit(1);
    }
    if (argc == 2)
        port = atoi(argv[1]);
    int listenSock = tcpServerSetup(port);
    setupPollSet();
    addToPollSet(listenSock);
    initHandleTable();
    while (1) {
        int ready = pollCall(-1);
        if (ready == listenSock) {
            int clientSock = tcpAccept(listenSock, 0);
            addToPollSet(clientSock);
        } else {
            processClientSocket(ready);
        }
    }
    return 0;
}

void processClientSocket(int sock) {
    uint8_t buf[MAXBUF];
    int len = recvPDU(sock, buf, MAXBUF);
    if (len <= 0) {
        /* Client disconnected; remove from handle table (if registered) */
        char *handle = lookupHandleBySocket(sock);
        if (handle != NULL)
            printf("Client %s disconnected.\n", handle);
        removeHandleBySocket(sock);
        removeFromPollSet(sock);
        close(sock);
        return;
    }
    uint8_t flag = buf[0];
    switch (flag) {
        case 1:
            processRegistration(sock, buf, len);
            break;
        case 4:
            processBroadcast(sock, buf, len);
            break;
        case 5:
            processMessage(sock, buf, len);
            break;
        case 6:
            processMulticast(sock, buf, len);
            break;
        case 10:
            processListRequest(sock, buf, len);
            break;
        default:
            /* Unknown flag: ignore */
            break;
    }
}

void processRegistration(int sock, uint8_t *buffer, int len) {
    /* Registration packet: flag=1, then 1-byte handle length, then handle */
    if (len < 2) return;
    uint8_t hlen = buffer[1];
    if (len < 2 + hlen) return;
    char handle[MAX_HANDLE+1];
    if (hlen > MAX_HANDLE) {
        uint8_t resp = 3; // error (handle too long)
        sendPDU(sock, &resp, 1);
        close(sock);
        removeFromPollSet(sock);
        return;
    }
    memcpy(handle, buffer + 2, hlen);
    handle[hlen] = '\0';
    if (lookupSocketByHandle(handle) != -1) {
        uint8_t resp = 3; // duplicate handle error
        sendPDU(sock, &resp, 1);
        close(sock);
        removeFromPollSet(sock);
        return;
    }
    addHandle(handle, sock);
    {
        uint8_t resp = 2; // registration accepted
        sendPDU(sock, &resp, 1);
    }
    printf("Client registered: %s\n", handle);
}

void processBroadcast(int sock, uint8_t *buffer, int len) {
    /* Broadcast packet: flag=4, then 1-byte sender handle length, then sender handle, then text */
    int off = 1;
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;
    /* Forward to all clients except the sender */
    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        if (entry->socket != sock)
            sendPDU(entry->socket, buffer, len);
        entry = entry->next;
    }
}

void processMessage(int sock, uint8_t *buffer, int len) {
    /* %M message packet: flag=5, then sender handle, then (1 dest) then destination handle, then text */
    int off = 1;
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;
    if (len < off + 1) return;
    uint8_t destCount = buffer[off++];
    if (destCount != 1) return;
    if (len < off + 1) return;
    uint8_t dhLen = buffer[off++];
    char destHandle[MAX_HANDLE+1] = {0};
    if (len < off + dhLen) return;
    memcpy(destHandle, buffer + off, dhLen);
    destHandle[dhLen] = '\0';
    off += dhLen;
    /* Look up destination and forward packet */
    int destSock = lookupSocketByHandle(destHandle);
    if (destSock == -1)
        sendErrorPacket(sock, destHandle);
    else
        sendPDU(destSock, buffer, len);
}

void processMulticast(int sock, uint8_t *buffer, int len) {
    /* Multicast packet: flag=6, then sender handle, then 1-byte number of dest,
     * then for each destination: [1-byte length, dest handle],
     * then text message.
     */
    int off = 1;
    if (len < off + 1) return;
    uint8_t shLen = buffer[off++];
    char sender[MAX_HANDLE+1] = {0};
    if (len < off + shLen) return;
    memcpy(sender, buffer + off, shLen);
    sender[shLen] = '\0';
    off += shLen;
    if (len < off + 1) return;
    uint8_t numDest = buffer[off++];
    for (int i = 0; i < numDest; i++) {
        if (len < off + 1) return;
        uint8_t dlen = buffer[off++];
        char destHandle[MAX_HANDLE+1] = {0};
        if (len < off + dlen) return;
        memcpy(destHandle, buffer + off, dlen);
        destHandle[dlen] = '\0';
        off += dlen;
        int destSock = lookupSocketByHandle(destHandle);
        if (destSock == -1)
            sendErrorPacket(sock, destHandle);
        else
            sendPDU(destSock, buffer, len);
    }
    /* (Text message follows; no additional processing needed here.) */
}

void processListRequest(int sock, uint8_t *buffer, int len) {
    /* List request: flag=10. Reply with:
     *  - A packet with flag=11 and a 4-byte count.
     *  - Then one flag=12 packet per handle.
     *  - Finally, a flag=13 packet.
     */
    uint32_t count = getHandleCount();
    uint32_t count_net = htonl(count);
    uint8_t resp[1 + 4];
    resp[0] = 11;
    memcpy(resp + 1, &count_net, 4);
    sendPDU(sock, resp, sizeof(resp));
    struct ClientEntry *entry = getHandleTableHead();
    while (entry) {
        uint8_t pkt[1 + 1 + MAX_HANDLE];
        int off = 0;
        pkt[off++] = 12;
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

void sendErrorPacket(int sock, const char *destHandle) {
    /* Build error packet: flag=7, then 1-byte handle length, then destHandle */
    uint8_t pkt[1 + 1 + MAX_HANDLE];
    int off = 0;
    pkt[off++] = 7;
    uint8_t hlen = (uint8_t) strlen(destHandle);
    pkt[off++] = hlen;
    memcpy(pkt + off, destHandle, hlen);
    off += hlen;
    sendPDU(sock, pkt, off);
}
