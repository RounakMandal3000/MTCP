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
        g_sm->sm_entry[i].receiver.next_val = 1;
    }
    
    pthread_t tid_R, tid_S;

    if (pthread_create(&tid_R, NULL, receiver_thread, NULL) != 0) {
        perror("Failed to create receiver thread");
        exit(1);
    }

    if (pthread_create(&tid_S, NULL, sender_thread, NULL) != 0) {
        perror("Failed to create sender thread");
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
    return 0;
}



void* receiver_thread(void *arg) {
    printf("Receiver thread started\n");
    MTP_SM *g_sm = finder(SHM_KEY);
    // void *base = base_finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
    // loop: recvfrom() on UDP socket, update rwnd
    
    while (1) {
        // blocking receive, e.g. recvfrom(sock, buf, ...);
        // pthread_mutex_lock(&g_sm->lock_sm);
        // sleep(2);
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = -1;
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (!g_sm->sm_entry[i].free_slot){
                int fd = g_sm->sm_entry[i].sock.udp_sockfd;
                if (fd < 0) {
                    pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    continue;
                }
                if(fd>=0){
                    FD_SET(fd, &rfds);
                    if (fd > maxfd) maxfd = fd;
                }
                else{
                    printf("Skipping invalid fd=%d at slot=%d\n", fd, i);
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
        // printf("Data is available to read\n");

        for (int i = 0; i < MAX_SOCKETS; ++i) {
            // printf("%d ", i);
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            // printf("!!!!!!!__\n");
            int fd = g_sm->sm_entry[i].sock.udp_sockfd;
            if (FD_ISSET(fd, &rfds)) {
                // printf("%d \n", i);
                // Data is available to read
                for(int k=0; k<RECV_SWND; k++){

                    MTP_Message *buf = malloc(sizeof(MTP_Message));
                    struct sockaddr_in from;
                    socklen_t fromlen = sizeof(from);
                    // pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    int n = recvfrom(fd, buf, sizeof(MTP_Message), 0, (struct sockaddr *)&from, &fromlen);
                    
                    if (n <= 0) {
                        perror("recvfrom failed");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        break;
                    }
                    if(!buf->is_ack) { // Check for specific packet type
                        int diff = seq_num_finder(buf->seq_num, g_sm->r_ack[i]);
                        if(diff >= RECV_SWND){  // NO SPACE to hold  this message
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            free(buf);
                            continue;
                        }
                        MTP_Message *msg = (MTP_Message *)buf;
                        MTP_Message *ack_back = malloc(sizeof(MTP_Message)); initializer_message(ack_back, true);
                        ack_back->seq_num = msg->seq_num;
                        int duplicate_present = 0;
                        for(int p=0; p<RECV_BUFFER; p++){
                            if(g_sm->sm_entry[i].receiver.buffer[p].seq_num == msg->seq_num) {
                                duplicate_present = 1;
                                break;
                            }
                        }

                        int next_val_required = g_sm->r_ack[i];
                        // printf("SEQ NUM RECEIVED: %d \n", msg->seq_num);
                        if(!duplicate_present){
                            if(count_buffer(i, 1) == 0){
                                g_sm->sm_entry[i].receiver.buffer[0] = *msg;
                            }
                            else{
                                for(int k=0; k<RECV_BUFFER; k++){
                                    if(g_sm->sm_entry[i].receiver.buffer[k].seq_num == -1){
                                        g_sm->sm_entry[i].receiver.buffer[k] = *msg;
                                        break;
                                    }
                                    if(seq_num_finder(g_sm->sm_entry[i].receiver.buffer[k].seq_num, g_sm->r_ack[i]) > seq_num_finder(msg->seq_num, g_sm->r_ack[i])){
                                        for(int p=RECV_BUFFER-1; p>k; p--){
                                            g_sm->sm_entry[i].receiver.buffer[p] = g_sm->sm_entry[i].receiver.buffer[p-1];
                                        }
                                        g_sm->sm_entry[i].receiver.buffer[k] = *msg;
                                    }
                                }
                            }
                        }
                        
                        // THIS PART IS FOR FINDING THE NEXT SEQ NUMBER REQUIRED
                        if(g_sm->sm_entry[i].receiver.buffer[0].seq_num == g_sm->r_ack[i]){
                            for(int p=0; p<RECV_BUFFER; p++){
                                if(g_sm->sm_entry[i].receiver.buffer[p].seq_num != -1){
                                    if(p+1 < RECV_BUFFER && g_sm->sm_entry[i].receiver.buffer[p+1].seq_num == -1) {
                                        next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num+1)%MAX_SEQ_NUM;
                                        break;
                                    }
                                    if(p+1 <RECV_BUFFER && g_sm->sm_entry[i].receiver.buffer[p+1].seq_num != (g_sm->sm_entry[i].receiver.buffer[p].seq_num+1)%MAX_SEQ_NUM){
                                        next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num+1)%MAX_SEQ_NUM;
                                        break;
                                    }
                                    next_val_required = (g_sm->sm_entry[i].receiver.buffer[p].seq_num+1)%MAX_SEQ_NUM;
                                }
                                else break;
                            }
                        }
                        ack_back->next_val = next_val_required;
                        printf("ACK_BACK: %d\n", ack_back->next_val);
                        int mn = sendto(g_sm->sm_entry[i].sock.udp_sockfd, ack_back, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        if (mn < 0) {
                            perror("sendto failed");
                            free(ack_back);
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                        // printf("SENT ACK: %d\n", ack_back->seq_num);
                        // for(int k=0;k<RECV_BUFFER;k++){
                        //     printf("%d ", g_sm->sm_entry[i].receiver.buffer[k].seq_num);
                        // }
                        // printf("\n");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        
                        // g_sm->r_ack[i] = next_val_required;
                    }
                
                    else{    // HANDLE ACK MESSAGES
                        MTP_Message *msg = (MTP_Message *)buf;
                        printf("In ack handling, next val %d %d %d\n", msg->next_val, msg->seq_num, g_sm->sm_entry[i].sender.swnd_count);
                        for(int j=0; j<SENDER_SWND; j++){
                            // printf("SEQ NUMS %d %d\n", g_sm->sm_entry[i].sender.swnd[j].seq_num, seq_num_finder(g_sm->sm_entry[i].sender.swnd[j].seq_num, msg->next_val) );
                            if(g_sm->sm_entry[i].sender.swnd[j].seq_num!=-1 && seq_num_finder(g_sm->sm_entry[i].sender.swnd[j].seq_num, msg->next_val) >= SENDER_SWND && g_sm->sm_entry[i].sender.swnd_count>0){
                                g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
                                g_sm->sm_entry[i].sender.swnd_count -= 1;
                                g_sm->sm_entry[i].sender.swnd[j].seq_num = -1;
                                break;
                                // printf("FREED A SENDER WINDOW ENTRY\n");
                            }
                        }
                        // printf("\n");
                    }
                }
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
        // printf("\n");
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
        sleep(T/2);     

        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);

            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }

            // MTP_Sender *sender = &g_sm->sm_entry[i].sender;
            // printf("hewllo"); 
            int retransmit = 0;
            // Check sliding window, send packets
            for (int j = 0; j < SENDER_SWND; ++j) {
                if (g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec  != default_time.tv_sec || g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec != default_time.tv_nsec) {
                    // Handle retransmission
                    long ms = (now.tv_sec - g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec) * 1000
                            + (now.tv_nsec - g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec) / 1000000;

                    float elapsed = (float)ms / 1000.0f;  
                    if(elapsed >= T) {
                        retransmit = 1;
                        break;
                    }
                }
            }
            if(retransmit) {
                // Retransmit packet
                for (int j=0; j<SENDER_SWND; j++) {
                    if (g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec  != default_time.tv_sec || g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec != default_time.tv_nsec) {
                        sendto(g_sm->sm_entry[i].sock.udp_sockfd, &g_sm->sm_entry[i].sender.swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        g_sm->sm_entry[i].sender.swnd[j].sent_time = now; // Update sent time
                    }
                }
                printf("retransmitting");
            }
            int j = g_sm->sm_entry[i].sender.swnd_count;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            while(j<SENDER_SWND && count_buffer(i, 0)>0) {
                // Send new packet
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                
                if((g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_sec  == default_time.tv_sec && g_sm->sm_entry[i].sender.swnd[j].sent_time.tv_nsec == default_time.tv_nsec)){
                    MTP_Message buf;
                    memset(&buf, 0, sizeof(buf));
                    // int rv = dequeue_recv(i, &buf, 0);
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
                
                    // printf("index %d\n", j);
                    g_sm->sm_entry[i].sender.swnd[j] = buf;
                    g_sm->sm_entry[i].sender.swnd_count++;
                    // g_sm->sm_entry[i].sender.buffer[0] = store_buf;
                    printf("Sender window first entry: %d\n", g_sm->sm_entry[i].sender.buffer[0].seq_num);
                    printf("Sending new packet %d New sender window: %d\n", g_sm->sm_entry[i].sender.swnd[j].seq_num, g_sm->sm_entry[i].sender.swnd_count);
                    sendto(g_sm->sm_entry[i].sock.udp_sockfd, &g_sm->sm_entry[i].sender.swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                }
                j++;
                printf("SENDER BUFFER: ");
                for(int k=0;k<SENDER_BUFFER;k++){
                        printf("%d ", g_sm->sm_entry[i].sender.buffer[k].seq_num);
                }
                printf("\n");
                printf("SENDER WINDOW: ");
                for(int k=0;k<SENDER_SWND;k++){
                    printf("%d ", g_sm->sm_entry[i].sender.swnd[k].seq_num);
                }
                printf("\n");
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


