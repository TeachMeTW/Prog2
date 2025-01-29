#include "pdu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>   // htons, ntohs
#include <errno.h>
#include <sys/socket.h>  // send(), recv()

#include "safeUtil.h"    // for safeSend(), safeRecv()

/*
 * sendPDU():
 *   Build the total PDU: 2-byte length header (in network order) + payload.
 *   Send it in ONE send() call. Return data-bytes-sent (excluding header),
 *   or -1 if an error is detected.
 */
int sendPDU(int socketNumber, const uint8_t *dataBuffer, int lengthOfData)
{
    // totalLen includes the 2-byte header + the actual payload
    int totalLen = lengthOfData + 2;

    // Allocate a temporary buffer to build the entire PDU
    uint8_t *pdu = (uint8_t*)malloc(totalLen);
    if (!pdu)
    {
        perror("sendPDU malloc");
        exit(-1);
    }

    // Convert totalLen to network order, store in first 2 bytes
    uint16_t netLen = htons(totalLen);
    memcpy(pdu, &netLen, 2);

    // Copy user data after the 2-byte header
    memcpy(pdu + 2, dataBuffer, lengthOfData);

    // Send in one call. safeSend() either returns bytesSent or exits on error
    int bytesSent = safeSend(socketNumber, pdu, totalLen, 0);

    free(pdu);

    // If safeSend() doesn’t exit on error, it might return <0; handle it:
    if (bytesSent < 0) 
    {
        return -1;
    }

    // bytesSent should match totalLen on success
    // We only return how many DATA bytes were sent
    return (bytesSent - 2);
}

/*
 * recvPDU():
 *   1) Read 2 bytes => total PDU length
 *   2) If read=0 => other side closed => return 0
 *   3) Convert to host order => totalLen
 *   4) payloadLen = totalLen - 2
 *   5) Make sure payloadLen <= bufferSize, else exit(-1)
 *   6) read payloadLen bytes => dataBuffer
 *   7) Return payloadLen, or 0 if closed, or -1 if error
 */
int recvPDU(int socketNumber, uint8_t *dataBuffer, int bufferSize)
{
    uint8_t lengthBuf[2];
    int ret = 0;

    // Step 1: read 2-byte header with MSG_WAITALL
    ret = recv(socketNumber, lengthBuf, 2, MSG_WAITALL);
    if (ret < 0)
    {
        // On some OS, ECONNRESET => treat as closed
        if (errno == ECONNRESET)
        {
            return 0;
        }
        perror("recvPDU header");
        return -1;
    }
    if (ret == 0)
    {
        // means other side closed
        return 0;
    }
    if (ret != 2)
    {
        // partial read => error
        fprintf(stderr, "recvPDU: partial header read (ret=%d)\n", ret);
        return -1;
    }

    // Step 2: parse totalLen
    uint16_t netLen = 0;
    memcpy(&netLen, lengthBuf, 2);
    int totalLen = ntohs(netLen);

    // totalLen includes the 2 header bytes => payloadLen
    int payloadLen = totalLen - 2;

    // Step 3: check buffer size
    if (payloadLen > bufferSize)
    {
        fprintf(stderr, "recvPDU error: PDU len %d exceeds bufferSize %d\n",
                payloadLen, bufferSize);
        exit(-1);
    }

    // Step 4: read payloadLen bytes into dataBuffer
    ret = recv(socketNumber, dataBuffer, payloadLen, MSG_WAITALL);
    if (ret < 0)
    {
        if (errno == ECONNRESET)
        {
            return 0;
        }
        perror("recvPDU payload");
        return -1;
    }
    if (ret == 0)
    {
        // closed mid read
        return 0;
    }
    if (ret != payloadLen)
    {
        fprintf(stderr, "recvPDU: partial payload read (ret=%d)\n", ret);
        return -1;
    }

    // success => ret == payloadLen
    return payloadLen;
}
