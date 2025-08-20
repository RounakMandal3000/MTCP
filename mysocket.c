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


MTP_SM* finder(int shmid=SHM_KEY){
    MTP_SM *g_sm = (MTP_SM *)shmat(shmid, NULL, 0);
    if (g_sm == (MTP_SM *)-1) {
        perror("shmat failed");
        return NULL;
    }
    return g_sm;
}

int m_socket(int domain, int type, int protocol){
    if (type != SOCK_MTP) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    MTP_SM* sm = finder();
    // pthread_mutex_lock(&sm->lock_sm);
    if(sm->count_occupied < MAX_SOCKETS){
        pthread_mutex_lock(&sm->lock_sm);
        sm->count_occupied++;
        pthread_mutex_unlock(&sm->lock_sm);

        for(int i = 0; i < MAX_SOCKETS; i++){
            pthread_mutex_lock(&sm->sm_entry[i].lock);
            if(sm->sm_entry[i].free_slot){
                sm->sm_entry[i].free_slot = false;
                sm->sm_entry[i].pid_creation = getpid();
                sm->sm_entry[i].sock->udp_sockfd = socket(domain, SOCK_DGRAM, protocol);
                if (sm->sm_entry[i].sock->udp_sockfd < 0){
                    pthread_mutex_unlock(&sm->sm_entry[i].lock);
                    // pthread_mutex_unlock(&sm->lock_sm);
                    return -1;
                }
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                // pthread_mutex_unlock(&sm->lock_sm);
                return 0;
            }
            pthread_mutex_unlock(&sm->sm_entry[i].lock);
        }
        // pthread_mutex_unlock(&sm->lock_sm);
    }
    else{
        errno = ENOBUFS;
        // pthread_mutex_unlock(&sm->lock_sm);
        return -1;
    }
}

int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen){
    MTP_SM* sm = finder();
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm.sm_entry[i].lock);
        if(sm.sm_entry[i].sock->udp_sockfd == sockfd){
            sm.sm_entry[i].src_addr = *src_addr;
            sm.sm_entry[i].dest_addr = *dest_addr;
            // Bind the socket to the address
            if (bind(sockfd, src_addr, addrlen) < 0) {
                pthread_mutex_unlock(&sm.sm_entry[i].lock);
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
            pthread_mutex_unlock(&sm.sm_entry[i].lock);
            // pthread_mutex_unlock(&sm.lock_sm);
            return 0;
        }
        pthread_mutex_unlock(&sm.sm_entry[i].lock);
    }
    // pthread_mutex_unlock(&sm.lock_sm);
    errno = EBADF;
    return -1;
}

int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen){
    MTP_SM* sm = finder();
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm.sm_entry[i].lock);
        if(sm.sm_entry[i].dest_addr == *to){
            if (is_full(&sm.sm_entry[i].sender->buffer)) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm.sm_entry[i].lock);
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
            int rv = enqueue_recv(&sm.sm_entry[i].sender->buffer, *(MTP_Message *)msg);
            if (rv < 0) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm.sm_entry[i].lock);
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
            pthread_mutex_unlock(&sm.sm_entry[i].lock);
            // pthread_mutex_unlock(&sm.lock_sm);
            return 0;
        }
        pthread_mutex_unlock(&sm.sm_entry[i].lock);
    }
    // pthread_mutex_unlock(&sm.lock_sm);
    errno = ENOTBOUND;
    return -1;
}

int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen){
    MTP_SM* sm = finder();
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0;i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm.sm_entry[i].lock);
        if(sm.sm_entry[i].dest_addr == *from){
            if (is_empty(&sm.sm_entry[i].receiver->buffer)) {
                errno = ENOMSG;
                pthread_mutex_unlock(&sm.sm_entry[i].lock); 
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
            int rv = dequeue_recv(&sm.sm_entry[i].receiver->buffer, (MTP_Message *)buf);   // buf now stores the message
            if (rv < 0) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm.sm_entry[i].lock);
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
        }
        pthread_mutex_unlock(&sm.sm_entry[i].lock);
    }
    // pthread_mutex_unlock(&sm.lock_sm);
    errno = ENOTBOUND;  // HAVE SOME DOUBT HERE, SEE LATER
    return -1;
}

int m_close(int sockfd) {
    MTP_SM* sm = finder();
    int i;
    // pthread_mutex_lock(&sm.lock_sm);
    for (i = 0; i < MAX_SOCKETS; i++) {
        pthread_mutex_lock(&sm.sm_entry[i].lock);
        if (!sm.sm_entry[i].free_slot && sm.sm_entry[i].udp_sockfd == sockfd) {
            break;
        }
        pthread_mutex_unlock(&sm.sm_entry[i].lock);
    }
    if (i == MAX_SOCKETS) {
        errno = EBADF; // invalid socket fd
        // pthread_mutex_unlock(&sm.lock_sm);
        return -1;
    }
    if (close(sm.sm_entry[i].sock->udp_sockfd) < 0) {
        perror("close failed");
        pthread_mutex_unlock(&sm.sm_entry[i].lock);
        // pthread_mutex_unlock(&sm.lock_sm);
        return -1;
    }
    sm->sm_entry[i].sock = malloc(sizeof(MTP_socket));
    sm->sm_entry[i].sock->udp_sockfd = -1;
    sm->sm_entry[i].free_slot = true;
    sm->sm_entry[i].pid_creation = -1;
    sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
    sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
    sm->sm_entry[i].sender = malloc(sizeof(MTP_Sender));
    init_recv_queue(&sm->sm_entry[i].sender->buffer);
    sm->sm_entry[i].sender->swnd_count = 0;
    memset(sm->sm_entry[i].sender->swnd, 0, sizeof(sm->sm_entry[i].sender->swnd));
    sm->sm_entry[i].receiver = malloc(sizeof(MTP_Receiver));
    init_recv_queue(&sm->sm_entry[i].receiver->buffer);
    sm->sm_entry[i].receiver->rwnd_count = 0;
    sm->sm_entry[i].receiver->next_val = 1;
    pthread_mutex_unlock(&sm->sm_entry[i].lock);
    // pthread_mutex_unlock(&sm->lock_sm);
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
    return 0;  // success
}

int is_empty(MTP_Queue *q) {
    return q->count == 0;
}

int is_full(MTP_Queue *q) {
    return q->count == RECV_BUFFER;
}

void traverse_recv(MTP_Queue *q) {
    if (q->front == NULL) {
        printf("Queue is empty\n");
        return;
    }

    Node *current = q->front;
    printf("Queue contents (count=%d):\n", q->count);

    while (current != NULL) {
        // Print message details (adapt as per your MTP_Message struct)
        printf("Message: [id=%d, size=%d]\n", current->msg.id, current->msg.size);
        
        current = current->next;
    }
}


// 4 --> 1 2 5 -> 1 2 4 5 -> 1 2, 2, 1