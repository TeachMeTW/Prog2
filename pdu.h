#ifndef PDU_H
#define PDU_H

#include <stdint.h>

/*
 * sendPDU():
 *   Creates a 2-byte big-endian length header, places the payload after that header,
 *   and sends it in ONE send() call.
 *   Return value: the number of data bytes sent (not counting the 2-byte header),
 *                 or -1 if an error occurs.
 */
int sendPDU(int socketNumber, const uint8_t *dataBuffer, int lengthOfData);

/*
 * recvPDU():
 *   1) First recv() 2 bytes (using MSG_WAITALL) => total PDU length in network order.
 *   2) If 0 => other side closed => return 0.
 *   3) Convert length to host order => totalLen
 *   4) Payload length = totalLen - 2
 *   5) Check bufferSize >= payload length; else error => exit
 *   6) recv() the payload using MSG_WAITALL into dataBuffer
 *   7) Return the number of data bytes read (payload), or 0 if closed, or -1 on error
 */
int recvPDU(int socketNumber, uint8_t *dataBuffer, int bufferSize);

#endif
