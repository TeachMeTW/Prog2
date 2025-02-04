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

#include <stdio.h>          // Standard I/O functions
#include <stdlib.h>         // Standard library functions (exit, atoi, etc.)
#include <string.h>         // String handling (strlen, memcpy, etc.)
#include <ctype.h>          // Character handling (toupper)
#include <unistd.h>         // POSIX API functions (close, read, etc.)
#include "pdu.h"            // Functions to build and send Protocol Data Units (PDUs)
#include "networks.h"       // Networking helper functions (client/server setup)
#include "pollLib.h"        // Polling functions for monitoring multiple file descriptors
#include <termios.h>        // Terminal control functions (tcflush)

/* Define maximum buffer size for reading/writing and maximum handle length */
#define MAXBUF    1400     // Maximum size for message buffer
#define MAX_HANDLE 100     // Maximum allowed length for a client handle

/* Global variables */
/* clientHandle stores the username/handle of this client; extra byte for the null terminator */
char clientHandle[MAX_HANDLE+1] = {0};
/* Optional client ID; if provided on the command line, stored here */
int myClientID = 0;

/* Forward declarations of functions used only in this file */
static void checkArgs(int argc, char *argv[]);
static void sendRegistration(int socketNum);
static void processRegistrationResponse(uint8_t *buffer, int len);
static void clientControlLoop(int socketNum);
static void processUserInput(int socketNum);
static void processSocketData(int socketNum);
static void handleCommand(const char *input, int socketNum);

/*
 * checkArgs:
 *   Validates the command-line arguments.
 *   Ensures the usage is: chatClient <handle> <server-name> <server-port> [clientID]
 *   Also checks that the provided handle does not exceed MAX_HANDLE characters.
 */
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

/*
 * sendRegistration:
 *   Constructs and sends a registration packet to the server.
 *   Packet format:
 *     [flag=1] [1-byte handle length] [handle (without null terminator)]
 */
static void sendRegistration(int socketNum) {
    uint8_t buf[1 + 1 + MAX_HANDLE]; // Buffer for registration packet: flag + length + handle
    int offset = 0;                  // Offset to keep track of the current buffer position

    buf[offset++] = 1;               // Registration flag: flag = 1

    // Get the length of the client handle and add it to the packet
    uint8_t hlen = (uint8_t) strlen(clientHandle);
    buf[offset++] = hlen;

    // Copy the client handle into the buffer (without the null terminator)
    memcpy(buf + offset, clientHandle, hlen);
    offset += hlen;

    // Send the registration packet to the server
    sendPDU(socketNum, buf, offset);
}

/*
 * processRegistrationResponse:
 *   Processes the server's response to the registration packet.
 *   The server's response must contain at least one byte indicating the response flag:
 *     - flag == 2: Registration accepted.
 *     - flag == 3: Registration error (e.g., duplicate handle).
 *   Any unknown flag results in an error message and client exit.
 */
static void processRegistrationResponse(uint8_t *buffer, int len) {
    if (len < 1) return;             // Ensure there's at least one byte in the response
    uint8_t flag = buffer[0];        // Extract the flag from the response
    if (flag == 2) {
        // Registration accepted; nothing further is needed.
        return;
    } else if (flag == 3) {
        // Registration error: the handle is already in use.
        fprintf(stderr, "Handle already in use: %s\n", clientHandle);
        exit(1);
    } else {
        // Unknown response flag received; exit the client.
        fprintf(stderr, "Unknown registration response flag: %d\n", flag);
        exit(1);
    }
}

/*
 * processUserInput:
 *   Reads user input from STDIN and processes it.
 *   - Reads a line using fgets.
 *   - Removes any trailing newline.
 *   - If the input is non-empty, it is handed off to handleCommand().
 */
static void processUserInput(int socketNum) {
    char inputBuffer[MAXBUF];
    if (fgets(inputBuffer, MAXBUF, stdin) == NULL) {
        // If reading input fails (e.g., EOF), exit gracefully.
        exit(0);
    }
    /* Remove the trailing newline character, if present */
    size_t len = strlen(inputBuffer);
    if (len > 0 && inputBuffer[len - 1] == '\n') {
        inputBuffer[len - 1] = '\0';
    }
    /* If the input is empty, simply reprint the prompt */
    if (strlen(inputBuffer) == 0) {
        printf("$: ");
        fflush(stdout);
        return;
    }
    /* Process the entered command */
    handleCommand(inputBuffer, socketNum);
}

/*
 * handleCommand:
 *   Parses and processes commands entered by the user.
 *   Valid commands must start with '%' and are followed by a command character:
 *
 *   %M - Private message: Format is "%M destination-handle [text]"
 *        Packet format:
 *           [flag=5] [1-byte sender handle length] [sender handle]
 *           [1-byte destination count (1)] [1-byte destination handle length] [destination handle]
 *           [null-terminated text message]
 *
 *   %B - Broadcast message: Format is "%B [text]"
 *        Packet format:
 *           [flag=4] [1-byte sender handle length] [sender handle]
 *           [null-terminated text message]
 *
 *   %C - Multicast message: Format is "%C num-handles dest1 dest2 ... [text]"
 *        Packet format:
 *           [flag=6] [1-byte sender handle length] [sender handle]
 *           [1-byte number of destination handles]
 *           For each destination:
 *             [1-byte destination handle length] [destination handle]
 *           [null-terminated text message]
 *
 *   %L - List request: Format is simply "%L"
 *        Packet format:
 *           [flag=10]
 *
 *   If the command is not recognized, an error message is printed.
 */
static void handleCommand(const char *input, int socketNum) {
    /* All valid commands must begin with the '%' character */
    if (input[0] != '%') {
        printf("Invalid command\n");
        printf("$: ");
        fflush(stdout);
        return;
    }
    // Convert the command letter to uppercase to handle both lower- and uppercase input
    char cmd = toupper(input[1]);

    if (cmd == 'M') {
        /* Private message command: %M destination-handle [text] */
        char *copy = strdup(input);           // Duplicate the input to tokenize safely
        char *token = strtok(copy, " ");        // Token 1: "%M"
        token = strtok(NULL, " ");              // Token 2: destination handle
        if (!token) {
            printf("Invalid command format\n");
            free(copy);
            printf("$: ");
            fflush(stdout);
            return;
        }
        char destHandle[MAX_HANDLE+1];
        strncpy(destHandle, token, MAX_HANDLE);
        destHandle[MAX_HANDLE] = '\0';
        // The rest of the input line is considered the text message.
        char *text = strtok(NULL, "\n");
        if (!text) text = "";
        
        /* Build the private message packet:
         *   [flag=5] [1-byte sender handle length] [sender handle]
         *   [1-byte destination count (should be 1)] [1-byte destination handle length] [destination handle]
         *   [null-terminated text message]
         */
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 5; // Private message flag

        // Append sender handle
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;

        // Specify that there is exactly one destination
        buf[offset++] = 1;

        // Append destination handle
        uint8_t dhLen = (uint8_t) strlen(destHandle);
        buf[offset++] = dhLen;
        memcpy(buf + offset, destHandle, dhLen);
        offset += dhLen;

        // Append the text message, including the null terminator
        int textLen = strlen(text) + 1;
        memcpy(buf + offset, text, textLen);
        offset += textLen;

        // Send the private message packet to the server
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'B') {
        /* Broadcast command: %B [text]
         * Build a broadcast message packet.
         * Packet format:
         *   [flag=4] [1-byte sender handle length] [sender handle]
         *   [null-terminated text message]
         */
        char *copy = strdup(input);
        strtok(copy, " ");              // Skip the command token "%B"
        char *text = strtok(NULL, "\n");  // Get the text message (if any)
        if (!text) text = "";
        
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 4; // Broadcast flag

        // Append sender handle
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;

        // Append the text message with its null terminator
        int textLen = strlen(text) + 1;
        memcpy(buf + offset, text, textLen);
        offset += textLen;

        // Send the broadcast packet to the server
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'C') {
        /* Multicast command: %C num-handles dest1 dest2 ... [text]
         * Build a multicast message packet.
         * Packet format:
         *   [flag=6] [1-byte sender handle length] [sender handle]
         *   [1-byte number of destination handles]
         *   For each destination:
         *     [1-byte destination handle length] [destination handle]
         *   [null-terminated text message]
         */
        char *copy = strdup(input);
        char *token = strtok(copy, " ");  // Token 1: "%C"
        token = strtok(NULL, " ");        // Token 2: number of handles
        if (!token) {
            printf("Invalid command format\n");
            free(copy);
            printf("$: ");
            fflush(stdout);
            return;
        }
        int numHandles = atoi(token);
        // Check that the number of handles is between 2 and 9 (inclusive)
        if (numHandles < 2 || numHandles > 9) {
            printf("Invalid number of handles for multicast\n");
            free(copy);
            printf("$: ");
            fflush(stdout);
            return;
        }
        // Array to store each destination handle (up to 10 handles)
        char destHandles[10][MAX_HANDLE+1];
        for (int i = 0; i < numHandles; i++) {
            token = strtok(NULL, " ");
            if (!token) {
                printf("Invalid command format\n");
                free(copy);
                printf("$: ");
                fflush(stdout);
                return;
            }
            strncpy(destHandles[i], token, MAX_HANDLE);
            destHandles[i][MAX_HANDLE] = '\0';
        }
        // The rest of the input (if any) is the multicast message text.
        char *text = strtok(NULL, "\n");
        if (!text) text = "";
        
        uint8_t buf[MAXBUF];
        int offset = 0;
        buf[offset++] = 6; // Multicast flag

        // Append sender handle
        uint8_t shLen = (uint8_t) strlen(clientHandle);
        buf[offset++] = shLen;
        memcpy(buf + offset, clientHandle, shLen);
        offset += shLen;

        // Append the number of destination handles
        buf[offset++] = (uint8_t) numHandles;

        // Append each destination handle (with its length)
        for (int i = 0; i < numHandles; i++) {
            uint8_t dhLen = (uint8_t) strlen(destHandles[i]);
            buf[offset++] = dhLen;
            memcpy(buf + offset, destHandles[i], dhLen);
            offset += dhLen;
        }

        // Append the multicast text message including the null terminator
        int textLen = strlen(text) + 1;
        memcpy(buf + offset, text, textLen);
        offset += textLen;

        // Send the multicast packet to the server
        sendPDU(socketNum, buf, offset);
        free(copy);
    }
    else if (cmd == 'L') {
        /* List request command: %L
         * Build a simple list request packet.
         * Packet format:
         *   [flag=10]
         */
        uint8_t buf[1];
        buf[0] = 10; // List request flag
        sendPDU(socketNum, buf, 1);
    }
    else if (cmd == 'H') {
        /* Help command: %h
         * Print a help message listing all available commands and a brief description of each.
         */
        printf("\nAvailable Commands:\n");
        printf("  %%M <dest_handle> <text>\n");
        printf("       Send a private message to <dest_handle> with the specified <text>.\n");
        printf("  %%B <text>\n");
        printf("       Broadcast <text> to all connected clients.\n");
        printf("  %%C <num> <dest1> <dest2> ... <destN> <text>\n");
        printf("       Send a multicast message to the specified <num> destination handles.\n");
        printf("  %%L\n");
        printf("       Request a list of all connected client handles.\n");
        printf("  %%h\n");
        printf("       Display this help message.\n");
        printf("\n");
    }
    else {
        // If the command is not recognized, inform the user.
        printf("Invalid command\n");
    }
    // Reprint the prompt after processing the command.
    printf("$: ");
    fflush(stdout);
}

/*
 * processSocketData:
 *   Handles data received from the server.
 *   It reads an incoming packet using recvPDU() and processes it based on its flag.
 *
 *   Supported packet types include:
 *     - Error packet (flag=7): Notifies that a destination handle was not found.
 *     - Broadcast (flag=4): Contains a broadcast message from another client.
 *     - Private message (flag=5): Contains a direct message from another client.
 *     - Multicast (flag=6): Contains a multicast message.
 *     - List response (flag=11): Contains the number of clients and then a series
 *         of packets (flag=12 for each handle) followed by a termination packet (flag=13).
 */
static void processSocketData(int socketNum) {
    uint8_t buf[MAXBUF];             // Buffer to hold the incoming packet
    int len = recvPDU(socketNum, buf, MAXBUF);
    if (len <= 0) {
        // If no data is received, assume the server has terminated the connection.
        printf("\nServer Terminated\n");
        exit(0);
    }
    uint8_t flag = buf[0];           // The first byte indicates the packet type

    if (flag == 7) {
        /* Error packet: Format is [flag=7] [1-byte handle length] [handle]
         * Indicates that a destination handle does not exist.
         */
        if (len < 2) return;
        uint8_t hlen = buf[1];
        char missingHandle[MAX_HANDLE+1] = {0};
        if (len >= 2 + hlen)
            memcpy(missingHandle, buf + 2, hlen);
        missingHandle[hlen] = '\0';
        printf("\nClient with handle %s does not exist.\n", missingHandle);
    }
    else if (flag == 4) {
        /* Broadcast message: Format is [flag=4] [1-byte sender handle length]
         * [sender handle] [text message]
         */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        // The rest of the packet is the broadcast text
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 5) {
        /* Private message: Format is [flag=5] [1-byte sender handle length]
         * [sender handle] [1-byte destination count] [1-byte destination handle length]
         * [destination handle] [text message]
         */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        off++; // Skip destination count (should be 1)
        uint8_t dhLen = buf[off++];
        off += dhLen; // Skip the destination handle
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 6) {
        /* Multicast message: Format is [flag=6] [1-byte sender handle length]
         * [sender handle] [1-byte number of destination handles] [list of dest handles] [text message]
         */
        int off = 1;
        uint8_t shLen = buf[off++];
        char sender[MAX_HANDLE+1] = {0};
        memcpy(sender, buf + off, shLen);
        sender[shLen] = '\0';
        off += shLen;
        uint8_t num = buf[off++]; // Number of destination handles (not used for display)
        for (int i = 0; i < num; i++) {
            uint8_t dlen = buf[off++];
            off += dlen; // Skip each destination handle
        }
        char *msg = (char *)(buf + off);
        printf("\n%s: %s\n", sender, msg);
    }
    else if (flag == 11) {
        /* List response:
         * The first packet (flag=11) contains a 4-byte count of connected clients.
         * It is followed by 'count' packets (each with flag=12) containing individual handles,
         * and finally a termination packet with flag=13.
         */
        if (len < 5) return;
        uint32_t count_net;
        memcpy(&count_net, buf + 1, 4); // Extract the 4-byte count (network byte order)
        uint32_t count = ntohl(count_net); // Convert count to host byte order
        printf("\nNumber of clients: %u\n", count);
        // Loop to receive each handle (flag=12 packets)
        for (uint32_t i = 0; i < count; i++) {
            uint8_t pkt[MAXBUF];
            int r = recvPDU(socketNum, pkt, MAXBUF);
            if (r <= 0) break;
            if (pkt[0] != 12) continue;  // Each handle packet should start with flag 12
            int off = 1;
            uint8_t hlen = pkt[off++];
            char handle[MAX_HANDLE+1] = {0};
            memcpy(handle, pkt + off, hlen);
            handle[hlen] = '\0';
            printf("%s\n", handle);
        }
        // Finally, read the termination packet with flag=13
        uint8_t term;
        recvPDU(socketNum, &term, 1);
    }
    // Reprint the prompt after processing the incoming data
    printf("$: ");
    fflush(stdout);
}

/*
 * clientControlLoop:
 *   Main event loop for the client.
 *   Sets up a polling mechanism to wait for input from either STDIN (user input)
 *   or the socket (incoming data from the server).
 *
 *   When STDIN is ready, processUserInput() is called.
 *   When the socket is ready, processSocketData() is called.
 */
static void clientControlLoop(int socketNum) {
    setupPollSet();              // Initialize the poll set for file descriptors
    addToPollSet(STDIN_FILENO);  // Add standard input (keyboard) to the poll set
    addToPollSet(socketNum);     // Add the server socket to the poll set

    while (1) {
        // Wait indefinitely until one of the monitored file descriptors is ready
        int ready = pollCall(-1);
        if (ready == STDIN_FILENO)
            processUserInput(socketNum);
        else if (ready == socketNum)
            processSocketData(socketNum);
        else {
            // In the unlikely event that an unexpected descriptor is ready, log it.
            fprintf(stderr, "Unexpected FD %d\n", ready);
        }
    }
}

/*
 * main:
 *   The entry point for the chat client.
 *   - Validates command-line arguments.
 *   - Copies the client handle.
 *   - Establishes a TCP connection to the server.
 *   - Optionally sets a clientID.
 *   - Sends the registration packet and waits for the server's response.
 *   - Enters the main control loop to process user input and server messages.
 */
int main(int argc, char *argv[]) {
    /* Validate and check command-line arguments */
    checkArgs(argc, argv);

    /* Copy the provided handle into the global clientHandle variable */
    strncpy(clientHandle, argv[1], MAX_HANDLE);
    clientHandle[MAX_HANDLE] = '\0'; // Ensure the handle is null-terminated

    /* Set up a TCP connection to the server.
       tcpClientSetup() creates the socket and connects to the server using the provided
       server name and port. The third parameter (0) is used for any additional options. */
    int socketNum = tcpClientSetup(argv[2], argv[3], 0);

    /* If an optional clientID is provided, convert it from string to integer */
    if (argc == 5)
        myClientID = atoi(argv[4]);

    /* Print a message indicating connection details */
    printf("Connecting to Server %s on Port %s as Client %s", argv[2], argv[3], clientHandle);
    if (myClientID > 0)
        printf(" (ID %d)", myClientID);
    printf("\n");

    /* Flush STDIN to remove any unwanted buffered input */
    tcflush(STDIN_FILENO, TCIFLUSH);

    /* Send the registration packet to the server immediately upon connection */
    sendRegistration(socketNum);

    /* Wait for the server's registration response and process it */
    uint8_t regResp[100];
    int respLen = recvPDU(socketNum, regResp, 100);
    if (respLen <= 0) {
        fprintf(stderr, "No response from server during registration.\n");
        exit(1);
    }
    processRegistrationResponse(regResp, respLen);

    /* Print the prompt and enter the client control loop */
    printf("$: ");
    fflush(stdout);
    clientControlLoop(socketNum);

    /* Clean up: close the socket before exiting */
    close(socketNum);
    return 0;
}
