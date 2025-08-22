#define _GNU_SOURCE
#define _XOPEN_SOURCE 700

#include "msocket.h"
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <stdlib.h>
#include <string.h>

struct timespec default_time = {0};

void* receiver_thread(void *arg);
void* sender_thread(void *arg);
void* garbage_collector_thread(void *arg);
int socket_function();


void initializer_message(MTP_Message* msg, bool is_ack){
    msg->seq_num = -1;  // or 0 depending on design
    msg->wnd_sz = -1; // if you use a bool
    msg->is_ack = is_ack;
    msg->next_val = -1;
    msg->data = malloc(MTP_MSG_SIZE);
    msg->sent_time = default_time;
}

int main(){
    
    int shmid;
    MTP_SM *g_sm;
    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0777 | IPC_CREAT);
    if (shmid < 0) {
        perror("shmget failed");
        exit(1);
    }

    g_sm = (MTP_SM *)shmat(shmid, NULL, 0);
    if (g_sm == (MTP_SM *)-1) {
        perror("shmat failed");
        exit(1);
    }
    g_sm->count_occupied = 0;
    g_sm->lock_sm = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    // initQueue_pid(&g_sm->.pid_queue);

    // g_sm->sm_entry = malloc(sizeof(MTP_SM_entry) * MAX_SOCKETS);
    
    for(int i=0;i<MAX_SOCKETS;i++){
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);  // critical

        pthread_mutex_init(&g_sm->sm_entry[i].lock, &attr);
        pthread_mutexattr_destroy(&attr);
        g_sm->sm_entry[i].sock = (MTP_socket){0};
        g_sm->sm_entry[i].sock.udp_sockfd = -1;
        g_sm->sm_entry[i].free_slot = true;
        g_sm->sm_entry[i].pid_creation = -1;
        g_sm->sm_entry[i].src_addr = (struct sockaddr){0};
        g_sm->sm_entry[i].dest_addr = (struct sockaddr){0};
        init_recv_queue(i, 0);
        g_sm->sm_entry[i].sender.swnd_count = 0;
        for (int j = 0; j < SENDER_SWND; j++) {
            g_sm->sm_entry[i].sender.swnd[j].seq_num = -1;  // or 0 depending on design
            g_sm->sm_entry[i].sender.swnd[j].wnd_sz = -1; // if you use a bool
            g_sm->sm_entry[i].sender.swnd[j].is_ack = false;
            g_sm->sm_entry[i].sender.swnd[j].next_val = -1;
            g_sm->sm_entry[i].sender.swnd[j].data = malloc(MTP_MSG_SIZE);
            g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
        }

        init_recv_queue(i, 1);
        g_sm->sm_entry[i].receiver.rwnd_count = 0;
        g_sm->sm_entry[i].receiver.next_val = 1;
    }
    // while(1){
    //     // // Periodically check and clean up resources
    //     // if(!is_Empty_pid(g_sm->pid_queue)){
    //     //     while(!is_Empty_pid(g_sm->pid_queue)){
    //     //         Node_pid node = dequeue_pid(&g_sm->pid_queue);
    //     //         pid_t pid = node.pid;
    //     //         char *filename = node.filename;
    //     //         // Perform cleanup for the given PID
    //     //         socket_function(pid, filename);
    //     //     }
    //     // }
    //     sleep(1);
    // }
    // shmdt(g_sm);
    socket_function();
    return 0;
}

int socket_function(){
    pthread_t tid_R, tid_S;

    if (pthread_create(&tid_R, NULL, receiver_thread, NULL) != 0) {
        perror("Failed to create receiver thread");
        exit(1);
    }

    if (pthread_create(&tid_S, NULL, sender_thread, NULL) != 0) {
        perror("Failed to create sender thread");
        exit(1);
    }

    if (pthread_join(tid_S, NULL) != 0) {
        perror("Failed to join sender thread");
        exit(1);
    }
    if (pthread_join(tid_R, NULL) != 0) {
        perror("Failed to join receiver thread");
        exit(1);
    }
    // if (pthread_create(&tid_garbage, NULL, garbage_collector_thread, NULL) != 0) {
    //     perror("Failed to create garbage collector thread");
    //     exit(1);
    // }

    pthread_join(tid_R, NULL);
    pthread_join(tid_S, NULL);
    // pthread_join(tid_garbage, NULL);

    return 0;
}

void* receiver_thread(void *arg) {
    printf("Receiver thread started\n");
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
    // loop: recvfrom() on UDP socket, update rwnd
    fd_set rfds;
    FD_ZERO(&rfds);
    struct timeval tv;
    tv.tv_sec = TIME_SEC;
    tv.tv_usec = TIME_USEC;
    int maxfd = -1;
    while (1) {
        // blocking receive, e.g. recvfrom(sock, buf, ...);
        // pthread_mutex_lock(&g_sm->lock_sm);
        sleep(2);
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (g_sm->sm_entry[i].free_slot){
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            int fd = g_sm->sm_entry[i].sock.udp_sockfd;
            if (fd >= maxfd) maxfd = fd + 1;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            FD_SET(fd, &rfds);
        }

        int rv = select(maxfd, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            continue;
        }
        if (rv == 0) {                  // WRITE THIS LATER, TIMEOUT CASE
            printf("Timeout occurred! No data after %d seconds.\n", TIME_SEC);
            continue;
        }
        if(rv > 0){

            printf("okay\n");
            // pthread_mutex_lock(&g_sm->lock_sm);
            for (int i = 0; i < MAX_SOCKETS; ++i) {
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                if (g_sm->sm_entry[i].free_slot) {
                    pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    continue;
                }
                int fd = g_sm->sm_entry[i].sock.udp_sockfd;
                if (FD_ISSET(fd, &rfds)) {
                    // Data is available to read
                    MTP_Message *buf = malloc(sizeof(MTP_Message));
                    struct sockaddr from = g_sm->sm_entry[i].dest_addr;
                    socklen_t fromlen = sizeof(from);
                    pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    int n = recvfrom(fd, buf, sizeof(MTP_Message), 0, (struct sockaddr *)&from, &fromlen);
                    if (n < 0) {
                        perror("recvfrom failed");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        continue;
                    }
                    if(!buf->is_ack) { // Check for specific packet type
                        // Process received packet
                        MTP_Message *msg = (MTP_Message *)buf;
                        if(g_sm->sm_entry[i].receiver.rwnd_count == RECV_SWND){
                            perror("Receiver window is full");
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                        int ret = enqueue_recv(i, *msg, 1);
                        if(ret < 0) {
                            perror("enqueue_recv failed");
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                        MTP_Message *ack_back = malloc(sizeof(MTP_Message));
                        initializer_message(ack_back, true);
                        int cnt = 0;
                        if(!is_empty(i, 1)){
                            Node *current = g_sm->sm_entry[i].receiver.buffer.front;
                            while (current != NULL) {
                                if (current->msg.seq_num == buf->seq_num) {
                                    cnt = -1;
                                    break;
                                }
                                current = current->next;
                            }
                        }
                        if(cnt==0){             //NOT DUPLICATE
                            if(!is_full(i, 1)){
                                ack_back->seq_num = buf->seq_num;
                                MTP_Queue *q = &g_sm->sm_entry[i].receiver.buffer;
                                Node* current = q->front;
                                if(q->count == 0)
                                    enqueue_recv(i, *ack_back, 1);
                                else{
                                    if(current->msg.seq_num > buf->seq_num){
                                        Node *temp = (Node*)malloc(sizeof(Node));
                                        temp->msg = *ack_back;
                                        temp->next = current;
                                        q->front = temp;
                                        q->count++;
                                    }
                                    else{
                                        while (current->next != NULL && current != NULL) {
                                            if(current->msg.seq_num < buf->seq_num && current->next->msg.seq_num > buf->seq_num){
                                                Node *temp = (Node*)malloc(sizeof(Node));
                                                temp->msg = *ack_back;
                                                temp->next = current->next;
                                                current->next = temp;
                                                q->count++;
                                                break;
                                            }
                                            current = current->next;
                                        }
                                    }                                    
                                }
                                current = q->front;
                                while (current->next!=NULL && current != NULL) {
                                    if (current->next->msg.seq_num != current->msg.seq_num + 1) {
                                        break;
                                    }
                                    current = current->next;
                                }
                                ack_back->wnd_sz = q->count;
                                ack_back->next_val = (current->next->msg.seq_num + 1)%MAX_SEQ_NUM;

                            }
                        }
                        else{
                            MTP_Queue *q = &g_sm->sm_entry[i].receiver.buffer;
                            ack_back->seq_num = buf->seq_num;
                            ack_back->wnd_sz = q->count;
                            Node *current = q->front;
                            while (current->next!=NULL && current != NULL) {
                                if (current->next->msg.seq_num != current->msg.seq_num + 1) {
                                    break;
                                }
                                current = current->next;
                            }
                            ack_back->next_val = (current->next->msg.seq_num + 1)%MAX_SEQ_NUM;
                        }
                        int mn = sendto(g_sm->sm_entry[i].sock.udp_sockfd, ack_back, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        if (mn < 0) {
                            perror("sendto failed");
                            free(ack_back);
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                    }
                    else{    // HANDLE DUPLICATE MESSAGES
                        // Process other packet types
                        MTP_Message *msg = (MTP_Message *)buf;
                        uint16_t seq_num = msg->seq_num;
                        for(int j=0; j<SENDER_SWND; j++){
                            if(g_sm->sm_entry[i].sender.swnd[j].seq_num == seq_num){
                                g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
                                break;
                            }
                        }
                        struct sockaddr_in *from_in = (struct sockaddr_in *)&from;
                        printf("Received packet from %s:%d\n", inet_ntoa(from_in->sin_addr), ntohs(from_in->sin_port));
                    }
                }
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            }
        }
    }
    return NULL;
}

void* sender_thread(void *arg) {
    printf("Sender thread started\n");
    // loop: check send buffer, sendto() as needed, handle retransmission
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
   
    while (1) {
        sleep(T/2);     

        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }

            MTP_Sender *sender = &g_sm->sm_entry[i].sender;
            int retransmit = 0;
            // Check sliding window, send packets
            for (int j = 0; j < SENDER_SWND; ++j) {
                if (sender->swnd[j].sent_time.tv_sec  != default_time.tv_sec || sender->swnd[j].sent_time.tv_nsec != default_time.tv_nsec) {
                    // Handle retransmission
                    long ms = (now.tv_sec - sender->swnd[j].sent_time.tv_sec) * 1000
                            + (now.tv_nsec - sender->swnd[j].sent_time.tv_nsec) / 1000000;

                    float elapsed = (float)ms / 1000.0f;  
                    printf("elapsed %f\n", elapsed);
                    if(elapsed >= T) {
                        retransmit = 1;
                        break;
                    }
                }
            }
            if(retransmit) {
                // Retransmit packet
                for (int j=0; j<SENDER_SWND; j++) {
                    if (sender->swnd[j].sent_time.tv_sec  != default_time.tv_sec || sender->swnd[j].sent_time.tv_nsec != default_time.tv_nsec) {
                        printf("Retransmitting packet %d\n", sender->swnd[j].seq_num);
                        sendto(g_sm->sm_entry[i].sock.udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        sender->swnd[j].sent_time = now; // Update sent time
                    }
                }
            }
            int j = 0;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            while(j<SENDER_SWND && !is_empty(i, 0)) {
                // Send new packet
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                // printf("COUNT FOR THIS%d \n", g_sm->sm_entry[i].sender.buffer.front->msg.seq_num);
                if((sender->swnd[j].sent_time.tv_sec  == default_time.tv_sec && sender->swnd[j].sent_time.tv_nsec == default_time.tv_nsec)){
                    MTP_Message *buf = (MTP_Message *)malloc(sizeof(MTP_Message));
                    
                    if (!buf) {
                        perror("Failed to allocate memory");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        continue;
                    }

                    int rv = dequeue_recv(i, buf, 0);
                    if (rv < 0) {
                        perror("Failed to dequeue message");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        continue;
                    }
                    printf("4 \n");
                    buf->sent_time = now;
                    printf("5 \n");

                    sender->swnd[j] = *buf;
                    printf("6 \n");

                    sender->swnd_count++;
                    printf("7 \n");

                    // printf("Sending new packet %d\n", sender->next_seq);
                    sendto(g_sm->sm_entry[i].sock.udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                    printf("8 \n");

                }
                j++;
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
    }
    return NULL;
}

void* garbage_collector_thread(void *arg) {
    printf("Garbage collector thread started\n");
    // loop: clean up old sockets, free memory
    while (1) {
        // check for closed sockets, free resources
        sleep(5); // simulate work
    }
    return NULL;
}



