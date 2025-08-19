#include "msocket.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

int m_socket(int domain, int type, int protocol){
    if (type != SOCK_MTP) {
        errno = EINVAL;   // invalid argument
        return -1;
    }
    if(sm.any_free){
        sm.any_free--;
        for(int i = 0; i < MAX_SOCKETS; i++){
            if(sm.sm_entry[i].free_slot){
                sm.sm_entry[i].free_slot = false;
                sm.sm_entry[i].pid_creation = getpid();
                sm.sm_entry[i].udp_sockfd = socket(domain, SOCK_DGRAM, protocol);
                if (sm.sm_entry[i].udp_sockfd < 0) return -1;
                sm.sm_entry[i].sender = malloc(sizeof(MTP_Sender));
                sm.sm_entry[i].receiver = malloc(sizeof(MTP_Receiver));
                return 0;
            }
        }
    }
    else{
        errno = ENOBUFS;
        return -1;
    }
}

int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen){
    for(int i=0; i<MAX_SOCKETS; i++){
        if(sm.sm_entry[i].udp_sockfd == sockfd){
            // Bind the socket to the address
            if (bind(sockfd, src_addr, addrlen) < 0) {
                return -1;
            }
            sm.sm_entry[i].dest_addr = *dest_addr;
            return 0;
        }
    }
    errno = EBADF;
    return -1;
}

int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen){
    for(int i=0; i<MAX_SOCKETS; i++){
        if(sm.sm_entry[i].udp_sockfd == sockfd){
            if (sm.sm_entry[i].sender->swnd_end == SENDER_WND) {
                errno = ENOBUFS;
                return -1;
            }
            sm.sm_entry[i].sender->buffer[sm.sm_entry[i].sender->swnd_end] = *(MTP_Message *)msg;
            sm.sm_entry[i].sender->swnd_end++;
            return 0;
        }
    }
    errno = ENOTBOUND;
    return -1;
}

int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen){
    for(int i=0;i<MAX_SOCKETS; i++){
        if(sm.sm_entry[i].udp_sockfd == sockfd){
            if (sm.sm_entry[i].receiver->rwnd_start == 0) {
                errno = ENOMSG;
                return -1;
            }
            if (sm.sm_entry[i].receiver->occupied[sm.sm_entry[i].receiver->rwnd_start]) {
                *(MTP_Message *)buf = sm.sm_entry[i].receiver->buffer[sm.sm_entry[i].receiver->rwnd_start];
                sm.sm_entry[i].receiver->occupied[sm.sm_entry[i].receiver->rwnd_start] = false;
                sm.sm_entry[i].receiver->rwnd_start++;
                return sizeof(MTP_Message);
            }
            errno = ENOBUFS;
            return -1;
        }
    }
    errno = ENOTBOUND;  // HAVE SOME DOUBT HERE, SEE LATER
    return -1;
}

int m_close(int sockfd) {
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        if (!sm.sm_entry[i].free_slot && sm.sm_entry[i].udp_sockfd == sockfd) {
            break;
        }
    }
    if (i == MAX_SOCKETS) {
        errno = EBADF; // invalid socket fd
        return -1;
    }
    if (close(sm.sm_entry[i].udp_sockfd) < 0) {
        return -1;
    }
    sm.sm_entry[i].free_slot = true;
    sm.sm_entry[i].udp_sockfd = -1;
    memset(&sm.sm_entry[i].src_addr, 0, sizeof(struct sockaddr_in));
    memset(&sm.sm_entry[i].dest_addr, 0, sizeof(struct sockaddr_in));
    sm.sm_entry[i].sender = NULL;
    sm.sm_entry[i].receiver = NULL;
    sm.sm_entry[i].pid_creation = -1;
    return 0;
}

int dropMessage(float p){
    float random_num = (float)rand() / RAND_MAX;
    if (random_num < p)
        return 1; // Message dropped
    else
        return 0; // Message not dropped
}

void init_recv_queue(MTP_Queue *q) {
    q->front = NULL;
    q->rear = NULL;
    q->count = 0;
}

int enqueue_recv(MTP_Queue *q, MTP_Message msg) {
    Node *new_node = (Node*)malloc(sizeof(Node));
    if (!new_node) return -1;  // malloc failed
    if (is_full(q)) {
        free(new_node);
        return -1;  // queue is full
    }

    new_node->msg = msg;   // copy message
    new_node->next = NULL;

    if (q->rear == NULL) {
        // empty queue
        q->front = q->rear = new_node;
    } else {
        q->rear->next = new_node;
        q->rear = new_node;
    }

    q->count++;
    return 0;  // success
}

int dequeue_recv(MTP_Queue *q, MTP_Message *msg) {
    if (q->front == NULL) {
        return -1;  // empty queue
    }

    Node *temp = q->front;
    *msg = temp->msg;  // return message

    q->front = q->front->next;
    if (q->front == NULL) {
        // queue became empty
        q->rear = NULL;
    }

    free(temp);
    q->count--;
    return RecvNode;  // success
}

int is_empty(MTP_Queue *q) {
    return q->count == 0;
}

int is_full(MTP_Queue *q) {
    return q->count == RECV_BUFFER;
}