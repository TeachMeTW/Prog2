/******************************************************************************
 * cclient.c
 *
 * Chat client program.
 * Usage: chatClient <handle> <server-name> <server-port> [optional clientID]
 *
 * This client:
 *  - Connects to the server.
 *  - Immediately sends an initial registration packet (flag=1)
 *    containing the client’s handle.
 *  - Then reads STDIN commands (%M, %B, %C, %L) and builds packets per the spec.
 *  - It also processes incoming packets (forwarded messages, error packets, list responses).
 *
 * Author: Robin Simpson
 * Lab Section: 3pm
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include "pdu.h"
#include "networks.h"
#include "pollLib.h"
#include <termios.h>

#define MAXBUF    1400
#define MAX_HANDLE 100

// Global variables
char clientHandle[MAX_HANDLE+1] = {0};
int myClientID = 0; // Optional client ID

// Forward declarations
static void checkArgs(int argc, char *argv[]);
static void sendRegistration(int socketNum);
static void processRegistrationResponse(uint8_t *buffer, int len);
static void clientControlLoop(int socketNum);
static void processUserInput(int socketNum);
static void processSocketData(int socketNum);
static void handleCommand(const char *input, int socketNum);

static void checkArgs(int argc, char *argv[]) {
    if (argc < 4 || argc > 5) {
        fprintf(stderr, "usage: %s <handle> <server-name> <server-port> [clientID]\n", argv[0]);
        exit(1);
    }
    if (strlen(argv[1]) > MAX_HANDLE) {
        fprintf(stderr, "Invalid handle, handle longer than 100 characters: %s\n", argv[1]);
        exit(1);
    }
}

static void sendRegistration(int socketNum) {
    /* Build registration packet:
     * Payload: [flag=1] [1-byte handle length] [handle (no null)]
     */
    uint8_t buf[1 + 1 + MAX_HANDLE];
    int offset = 0;
    buf[offset++] = 1; // flag = 1 (registration)
    uint8_t hlen = (uint8_t) strlen(clientHandle);
    buf[offset++] = hlen;
    memcpy(buf + offset, clientHandle, hlen);
    offset += hlen;
    sendPDU(socketNum, buf, offset);
}

static void processRegistrationResponse(uint8_t *buffer, int len) {
    if (len < 1) return;
    uint8_t flag = buffer[0];
    if (flag == 2) {
        // Registration accepted.
        return;
    } else if (flag == 3) {
        fprintf(stderr, "Handle already in use: %s\n", clientHandle);
        exit(1);
    } else {
        fprintf(stderr, "Unknown registration response flag: %d\n", flag);
        exit(1);
    }
}

static void processUserInput(int socketNum) {
    char inputBuffer[MAXBUF];
    if (fgets(inputBuffer, MAXBUF, stdin) == NULL) {
        exit(0);
    }
    /* Remove trailing newline */
    size_t len = strlen(inputBuffer);
    if (len > 0 && inputBuffer[len - 1] == '\n') {
        inputBuffer[len - 1] = '\0';
    }
    if (strlen(inputBuffer) == 0) {
        printf("Enter command: ");
        fflush(stdout);
        return;
    }
    handleCommand(inputBuffer, socketNum);
}

static void handleCommand(const char *input, int socketNum) {
    /* All valid commands begin with '%' */
    if (input[0] != '%') {
        printf("Invalid command\n");
        printf("Enter command: ");
        fflush(stdout);
        return;
    }
    char cmd = toupper(input[1]);
    if (cmd == 'M') {
        /* %M destination-handle [text] */
        char *copy = strdup(input);
        char *token = strtok(copy, " "); // "%M"
        token = strtok(NULL, " ");         // destination handle
        if (!token) {
            printf("Invalid command format\n");
            free(copy);
            printf("Enter command: ");
            fflush(stdout);
            return;
        }
        char destHandle[MAX_HANDLE+1];
        strncpy(destHandle, token, MAX_HANDLE);
        destHandle[MAX_HANDLE] = '\0';
        char *text = strtok(NULL, "\n");   // rest of the line (may be empty)
        if (!text) text = "";
        /* Build packet:
         * [flag=5] [1-byte sender handle length] [sender handle]
         * [1-byte destination count = 1] [1-byte dest handle length] [dest handle]
         * [null-terminated text message]
         */
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 5; // flag = 5 (message)
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;
        buf[offset++] = 1; // exactly one destination
        uint8_t dhLen = (uint8_t) strlen(destHandle);
        buf[offset++] = dhLen;
        memcpy(buf + offset, destHandle, dhLen);
        offset += dhLen;
        int textLen = strlen(text) + 1; // include null terminator
        memcpy(buf + offset, text, textLen);
        offset += textLen;
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'B') {
        /* %B [text] broadcast */
        char *copy = strdup(input);
        strtok(copy, " "); // skip "%B"
        char *text = strtok(NULL, "\n");
        if (!text) text = "";
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 4; // flag = 4 (broadcast)
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;
        int textLen = strlen(text) + 1;
        memcpy(buf + offset, text, textLen);
        offset += textLen;
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'C') {
        /* %C num-handles dest1 dest2 ... [text] multicast */
        char *copy = strdup(input);
        char *token = strtok(copy, " "); // "%C"
        token = strtok(NULL, " ");         // number of handles
        if (!token) {
            printf("Invalid command format\n");
            free(copy);
            printf("Enter command: ");
            fflush(stdout);
            return;
        }
        int numHandles = atoi(token);
        if (numHandles < 2 || numHandles > 9) {
            printf("Invalid number of handles for multicast\n");
            free(copy);
            printf("Enter command: ");
            fflush(stdout);
            return;
        }
        char destHandles[10][MAX_HANDLE+1];
        for (int i = 0; i < numHandles; i++) {
            token = strtok(NULL, " ");
            if (!token) {
                printf("Invalid command format\n");
                free(copy);
                printf("Enter command: ");
                fflush(stdout);
                return;
            }
            strncpy(destHandles[i], token, MAX_HANDLE);
            destHandles[i][MAX_HANDLE] = '\0';
        }
        char *text = strtok(NULL, "\n");
        if (!text) text = "";
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 6; // flag = 6 (multicast)
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;
        buf[offset++] = (uint8_t) numHandles;
        for (int i = 0; i < numHandles; i++) {
            uint8_t dhLen = (uint8_t) strlen(destHandles[i]);
            buf[offset++] = dhLen;
            memcpy(buf + offset, destHandles[i], dhLen);
            offset += dhLen;
        }
        int textLen = strlen(text) + 1;
        memcpy(buf + offset, text, textLen);
        offset += textLen;
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'L') {
        /* %L: List handles request */
        uint8_t buf[1];
        buf[0] = 10; // flag = 10 (list request)
        sendPDU(socketNum, buf, 1);
    }
    else {
        printf("Invalid command\n");
    }
    printf("Enter command: ");
    fflush(stdout);
}

static void processSocketData(int socketNum) {
    uint8_t buf[MAXBUF];
    int len = recvPDU(socketNum, buf, MAXBUF);
    if (len <= 0) {
        printf("\nServer Terminated\n");
        exit(0);
    }
    uint8_t flag = buf[0];
    if (flag == 7) {
        /* Error packet: flag=7, then 1-byte handle length, then handle */
        if (len < 2) return;
        uint8_t hlen = buf[1];
        char missingHandle[MAX_HANDLE+1] = {0};
        if (len >= 2 + hlen)
            memcpy(missingHandle, buf + 2, hlen);
        missingHandle[hlen] = '\0';
        printf("\nClient with handle %s does not exist.\n", missingHandle);
    }
    else if (flag == 4) {
        /* Broadcast: flag=4, then sender handle length, sender handle, then text */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 5) {
        /* %M message: flag=5, then sender handle, then a 1-byte dest count, then dest handle, then text */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        off++; // skip destination count (should be 1)
        uint8_t dhLen = buf[off++];
        off += dhLen; // skip dest handle
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 6) {
        /* Multicast: flag=6, then sender handle, then 1-byte num dest, then list of dest handles, then text */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        uint8_t num = buf[off++];
        for (int i = 0; i < num; i++) {
            uint8_t dlen = buf[off++];
            off += dlen;
        }
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 11) {
        /* List response: flag=11, then 4-byte count; then expect count flag=12 packets, then a flag=13 packet */
        if (len < 5) return;
        uint32_t count_net;
        memcpy(&count_net, buf + 1, 4);
        uint32_t count = ntohl(count_net);
        printf("\nNumber of clients: %u\n", count);
        for (uint32_t i = 0; i < count; i++) {
            uint8_t pkt[MAXBUF];
            int r = recvPDU(socketNum, pkt, MAXBUF);
            if (r <= 0) break;
            if (pkt[0] != 12) continue;
            int off = 1;
            uint8_t hlen = pkt[off++];
            char handle[MAX_HANDLE+1] = {0};
            memcpy(handle, pkt + off, hlen);
            handle[hlen] = '\0';
            printf("%s\n", handle);
        }
        /* Finally, read the flag=13 termination packet */
        uint8_t term;
        recvPDU(socketNum, &term, 1);
    }
    /* Re-print prompt */
    printf("Enter command: ");
    fflush(stdout);
}

static void clientControlLoop(int socketNum) {
    setupPollSet();
    addToPollSet(STDIN_FILENO);
    addToPollSet(socketNum);
    while (1) {
        int ready = pollCall(-1);
        if (ready == STDIN_FILENO)
            processUserInput(socketNum);
        else if (ready == socketNum)
            processSocketData(socketNum);
        else {
            fprintf(stderr, "Unexpected FD %d\n", ready);
        }
    }
}

int main(int argc, char *argv[]) {
    /* Usage: chatClient <handle> <server-name> <server-port> [clientID] */
    checkArgs(argc, argv);
    strncpy(clientHandle, argv[1], MAX_HANDLE);
    clientHandle[MAX_HANDLE] = '\0';
    int socketNum = tcpClientSetup(argv[2], argv[3], 0);
    if (argc == 5)
        myClientID = atoi(argv[4]);

    /* Optionally print a connected message */
    printf("Connected to Server %s on Port %s as Client %s", argv[2], argv[3], clientHandle);
    if (myClientID > 0)
        printf(" (ID %d)", myClientID);
    printf("\n");

    /* Flush STDIN and send registration packet */
    tcflush(STDIN_FILENO, TCIFLUSH);
    sendRegistration(socketNum);
    uint8_t regResp[100];
    int respLen = recvPDU(socketNum, regResp, 100);
    if (respLen <= 0) {
        fprintf(stderr, "No response from server during registration.\n");
        exit(1);
    }
    processRegistrationResponse(regResp, respLen);

    printf("Enter command: ");
    fflush(stdout);
    clientControlLoop(socketNum);
    close(socketNum);
    return 0;
}
