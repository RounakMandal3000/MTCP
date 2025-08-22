#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

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
#include <sys/ipc.h>
#include <sys/shm.h>
#include <pthread.h>


MTP_SM* finder(int shmid){
    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0666);

    MTP_SM *g_sm = (MTP_SM *)shmat(shmid, NULL, 0);
    if (g_sm == (MTP_SM *)-1) {
        perror("shmat failed uwu");
        return NULL;
    }

    return g_sm;
}

int m_socket(int domain, int type, int protocol){
    
    if (type != SOCK_MTP) {
        errno = EPROTONOSUPPORT;
        return -1;
    }
    
    MTP_SM* sm = finder(SHM_KEY);
    // pthread_mutex_lock(&sm->lock_sm);
    if(sm->count_occupied < MAX_SOCKETS){
        pthread_mutex_lock(&sm->lock_sm);
        sm->count_occupied++;
        pthread_mutex_unlock(&sm->lock_sm);
        for(int i = 0; i < MAX_SOCKETS; i++){
            pthread_mutex_lock(&(sm->sm_entry[i].lock));
            if(sm->sm_entry[i].free_slot){
                sm->sm_entry[i].free_slot = false;
                sm->sm_entry[i].pid_creation = getpid();
                sm->sm_entry[i].sock.udp_sockfd = socket(domain, SOCK_DGRAM, protocol);
                // enqueue_pid(&sm->pid_queue, sm->sm_entry[i].pid_creation, );
                if (sm->sm_entry[i].sock.udp_sockfd < 0){
                    pthread_mutex_unlock(&sm->sm_entry[i].lock);
                    // pthread_mutex_unlock(&sm->lock_sm);
                    return -1;
                }

                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                // pthread_mutex_unlock(&sm->lock_sm);
                return sm->sm_entry[i].sock.udp_sockfd;
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
    return 0;
}

int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen){
    MTP_SM* sm = finder(SHM_KEY);
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(sm->sm_entry[i].sock.udp_sockfd == sockfd){
            sm->sm_entry[i].src_addr = *src_addr;
            sm->sm_entry[i].dest_addr = *dest_addr;
            // Bind the socket to the address
            if (bind(sockfd, src_addr, addrlen) < 0) {
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                // pthread_mutex_unlock(&sm.lock_sm);
                return -1;
            }
            pthread_mutex_unlock(&sm->sm_entry[i].lock);
            // pthread_mutex_unlock(&sm.lock_sm);
            return 0;
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    // pthread_mutex_unlock(&sm.lock_sm);
    errno = EBADF;
    return -1;
}

int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen){
    MTP_SM* sm = finder(SHM_KEY);
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(memcmp(&sm->sm_entry[i].dest_addr, to, sizeof(struct sockaddr)) == 0){
            if (is_full(&sm->sm_entry[i].sender.buffer)) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            int rv = enqueue_recv(&sm->sm_entry[i].sender.buffer, *(MTP_Message *)msg);
            if (rv < 0) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            sm->sm_entry[i].free_slot = false;
            pthread_mutex_unlock(&sm->sm_entry[i].lock);
            return 0;
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    errno = ENOTBOUND;
    return -1;
}

int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen){
    MTP_SM* sm = finder(SHM_KEY);
    // pthread_mutex_lock(&sm.lock_sm);
    for(int i=0;i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(memcmp(&sm->sm_entry[i].dest_addr, from, sizeof(struct sockaddr)) == 0){
            if (is_empty(&sm->sm_entry[i].receiver.buffer)) {
                errno = ENOMSG;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            int rv = dequeue_recv(&sm->sm_entry[i].receiver.buffer, (MTP_Message *)buf);   // buf now stores the message
            if (rv < 0) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    // pthread_mutex_unlock(&sm.lock_sm);
    errno = ENOTBOUND;  // HAVE SOME DOUBT HERE, SEE LATER
    return -1;
}

int m_close(int sockfd) {
    MTP_SM* sm = finder(SHM_KEY);
    int i;
    // pthread_mutex_lock(&sm.lock_sm);
    for (i = 0; i < MAX_SOCKETS; i++) {
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if (!sm->sm_entry[i].free_slot && sm->sm_entry[i].sock.udp_sockfd == sockfd) {
            break;
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    if (i == MAX_SOCKETS) {
        errno = EBADF; // invalid socket fd
        // pthread_mutex_unlock(&sm.lock_sm);
        return -1;
    }
    if (close(sm->sm_entry[i].sock.udp_sockfd) < 0) {
        perror("close failed");
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
        // pthread_mutex_unlock(&sm.lock_sm);
        return -1;
    }
    sm->sm_entry[i].sock = (MTP_socket){0};
    sm->sm_entry[i].sock.udp_sockfd = -1;
    sm->sm_entry[i].free_slot = true;
    sm->sm_entry[i].pid_creation = -1;
    sm->sm_entry[i].src_addr = (struct sockaddr){0};
    sm->sm_entry[i].dest_addr = (struct sockaddr){0};
    init_recv_queue(&sm->sm_entry[i].sender.buffer);
    sm->sm_entry[i].sender.swnd_count = 0;
    memset(sm->sm_entry[i].sender.swnd, 0, sizeof(sm->sm_entry[i].sender.swnd));
    init_recv_queue(&sm->sm_entry[i].receiver.buffer);
    sm->sm_entry[i].receiver.rwnd_count = 0;
    sm->sm_entry[i].receiver.next_val = 1;
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
    // printf("Queue contents (count=%d):\n", q->count);

    while (current != NULL) {
        // Print message details (adapt as per your MTP_Message struct)
        // printf("Message: [id=%d, size=%d]\n", current->msg.id, current->msg.size);
        
        current = current->next;
    }
}


// // Initialize the queue
// void initQueue_pid(PIDQueue *q) {
//     q->front = NULL;
//     q->rear = NULL;
//     q->size = 0;
// }

// // Check if queue is empty
// int is_Empty_pid(PIDQueue *q) {
//     return q->front == NULL;
// }

// // Enqueue
// void enqueue_pid(PIDQueue *q, pid_t pid, char *filename) {
//     Node_pid *newNode = malloc(sizeof(Node_pid));
//     newNode->pid = pid;
//     newNode->filename = filename;
//     newNode->next = NULL;
//     newNode->prev = q->rear;

//     if (q->rear) q->rear->next = newNode;
//     q->rear = newNode;
//     if (!q->front) q->front = newNode;

//     q->size++;
// }

// // Dequeue
// Node_pid dequeue_pid(PIDQueue *q) {
//     if (is_Empty_pid(q)) return (Node_pid){-1, NULL, NULL, NULL};  // or handle error

//     Node_pid *tmp = q->front;
//     pid_t pid = tmp->pid;

//     q->front = tmp->next;
//     if (q->front) q->front->prev = NULL;
//     else q->rear = NULL;  // queue is now empty

//     free(tmp);
//     q->size--;

//     return (Node_pid){pid, tmp->filename, NULL, NULL};
// }

void* file_to_sender_thread(void* arg) {
    int sockfd = *(int *)arg;

    MTP_SM *g_sm = finder(SHM_KEY);
    FILE *file = fopen("large_file.txt", "r");
    if (!file) {
        perror("Failed to open file");
        return NULL;
    }
    
    MTP_Sender sender = {0};
    int cnt=0;
    char line[MTP_MSG_SIZE];
    int i = 1, j_ = 0;
    for(j_ = 0;j_ < MAX_SOCKETS;j_++){
        pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
        if(g_sm->sm_entry[j_].sock.udp_sockfd == sockfd){
            sender = g_sm->sm_entry[j_].sender;
            cnt=1;
            pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            break;
        }
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
    }
    if(cnt == 0){
        perror("Sender not found");
        fclose(file);
        return NULL;
    }
    printf("%d %d\n", g_sm->sm_entry[j_].sock.udp_sockfd, sender.swnd_count);
    while(true){

        while (fgets(line, sizeof(line), file) && sender.swnd_count < SENDER_SWND) {
            pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
            MTP_Message msg = {0};
            msg.data = strdup(line);
            msg.seq_num = i; // CHANGE THIS
            msg.is_ack = false; // Not an ACK
            msg.wnd_sz= -1;
            msg.next_val = -1;
            // struct timespec now;
            // clock_gettime(CLOCK_MONOTONIC, &now);
            // msg.sent_time = now;
            int rv = enqueue_recv(&(sender.buffer), msg);
            if (rv < 0) {
                perror("Failed to enqueue message");
                pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
                perror("kock");
                sleep(T/3);
                continue;
            }
            else{
                printf("SUCCESS: %d\n", i);
            }
            
            pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            rv = m_sendto(sockfd, (const void *)&msg, sizeof(msg), 0, (const struct sockaddr *)&g_sm->sm_entry[j_].dest_addr, sizeof(g_sm->sm_entry[j_].dest_addr));
            if (rv < 0) {
                perror("Failed to send message");
            }
            i = (i)%16+1;
        }
        sleep(3);
    }
    fclose(file);
    return NULL;
}

void* receiver_to_file_thread(void* arg) {
    int sockfd = *(int*)arg;
    MTP_SM *g_sm = finder(SHM_KEY);
    int j_ = 0;
    for(j_ = 0;j_ < MAX_SOCKETS;j_++){
        pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
        if(g_sm->sm_entry[j_].sock.udp_sockfd == sockfd){
            pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            break;
        }
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
    }
    char filename[64] = "receiver_buffer.txt";
    FILE *file = fopen(filename, "a");
    if (!file) {
        perror("Failed to open file for writing");
        return NULL; 
    }
        
    while(true){
        pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
        MTP_Message buf;
        int addrlen = sizeof(g_sm->sm_entry[j_].dest_addr);
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
        while (true) {
            int rv = m_recvfrom(sockfd, (void *)&buf, sizeof(buf), 0, (struct sockaddr *)&g_sm->sm_entry[j_].dest_addr, &addrlen);
            if (rv < 0) {
                perror("Failed to receive message");
                sleep(2);
                continue;
            }
            fprintf(file, "%s\n", buf.data);
            printf("Successfully wrote to file\n");
            fflush(file);
            free(buf.data);
        }
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
        sleep(3);
    }
    fclose(file);
    return NULL;
}