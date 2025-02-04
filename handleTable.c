/******************************************************************************
 * handleTable.c
 *
 * Implementation of the handle table API.
 *
 * Uses a simple linked list to store entries.
 * Yes I know, we could use a map or multimap but alas what can you do.
 *
 * Author: Robin Simpson
 * Lab Section: 3 pm
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handleTable.h"

/* 
 * 'head' is a static pointer to the first element in the linked list that 
 * stores all client entries. Since it is declared static at file scope, 
 * it is accessible only within this file.
 */
static struct ClientEntry *head = NULL;

/*
 * initHandleTable:
 *   Initializes the handle table by setting the head of the list to NULL.
 *   This function should be called at server startup to ensure that the 
 *   handle table is empty.
 */
void initHandleTable() {
    head = NULL;
}

/*
 * addHandle:
 *   Adds a new client entry to the handle table.
 *
 * Parameters:
 *   - handle: The string representing the client's handle (username).
 *   - socket: The socket descriptor associated with the client.
 *
 * Returns:
 *   0 on success.
 *
 * Operation:
 *   - Allocates memory for a new ClientEntry structure.
 *   - Copies the provided handle into the structure (ensuring null termination).
 *   - Sets the socket field.
 *   - Inserts the new entry at the beginning of the linked list.
 */
int addHandle(const char *handle, int socket) {
    /* Allocate memory for a new client entry */
    struct ClientEntry *newEntry = malloc(sizeof(struct ClientEntry));
    if (!newEntry) {
        perror("malloc");
        exit(1);
    }
    /* Copy the provided handle into the new entry.
     * Use strncpy to avoid buffer overflow, limiting copy to 100 characters.
     * Then, explicitly set the 101st character to '\0' to ensure proper null-termination.
     */
    strncpy(newEntry->handle, handle, 100);
    newEntry->handle[100] = '\0';
    
    /* Set the socket descriptor for the client */
    newEntry->socket = socket;
    
    /* Insert the new entry at the beginning of the linked list */
    newEntry->next = head;
    head = newEntry;
    
    return 0;
}

/*
 * removeHandleBySocket:
 *   Removes a client entry from the handle table based on its socket descriptor.
 *
 * Parameters:
 *   - socket: The socket descriptor of the client to be removed.
 *
 * Returns:
 *   0 if an entry is successfully removed, or -1 if no matching entry is found.
 *
 * Operation:
 *   - Traverses the linked list looking for an entry whose socket matches the given socket.
 *   - Updates the pointers to remove the entry from the list and frees its memory.
 */
int removeHandleBySocket(int socket) {
    struct ClientEntry *curr = head, *prev = NULL;
    
    /* Traverse the linked list */
    while (curr) {
        if (curr->socket == socket) {
            /* Found the matching entry. Adjust pointers accordingly. */
            if (prev)
                prev->next = curr->next;
            else
                head = curr->next;
                
            /* Free the memory allocated for the removed entry */
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    /* If we reach here, no matching entry was found */
    return -1;
}

/*
 * lookupSocketByHandle:
 *   Searches for a client entry in the handle table by its handle (username)
 *   and returns the associated socket descriptor.
 *
 * Parameters:
 *   - handle: The client's handle to search for.
 *
 * Returns:
 *   The socket descriptor if found, or -1 if no matching entry exists.
 */
int lookupSocketByHandle(const char *handle) {
    struct ClientEntry *curr = head;
    
    /* Traverse the linked list looking for the matching handle */
    while (curr) {
        if (strcmp(curr->handle, handle) == 0)
            return curr->socket;
        curr = curr->next;
    }
    /* Return -1 if no entry with the given handle is found */
    return -1;
}

/*
 * lookupHandleBySocket:
 *   Searches for a client entry by its socket descriptor and returns the
 *   corresponding handle (username).
 *
 * Parameters:
 *   - socket: The socket descriptor of the client.
 *
 * Returns:
 *   A pointer to the handle string if found, or NULL if not found.
 *
 * Note:
 *   The returned pointer refers to the handle stored within the linked list.
 */
char *lookupHandleBySocket(int socket) {
    struct ClientEntry *curr = head;
    
    /* Traverse the linked list */
    while (curr) {
        if (curr->socket == socket)
            return curr->handle;
        curr = curr->next;
    }
    return NULL;
}

/*
 * getHandleCount:
 *   Counts the total number of client entries in the handle table.
 *
 * Returns:
 *   The count of client entries as an unsigned integer.
 *
 * Operation:
 *   - Traverses the linked list and increments a counter for each entry.
 */
unsigned int getHandleCount() {
    unsigned int count = 0;
    struct ClientEntry *curr = head;
    
    /* Loop through the list, counting each entry */
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

/*
 * getHandleTableHead:
 *   Returns the head pointer of the handle table.
 *
 * Returns:
 *   A pointer to the first ClientEntry in the linked list.
 *
 * Note:
 *   This function allows external modules (like the server) to traverse the handle table.
 */
struct ClientEntry *getHandleTableHead() {
    return head;
}
