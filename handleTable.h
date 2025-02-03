/******************************************************************************
 * handleTable.h
 *
 * API for the server’s handle table.
 *
 * Defines a dynamic data structure (here implemented as a linked list)
 * that maps a client’s handle (a string) to its socket descriptor.
 *
 * Functions:
 *    initHandleTable() – must be called at server startup.
 *    addHandle(handle, socket) – adds a client entry.
 *    removeHandleBySocket(socket) – removes an entry by socket.
 *    lookupSocketByHandle(handle) – returns the socket for a given handle (or -1 if not found).
 *    lookupHandleBySocket(socket) – returns the registered handle for a given socket (or NULL).
 *    getHandleCount() – returns the number of registered handles.
 *    getHandleTableHead() – returns a pointer to the head of the table (for iteration).
 *
 * Author: Robin Simpson
 * Lab Section: 3 pm
 *****************************************************************************/

#ifndef HANDLETABLE_H
#define HANDLETABLE_H

struct ClientEntry {
    char handle[101];
    int socket;
    struct ClientEntry *next;
};

void initHandleTable();
int addHandle(const char *handle, int socket);
int removeHandleBySocket(int socket);
int lookupSocketByHandle(const char *handle); // Returns socket or -1 if not found.
char *lookupHandleBySocket(int socket);         // Returns pointer to the handle string or NULL.
unsigned int getHandleCount();
struct ClientEntry *getHandleTableHead();

#endif
