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

Node* get_node(void *base, int offset) {
    if (offset == -1) return NULL;
    return (Node*)((char*)base + offset);
}

Node *alloc_node_in_shm(MTP_SM *g_sm) {
    // Example: allocate from a preallocated pool in shared memory
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!g_sm->node_pool[i].used) {
            g_sm->node_pool[i].used = 1;
            return &g_sm->node_pool[i].node;
        }
    }
    return NULL; // pool exhausted
}

void free_node_in_shm(MTP_SM *g_sm, Node *node) {
    if (!node) return;

    for (int i = 0; i < MAX_NODES; i++) {
        if (&g_sm->node_pool[i].node == node) {
            g_sm->node_pool[i].used = 0;
            return;
        }
    }
}


MTP_SM* finder(int shmid){
    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0777);

    MTP_SM *g_sm = (MTP_SM *)shmat(shmid, NULL, 0);
    if (g_sm == (MTP_SM *)-1) {
        perror("shmat failed uwu");
        return NULL;
    }

    return g_sm;
}

void* base_finder(int shmid) {
    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0777);
    void *base = shmat(shmid, NULL, 0);
    if (base == (void *)-1) {
        perror("shmat failed");
        return NULL;
    }
    return base;
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
                sm->sm_entry[i].pid_creation = getpid();
                sm->sm_entry[i].sock.udp_sockfd = -2;
                pthread_mutex_lock(&sm->lock_sm);
                sm->bind_socket = 0;
                pthread_mutex_unlock(&sm->lock_sm);
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                sleep(1);
                pthread_mutex_lock(&sm->sm_entry[i].lock);
                if(sm->sm_entry[i].sock.udp_sockfd < 0){
                    perror("socket creation failed");
                    sm->sm_entry[i].free_slot = true;
                    sm->sm_entry[i].pid_creation = -1;
                    pthread_mutex_unlock(&sm->sm_entry[i].lock);
                    // pthread_mutex_unlock(&sm->lock_sm);
                    return -1;
                }
                pthread_mutex_lock(&sm->lock_sm);
                sm->bind_socket = -1;
                pthread_mutex_unlock(&sm->lock_sm);
                sm->sm_entry[i].free_slot = false;
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
            sm->sm_entry[i].src_addr = *(struct sockaddr_in *)src_addr;
            sm->sm_entry[i].dest_addr = *(struct sockaddr_in *)dest_addr;
            // Bind the socket to the address
            // if (bind(sockfd, (struct sockaddr *)&sm->sm_entry[i].src_addr, addrlen) < 0) {
            //     pthread_mutex_unlock(&sm->sm_entry[i].lock);
            //     // pthread_mutex_unlock(&sm.lock_sm);
            //     return -1;
            // }
            pthread_mutex_lock(&sm->lock_sm);
            sm->bind_socket = 1;
            pthread_mutex_unlock(&sm->lock_sm);

            pthread_mutex_unlock(&sm->sm_entry[i].lock);
            pthread_mutex_lock(&sm->lock_sm);
            sm->bind_socket = -1;
            pthread_mutex_unlock(&sm->lock_sm);
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
            if (is_full(i, 0)) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }

            int rv = enqueue_recv(i, *(MTP_Message *)msg, 0);
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
        if(memcmp(&sm->sm_entry[i].dest_addr, from, sizeof(struct sockaddr_in)) == 0){
            if (is_empty(i, 1)) {
                errno = ENOMSG;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            int rv = dequeue_recv(i, (MTP_Message *)buf, 1);   // buf now stores the message
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
    sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
    sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
    init_recv_queue(i, 0);
    sm->sm_entry[i].sender.swnd_count = 0;
    memset(sm->sm_entry[i].sender.swnd, 0, sizeof(sm->sm_entry[i].sender.swnd));
    init_recv_queue(i, 1);
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

// void init_recv_queue(int socket_id, int flag) {
//     MTP_SM *g_sm = finder(SHM_KEY);
//     if (g_sm == NULL) {
//         perror("Failed to find shared memory");
//         return;
//     }
//     MTP_Queue *q;
//     if(flag==1)
//         q = &g_sm->sm_entry[socket_id].receiver.buffer;
//     else
//         q = &g_sm->sm_entry[socket_id].sender.buffer;
//     q->front = NULL;
//     q->rear = NULL;
//     q->count = 0;
// }

void init_recv_queue(int socket_id, int flag) {
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return;
    }

    MTP_Queue *q;
    if (flag == 1)
        q = &g_sm->sm_entry[socket_id].receiver.buffer;
    else
        q = &g_sm->sm_entry[socket_id].sender.buffer;

    // Store offsets instead of pointers
    q->front = 0;   // NULL offset
    q->rear  = 0;   // NULL offset
    q->count = 0;
}

// int enqueue_recv(int socket_id, MTP_Message msg, int flag) {
//     MTP_SM *g_sm = finder(SHM_KEY);
//     printf("%p ajfa;lksfjlkafjslfj\n", (void *)g_sm);
//     if (g_sm == NULL) {
//         perror("Failed to find shared memory");
//         return -1;
//     }
//     MTP_Queue *q;
//     if(flag==1)
//         q = &g_sm->sm_entry[socket_id].receiver.buffer;
//     else
//         q = &g_sm->sm_entry[socket_id].sender.buffer;
//     // printf("%p from enqueue_recv\n", (void *)q);
//     Node *new_node = (Node*)malloc(sizeof(Node));
//     if (!new_node) return -1;  // malloc failed
//     if (is_full(socket_id, flag)) {
//         free(new_node);
//         return -1;  // queue is full
//     }

//     new_node->msg = msg;   // copy message
    
//     new_node->next = NULL;

//     if (q->rear == NULL) {
//         // empty queue
//         q->front = new_node;
//         q->rear = new_node;
//     } else {
//         q->rear->next = new_node;
//         q->rear = new_node;
//     }

//     q->count++;
//     return 0;  // success
// }

int enqueue_recv(int socket_id, MTP_Message msg, int flag) {
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return -1;
    }

    MTP_Queue *q;
    if (flag == 1)
        q = &g_sm->sm_entry[socket_id].receiver.buffer;
    else
        q = &g_sm->sm_entry[socket_id].sender.buffer;

    if (is_full(socket_id, flag)) {
        return -1;  // queue is full
    }

    // Allocate node INSIDE shared memory instead of malloc
    Node *new_node = alloc_node_in_shm(g_sm);  // custom allocator
    if (!new_node) return -1;

    new_node->msg = msg;
    new_node->next = 0;  // NULL offset

    size_t new_off = OFFSET(g_sm, new_node);

    if (q->rear == 0) {
        // empty queue
        q->front = new_off;
        q->rear = new_off;
    } else {
        Node *rear_node = (Node*)PTR(g_sm, q->rear);
        rear_node->next = new_off;
        q->rear = new_off;
    }

    q->count++;
    return 0;  // success
}


// int dequeue_recv(int socket_id, MTP_Message *msg, int flag) {
//     MTP_SM *g_sm = finder(SHM_KEY);
//     printf("%p \n", (void *)g_sm);
//     if (g_sm == NULL) {
//         perror("Failed to find shared memory");
//         return -1;
//     }
//     MTP_Queue *q;
//     if(flag==1)
//         q = &g_sm->sm_entry[socket_id].receiver.buffer;
//     else
//         q = &g_sm->sm_entry[socket_id].sender.buffer;
//     printf("%p from enqueue_recv\n", (void *)q);
//     printf("Dequeuing message from receiver buffer\n");
//     if (q->front == NULL) {
//         // Queue is empty
//         return -1;
//     }

//     Node *temp = q->front;
//     *msg = temp->msg;   // copy message to caller

//     q->front = q->front->next;
//     if (q->front == NULL) {
//         // Queue became empty
//         q->rear = NULL;
//     }

//     free(temp);
//     q->count--;
//     return 0;  // success
// }

int dequeue_recv(int socket_id, MTP_Message *msg, int flag) {
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return -1;
    }

    MTP_Queue *q;
    if (flag == 1)
        q = &g_sm->sm_entry[socket_id].receiver.buffer;
    else
        q = &g_sm->sm_entry[socket_id].sender.buffer;

    if (q->front == 0) {
        // Queue is empty (0 means NULL offset)
        return -1;
    }

    // Get front node
    Node *front_node = (Node*)PTR(g_sm, q->front);

    // Copy message out
    *msg = front_node->msg;

    // Move front forward
    q->front = front_node->next;

    if (q->front == 0) {
        // Queue became empty
        q->rear = 0;
    }

    // Free node (return it to SHM allocator pool)
    free_node_in_shm(g_sm, front_node);

    q->count--;
    return 0;  // success
}

// int is_empty(int socket_id, int flag) {
//     MTP_SM *g_sm = finder(SHM_KEY);
//     if (g_sm == NULL) {
//         perror("Failed to find shared memory");
//         return -1;
//     }
//     MTP_Queue *q;
//     if(flag==1)
//         q = &g_sm->sm_entry[socket_id].receiver.buffer;
//     else
//         q = &g_sm->sm_entry[socket_id].sender.buffer;
//     return q->count == 0;
// }

int is_empty(int socket_id, int flag) {
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return -1;
    }

    MTP_Queue *q;
    if (flag == 1)
        q = &g_sm->sm_entry[socket_id].receiver.buffer;
    else
        q = &g_sm->sm_entry[socket_id].sender.buffer;

    // Trust count, but double check front offset
    return (q->count == 0 || q->front == 0);
}

// int is_full(int socket_id, int flag) {
//     MTP_SM *g_sm = finder(SHM_KEY);
//     if (g_sm == NULL) {
//         perror("Failed to find shared memory");
//         return -1;
//     }
//     MTP_Queue *q;
//     if(flag==1)
//         q = &g_sm->sm_entry[socket_id].receiver.buffer;
//     else
//         q = &g_sm->sm_entry[socket_id].sender.buffer;
//     return q->count == RECV_BUFFER;
// }
int is_full(int socket_id, int flag) {
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return -1;
    }

    MTP_Queue *q;
    if (flag == 1)
        q = &g_sm->sm_entry[socket_id].receiver.buffer;
    else
        q = &g_sm->sm_entry[socket_id].sender.buffer;

    if (flag == 1){
        if(q->count >= RECV_BUFFER) return 1;
    }
    else{
        if(q->count >= SENDER_BUFFER) return 1;
    }
    return 0;
}


void* file_to_sender_thread(void* arg) {
    int sockfd = *(int *)arg;

    MTP_SM *g_sm = finder(SHM_KEY);
    FILE *file = fopen("large_file.txt", "r");
    if (!file) {
        perror("Failed to open file");
        return NULL;
    }
    
    int cnt=0;
    char line[MTP_MSG_SIZE];
    int i = 1, j_ = 0;
    for(j_ = 0;j_ < MAX_SOCKETS;j_++){
        pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
        if(g_sm->sm_entry[j_].sock.udp_sockfd == sockfd){
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
    while(true){
        while (fgets(line, sizeof(line), file)) {
            pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
            MTP_Message msg = {0};
            strncpy(msg.data, line, sizeof(msg.data) - 1);
            msg.data[sizeof(msg.data) - 1] = '\0';
            msg.seq_num = i; // CHANGE THIS
            msg.is_ack = false; // Not an ACK
            msg.wnd_sz= -1;
            msg.next_val = -1;
            pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            int rv = m_sendto(sockfd, (const void *)&msg, sizeof(msg), 0, (const struct sockaddr *)&g_sm->sm_entry[j_].dest_addr, sizeof(g_sm->sm_entry[j_].dest_addr));
            if (rv < 0) {
                perror("Failed to send_to message");
                sleep(2);
                continue;
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
                sleep(T/3);
                continue;
            }
            fprintf(file, "%s\n", buf.data);
            printf("Successfully wrote to file\n");
            fflush(file);
        }
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
        sleep(3);
    }
    fclose(file);
    return NULL;
}