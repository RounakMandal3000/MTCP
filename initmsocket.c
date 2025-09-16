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
#include <errno.h>
#include <signal.h>   


void print_fdset(fd_set *set, int maxfd) {
    printf("FD_SET contents: [");
    for (int fd = 0; fd < maxfd; fd++) {
        if (FD_ISSET(fd, set)) {
            printf(" %d", fd);
        }
    }
    printf(" ]\n");
}

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
    memset(msg->data, 0, sizeof(msg->data));
    msg->sent_time = default_time;
}

int seq_num_finder(int seq_num, int expected){
    return (seq_num - expected + MAX_SEQ_NUM) % MAX_SEQ_NUM;
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
    g_sm->bind_socket = -1;
    g_sm->count_occupied = 0;
    memset(g_sm->r_ack, 0, sizeof(g_sm->r_ack));
    g_sm->lock_sm = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
    for (int i = 0; i < MAX_NODES; i++) {
        g_sm->node_pool[i].used = 0;
    }
    
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
        g_sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
        g_sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
        
        for(int k=0;k<SENDER_BUFFER;k++){
            initializer_message(&(g_sm->sm_entry[i].sender.buffer[k]), false);
        }
        g_sm->sm_entry[i].sender.swnd_count = 0;
        for (int j = 0; j < SENDER_SWND; j++) {
            g_sm->sm_entry[i].sender.swnd[j].seq_num = -1;  // or 0 depending on design
            g_sm->sm_entry[i].sender.swnd[j].wnd_sz = -1; // if you use a bool
            g_sm->sm_entry[i].sender.swnd[j].is_ack = false;
            g_sm->sm_entry[i].sender.swnd[j].next_val = -1;
            memset(g_sm->sm_entry[i].sender.swnd[j].data, 0, sizeof(g_sm->sm_entry[i].sender.swnd[j].data));
            g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
            g_sm->sm_entry[i].to_bind = 0;
        }
        for(int k=0;k<RECV_BUFFER;k++){
            initializer_message(&(g_sm->sm_entry[i].receiver.buffer[k]), false);
        }
        memset(g_sm->sm_entry[i].receiver.whether_taken, 0, sizeof(g_sm->sm_entry[i].receiver.whether_taken));
        g_sm->sm_entry[i].receiver.rwnd_count = 0;
        g_sm->sm_entry[i].receiver.next_val = 0;
    }
    
    pthread_t tid_R, tid_S, tid_GC;

    if (pthread_create(&tid_R, NULL, receiver_thread, NULL) != 0) {
        perror("Failed to create receiver thread");
        exit(1);
    }

    if (pthread_create(&tid_S, NULL, sender_thread, NULL) != 0) {
        perror("Failed to create sender thread");
        exit(1);
    }
    if (pthread_create(&tid_GC, NULL, garbage_collector_thread, NULL) != 0) {
        perror("Failed to create garbage collector thread");
        exit(1);
    }
    while(1){
        pthread_mutex_lock(&g_sm->lock_sm);
        if(g_sm->bind_socket == 0){
            for(int i=0; i<MAX_SOCKETS; i++){
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                if(g_sm->sm_entry[i].sock.udp_sockfd == -2){
                    int sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
                    g_sm->sm_entry[i].sock.udp_sockfd = sockfd_;
                }
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            }
        }
        else if(g_sm->bind_socket == 1){
            for(int i=0; i<MAX_SOCKETS; i++){
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                if(!g_sm->sm_entry[i].free_slot && g_sm->sm_entry[i].to_bind == 1 ){
                    struct sockaddr_in server_addr = g_sm->sm_entry[i].src_addr;
                    if(bind(g_sm->sm_entry[i].sock.udp_sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0){
                        perror("Failed to bind socket");
                        close(g_sm->sm_entry[i].sock.udp_sockfd);
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    }
                }
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            }
        }
        pthread_mutex_unlock(&g_sm->lock_sm);
        sleep(1);
    }
    if (pthread_join(tid_S, NULL) != 0) {
        perror("Failed to join sender thread");
        exit(1);
    }
    if (pthread_join(tid_R, NULL) != 0) {
        perror("Failed to join receiver thread");
        exit(1);
    }
    if (pthread_join(tid_GC, NULL) != 0) {
        perror("Failed to join garbage collector thread");
        exit(1);
    }
    return 0;
}



void* receiver_thread(void *arg) {
    printf("Receiver thread started\n");
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
    
    while (1) {
        sleep(1);
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (!g_sm->sm_entry[i].free_slot){
                int fd = g_sm->sm_entry[i].sock.udp_sockfd;
                if (fd >= 0) {
                    FD_SET(fd, &rfds);
                    if (fd > maxfd) maxfd = fd;
                } else {
                    // invalid fd
                }
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
        
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 5;
        int rv = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("select failed");
            continue;
        }
        else if (rv == 0) {           // WRITE THIS LATER, TIMEOUT CASE
            printf("Timeout occurred! No data after %d seconds.\n", TIME_SEC);
            continue;
        }
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            // Lock briefly to check slot and fetch fd
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            int fd = g_sm->sm_entry[i].sock.udp_sockfd;
            struct sockaddr_in dest_copy = g_sm->sm_entry[i].dest_addr;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);

            if (FD_ISSET(fd, &rfds)) {
                // Drain all available datagrams without blocking
                while (1) {
                    MTP_Message buf;
                    struct sockaddr_in from;
                    socklen_t fromlen = sizeof(from);
                    int n = recvfrom(fd, &buf, sizeof(MTP_Message), MSG_DONTWAIT, (struct sockaddr *)&from, &fromlen);

                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            // No more data to read for this fd
                            break;
                        }
                        perror("recvfrom failed");
                        break;
                    }

                    if (dropMessage(DROP_PROB) == 1) {
                        printf("MESSAGE DROPPED %d, ACK %s\n", buf.seq_num, buf.is_ack ? "true" : "false");
                        continue; // simulate drop, do not process further
                    }
                    printf("Received message with seq_num: %d, is_ack: %s from %s\n", buf.seq_num, buf.is_ack ? "true" : "false", inet_ntoa(from.sin_addr));
                    if (!buf.is_ack) {
                        // Process data message
                        int diff;
                        int next_val_required;
                        // Lock around shared receiver buffer and r_ack
                        pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                        diff = seq_num_finder(buf.seq_num, g_sm->r_ack[i]);
                        if (diff >= RECV_SWND) {
                            // No space within receiver window
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }

                        MTP_Message msg = buf; // local copy
                        MTP_Message ack_back; initializer_message(&ack_back, true);
                        ack_back.seq_num = msg.seq_num;
                        int duplicate_present = 0;
                        for (int p = 0; p < RECV_BUFFER; p++) {
                            if (g_sm->sm_entry[i].receiver.buffer[p].seq_num == msg.seq_num) {
                                duplicate_present = 1;
                                break;
                            }
                        }

                        next_val_required = g_sm->r_ack[i];

                        if (!duplicate_present) {
                            if (count_buffer(i, 1) == 0) {
                                g_sm->sm_entry[i].receiver.buffer[0] = msg;
                            } else {
                                for (int kk = 0; kk < RECV_BUFFER; kk++) {
                                    if (g_sm->sm_entry[i].receiver.buffer[kk].seq_num == -1) {
                                        g_sm->sm_entry[i].receiver.buffer[kk] = msg;
                                        break;
                                    }
                                    if (seq_num_finder(g_sm->sm_entry[i].receiver.buffer[kk].seq_num, g_sm->r_ack[i]) >
                                        seq_num_finder(msg.seq_num, g_sm->r_ack[i])) {
                                        for (int p = RECV_BUFFER - 1; p > kk; p--) {
                                            g_sm->sm_entry[i].receiver.buffer[p] = g_sm->sm_entry[i].receiver.buffer[p - 1];
                                        }
                                        g_sm->sm_entry[i].receiver.buffer[kk] = msg;
                                        break; // inserted in order
                                    }
                                }
                            }
                        }


                        if (g_sm->sm_entry[i].receiver.buffer[0].seq_num == g_sm->r_ack[i]) {
                            for (int p = 0; p < RECV_BUFFER; p++) {
                                if (g_sm->sm_entry[i].receiver.buffer[p].seq_num != -1) {
                                    if (p + 1 < RECV_BUFFER && g_sm->sm_entry[i].receiver.buffer[p + 1].seq_num == -1) {
                                        next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num + 1) % MAX_SEQ_NUM;
                                        break;
                                    }
                                    if (p + 1 < RECV_BUFFER && g_sm->sm_entry[i].receiver.buffer[p + 1].seq_num !=
                                                                (g_sm->sm_entry[i].receiver.buffer[p].seq_num + 1) % MAX_SEQ_NUM) {
                                        next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num + 1) % MAX_SEQ_NUM;
                                        break;
                                    }
                                    next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num + 1) % MAX_SEQ_NUM;
                                } else {
                                    break;
                                }
                            }
                        }

                        ack_back.next_val = next_val_required;
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);

                        int mn = sendto(fd, &ack_back, sizeof(MTP_Message), 0,
                                        (struct sockaddr *)&dest_copy, sizeof(dest_copy));
                        if (mn < 0) {
                            perror("sendto failed");
                            continue;
                        }
                        printf("Sent acknowledgement with next seq required as: %d to %s\n", ack_back.next_val, inet_ntoa(dest_copy.sin_addr));
                    } else {
                        // HANDLE ACK MESSAGES
                        pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                        MTP_Message msg = buf;
                        printf("Received acknowledgement with next seq required as: %d from %s\n", msg.next_val, inet_ntoa(from.sin_addr));
                        for (int j = 0; j < SENDER_SWND; j++) {
                            if (g_sm->sm_entry[i].sender.swnd[j].seq_num != -1 &&
                                seq_num_finder(g_sm->sm_entry[i].sender.swnd[j].seq_num, msg.next_val) >= SENDER_SWND &&
                                g_sm->sm_entry[i].sender.swnd_count > 0) {
                                g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
                                g_sm->sm_entry[i].sender.swnd_count -= 1;
                                g_sm->sm_entry[i].sender.swnd[j].seq_num = -1;
                            }
                        }
                        order(g_sm->sm_entry[i].sender.swnd, SENDER_SWND);
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    }
                }
            }
        }
    }
    return NULL;
}

void* sender_thread(void *arg) {
    printf("Sender thread started\n");
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
   
    while (1) {
        sleep(T);     
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            for (int j = 0; j < SENDER_SWND; ++j) {
                if (g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec  != default_time.tv_sec || g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec != default_time.tv_nsec) {
                    // Handle retransmission
                    // printf("Transmit time %ld %ld\n", g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec, g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec);
                    long ms = (now.tv_sec - g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec) * 1000
                            + (now.tv_nsec - g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec) / 1000000;

                    float elapsed = (float)ms / 1000.0f;  
                    if(elapsed >= T) {
                        sendto(g_sm->sm_entry[i].sock.udp_sockfd, &g_sm->sm_entry[i].sender.swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        g_sm->sm_entry[i].sender.swnd[j].sent_time = now; 
                        printf("Retransmitting %d\n", g_sm->sm_entry[i].sender.swnd[j].seq_num);
                    }
                }
            }
            printf("\n");
           
            int j = g_sm->sm_entry[i].sender.swnd_count;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            
            while(j<SENDER_SWND && count_buffer(i, 0)>0) {
                // Send new packet
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                
                if((g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec  == default_time.tv_sec && g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec == default_time.tv_nsec)){
                    MTP_Message buf;
                    memset(&buf, 0, sizeof(buf));
                    int rv = 1;
                    buf = g_sm->sm_entry[i].sender.buffer[0];
                    
                    for(int k=1; k<SENDER_BUFFER; k++){
                        g_sm->sm_entry[i].sender.buffer[k-1] = g_sm->sm_entry[i].sender.buffer[k];
                    }
                    initializer_message(&(g_sm->sm_entry[i].sender.buffer[SENDER_BUFFER-1]), false);
                    
                    if (rv < 0) {
                        perror("Failed to dequeue message");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        continue;
                    }
                    buf.sent_time = now;
                
                    g_sm->sm_entry[i].sender.swnd[j] = buf;
                    g_sm->sm_entry[i].sender.swnd_count++;
                    sendto(g_sm->sm_entry[i].sock.udp_sockfd, &g_sm->sm_entry[i].sender.swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                    printf("Sending new packet %d\n", g_sm->sm_entry[i].sender.swnd[j].seq_num);
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
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory in GC");
        return NULL;
    }

    while (1) {
        sleep(3); // GC interval
        for (int i = 0; i < MAX_SOCKETS; i++) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (!g_sm->sm_entry[i].free_slot) {
                pid_t owner = g_sm->sm_entry[i].pid_creation;
                int fd = g_sm->sm_entry[i].sock.udp_sockfd;
                if (owner > 0) {
                    if (kill(owner, 0) == -1 && errno == ESRCH) {
                        printf("[GC] Reclaiming orphaned socket idx=%d (pid=%d) fd=%d\n", i, owner, fd);
                        if (fd >= 0) close(fd);
                        g_sm->sm_entry[i].sock.udp_sockfd = -1;
                        g_sm->sm_entry[i].free_slot = true;
                        g_sm->sm_entry[i].pid_creation = -1;
                        g_sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
                        g_sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
                        // Clear sender buffers
                        for (int k = 0; k < SENDER_BUFFER; k++) {
                            initializer_message(&g_sm->sm_entry[i].sender.buffer[k], false);
                        }
                        for (int k = 0; k < SENDER_SWND; k++) {
                            initializer_message(&g_sm->sm_entry[i].sender.swnd[k], false);
                        }
                        g_sm->sm_entry[i].sender.swnd_count = 0;
                        // Clear receiver buffers
                        for (int k = 0; k < RECV_BUFFER; k++) {
                            initializer_message(&g_sm->sm_entry[i].receiver.buffer[k], false);
                        }
                        memset(g_sm->sm_entry[i].receiver.whether_taken, 0, sizeof(g_sm->sm_entry[i].receiver.whether_taken));
                        g_sm->sm_entry[i].receiver.rwnd_count = 0;
                        g_sm->sm_entry[i].receiver.next_val = 0;
                        g_sm->r_ack[i] = 0; // reset expected ack for this slot
                    }
                }
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
    }
    return NULL;
}

