/******************************************************************************
 * handleTable.c
 *
 * Implementation of the handle table API.
 *
 * Uses a simple linked list to store entries.
 *
 * Author: Robin Simpson
 * Lab Section: 3 pm
 *****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "handleTable.h"

static struct ClientEntry *head = NULL;

void initHandleTable() {
    head = NULL;
}

int addHandle(const char *handle, int socket) {
    struct ClientEntry *newEntry = malloc(sizeof(struct ClientEntry));
    if (!newEntry) {
        perror("malloc");
        exit(1);
    }
    strncpy(newEntry->handle, handle, 100);
    newEntry->handle[100] = '\0';
    newEntry->socket = socket;
    newEntry->next = head;
    head = newEntry;
    return 0;
}

int removeHandleBySocket(int socket) {
    struct ClientEntry *curr = head, *prev = NULL;
    while (curr) {
        if (curr->socket == socket) {
            if (prev)
                prev->next = curr->next;
            else
                head = curr->next;
            free(curr);
            return 0;
        }
        prev = curr;
        curr = curr->next;
    }
    return -1;
}

int lookupSocketByHandle(const char *handle) {
    struct ClientEntry *curr = head;
    while (curr) {
        if (strcmp(curr->handle, handle) == 0)
            return curr->socket;
        curr = curr->next;
    }
    return -1;
}

char *lookupHandleBySocket(int socket) {
    struct ClientEntry *curr = head;
    while (curr) {
        if (curr->socket == socket)
            return curr->handle;
        curr = curr->next;
    }
    return NULL;
}

unsigned int getHandleCount() {
    unsigned int count = 0;
    struct ClientEntry *curr = head;
    while (curr) {
        count++;
        curr = curr->next;
    }
    return count;
}

struct ClientEntry *getHandleTableHead() {
    return head;
}
