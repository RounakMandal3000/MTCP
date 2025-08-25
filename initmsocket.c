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
        g_sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
        g_sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
        init_recv_queue(i, 0);
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

        init_recv_queue(i, 1);
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
    void *base = base_finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
    // loop: recvfrom() on UDP socket, update rwnd
    
    while (1) {
        // blocking receive, e.g. recvfrom(sock, buf, ...);
        // pthread_mutex_lock(&g_sm->lock_sm);
        sleep(2);
        fd_set rfds;
        FD_ZERO(&rfds);
        
        int maxfd = -1;
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            printf("i=%d\n", i);
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
        printf("SELECT WILL START\n");
        int rv = select(maxfd+1, &rfds, NULL, NULL, &tv);
        printf("SELECT WILL END \n");
        if (rv < 0) {
            perror("select failed");
            continue;
        }
        else if (rv == 0) {           // WRITE THIS LATER, TIMEOUT CASE
            printf("Timeout occurred! No data after %d seconds.\n", TIME_SEC);
            continue;
        }
        else if(rv > 0){
            printf("Data is available to read\n");

            for (int i = 0; i < MAX_SOCKETS; ++i) {
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                if (g_sm->sm_entry[i].free_slot) {
                    pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    continue;
                }
                int fd = g_sm->sm_entry[i].sock.udp_sockfd;
                if (FD_ISSET(fd, &rfds)) {
                    // Data is available to read
                    for(int k=0; k<RECV_SWND; k++){
                        MTP_Message *buf = malloc(sizeof(MTP_Message));
                        // struct sockaddr_in from = g_sm->sm_entry[i].dest_addr;
                        struct sockaddr_in from;
                        socklen_t fromlen = sizeof(from);
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        int n = recvfrom(fd, buf, sizeof(MTP_Message), 0, (struct sockaddr *)&from, &fromlen);
                        
                        if (n <= 0) {
                            perror("recvfrom failed");
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            break;
                        }
                        if(!buf->is_ack) { // Check for specific packet type
                            int diff = seq_num_finder(buf->seq_num, g_sm->r_ack[i]);
                            if(diff >= RECV_SWND){
                                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                                continue;
                            }
                            // Process received packet
                            printf("RECEIVED PACKET SEQ NUM: %d\n", buf->seq_num);
                            MTP_Message *msg = (MTP_Message *)buf;
                            if(g_sm->sm_entry[i].receiver.rwnd_count == RECV_SWND){
                                perror("Receiver window is full");
                                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                                continue;
                            }
                            // int ret = enqueue_recv(i, *msg, 1);
                            // if(ret < 0) {
                            //     perror("enqueue_recv failed");
                            //     pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            //     continue;
                            // }
                            MTP_Message *ack_back = malloc(sizeof(MTP_Message));
                            initializer_message(ack_back, true);
                            int cnt = 0;
                            if(!is_empty(i, 1)){
                                Node *current = get_node(base, g_sm->sm_entry[i].receiver.buffer.front);
                                while (current != 0) {
                                    printf("SEQUENCE NUMBER: %d ", current->msg.seq_num);
                                    // current = current->next;
                                    if (current->next == 0){
                                        current = 0;
                                    }
                                    else
                                        current = get_node(base, current->next);    
                                }
                            }
                            printf("\n");
                            if(!is_empty(i, 1)){
                                Node *current = get_node(base, g_sm->sm_entry[i].receiver.buffer.front);
                                while (current != 0) {
                                    if (current->msg.seq_num == msg->seq_num) {
                                        cnt = -1;
                                        break;
                                    }
                                    // current = current->next;
                                    if (current->next == 0){
                                        current = 0;
                                    }
                                    else
                                        current = get_node(base, current->next);    
                                }
                            }
                            printf("COUNT   %d \n", cnt);
                            if(cnt==0){      
                                printf("1\n");
                                if(!is_full(i, 1)){
                                    ack_back->seq_num = buf->seq_num;
                                    MTP_Queue *q = &g_sm->sm_entry[i].receiver.buffer;
                                    Node* current = get_node(base, q->front);
                                    printf("2\n");
                                    if(q->count == 0){
                                        // enqueue_recv(i, *ack_back, 1);
                                        enqueue_recv(i, *msg, 1);
                                    }
                                    else{
                                        if(seq_num_finder(current->msg.seq_num, g_sm->r_ack[i]) > seq_num_finder(buf->seq_num, g_sm->r_ack[i])){
                                            Node *temp = (Node*)malloc(sizeof(Node));
                                            // temp->msg = *ack_back;
                                            temp->msg = *msg;
                                            temp->next = OFFSET(base, current);
                                            q->front = OFFSET(base, temp);
                                            q->count++;
                                            printf("3\n");
                                        }
                                        else{
                                            while (current != 0) {
                                                if((seq_num_finder(current->msg.seq_num, g_sm->r_ack[i]) < seq_num_finder(buf->seq_num, g_sm->r_ack[i])) && (current->next == 0 || seq_num_finder(get_node(base, current->next)->msg.seq_num, g_sm->r_ack[i]) > seq_num_finder(buf->seq_num, g_sm->r_ack[i]))){
                                                    Node *temp = (Node*)malloc(sizeof(Node));
                                                    // temp->msg = *ack_back;
                                                    temp->msg = *msg;
                                                    temp->next = current->next;
                                                    current->next = OFFSET(base, temp);
                                                    q->count++;
                                                    break;
                                                }
                                                if (current->next == 0)
                                                    current = 0;
                                                else
                                                    current = get_node(base, current->next);
                                            }
                                            printf("4\n");
                                        }                                    
                                    }
                                    printf("5\n");
                                    current = get_node(base, q->front);
                                    printf("COUNT OF NODES%d \n", q->count);
                                    if(q->count!=1){
                                        while (current->next!=0 && current != 0) {
                                            if (get_node(base, current->next)->msg.seq_num != (current->msg.seq_num + 1)%MAX_SEQ_NUM) {
                                                break;
                                            }
                                            if (current->next == 0)
                                                current = 0;
                                            else
                                                current = get_node(base, current->next);
                                        }
                                        ack_back->next_val = (get_node(base, current->next)->msg.seq_num + 1)%MAX_SEQ_NUM;
                                    }
                                    else{
                                        ack_back->next_val = (get_node(base, current->next)->msg.seq_num + 1)%MAX_SEQ_NUM;
                                        printf("hwlrwkjr\n");
                                    }
                                    ack_back->wnd_sz = q->count;

                                }
                                print_queue(i, 1);
                            }
                            else{
                                MTP_Queue *q = &g_sm->sm_entry[i].receiver.buffer;
                                ack_back->seq_num = buf->seq_num;
                                ack_back->wnd_sz = q->count;
                                Node *current = get_node(base, q->front);
                                while (current->next!=0 && current != 0) {
                                    if (get_node(base, current->next)->msg.seq_num != (current->msg.seq_num + 1)%MAX_SEQ_NUM) {
                                        break;
                                    }
                                    current = get_node(base, current->next);
                                }
                                ack_back->next_val = (get_node(base, current->next)->msg.seq_num + 1)%MAX_SEQ_NUM;
                            }
                            g_sm->r_ack[i] = ack_back->next_val;
                            int mn = sendto(g_sm->sm_entry[i].sock.udp_sockfd, ack_back, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));

                            if (mn < 0) {
                                perror("sendto failed");
                                free(ack_back);
                                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                                continue;
                            }
                            print_queue(i, 1);
                            printf("SENT ACK\n");
                        }
                        else{    // HANDLE ACK MESSAGES
                            // Process other packet types
                            MTP_Message *msg = (MTP_Message *)buf;
                            int cur_ack = msg->next_val;
                            printf("RECEIVED ACK PACKET SEQ NUM and NEXT VALUE NEEDED: %d %d\n", cur_ack, msg->next_val);
                            for(int j=0; j<SENDER_SWND; j++){
                                if(seq_num_finder(g_sm->sm_entry[i].sender.swnd[j].seq_num, msg->next_val) >= MAX_SEQ_NUM){
                                    g_sm->sm_entry[i].sender.swnd[j].sent_time = default_time;
                                }
                            }
                        }
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
                    // printf("elapsed %f\n", elapsed);
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


                        // struct sockaddr_in server_addr;
                        // memset(&server_addr, 0, sizeof(server_addr));
                        // server_addr.sin_family = AF_INET;
                        // server_addr.sin_port = htons(SOCK_PORT);
                        // server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
                        // int sockfd_temp = socket(AF_INET, SOCK_DGRAM, 0);
                        // if (sockfd_temp < 0) {
                        //     perror("socket creation failed");
                        //     continue;
                        // }
                        // if(bind(sockfd_temp, (struct sockaddr *)&server_addr, sizeof(server_addr))<0){
                        //     perror("bind failed ");
                        //     continue;
                        // }

                       
                        sendto(g_sm->sm_entry[i].sock.udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        // sendto(sockfd_temp, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));



                        // sendto(g_sm->sm_entry[i].sock.udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        // printf("SENDER SIDE src_ip: %s, src_port: %d, dest_ip: %s, dest_port: %d\n",
                        //     inet_ntoa(g_sm->sm_entry[i].src_addr.sin_addr),
                        //     ntohs(g_sm->sm_entry[i].src_addr.sin_port),
                        //     inet_ntoa(g_sm->sm_entry[i].dest_addr.sin_addr),
                        //     ntohs(g_sm->sm_entry[i].dest_addr.sin_port));
                        sender->swnd[j].sent_time = now; // Update sent time
                        // close(sockfd_temp);
                    }
                }
            }
            int j = sender->swnd_count;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
            while(j<SENDER_SWND && !is_empty(i, 0)) {
                // Send new packet
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                // printf("COUNT FOR THIS%d \n", g_sm->sm_entry[i].sender.buffer.front->msg.seq_num);
                if((sender->swnd[j].sent_time.tv_sec  == default_time.tv_sec && sender->swnd[j].sent_time.tv_nsec == default_time.tv_nsec)){
                    MTP_Message buf;
                    memset(&buf, 0, sizeof(buf));
                    int rv = dequeue_recv(i, &buf, 0);
                    if (rv < 0) {
                        perror("Failed to dequeue message");
                        pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                        continue;
                    }
                    buf.sent_time = now;
                    sender->swnd[j] = buf;
                    sender->swnd_count++;




                    // struct sockaddr_in server_addr;
                    // memset(&server_addr, 0, sizeof(server_addr));
                    // server_addr.sin_family = AF_INET;
                    // server_addr.sin_port = htons(SOCK_PORT);
                    // server_addr.sin_addr.s_addr = inet_addr(SERVER_IP);
                    // int sockfd_temp = socket(AF_INET, SOCK_DGRAM, 0);
                    // if (sockfd_temp < 0) {
                    //     perror("socket creation failed");
                    //     continue;
                    // }
                    // if(bind(sockfd_temp, (struct sockaddr *)&server_addr, sizeof(server_addr))<0){
                    //         perror("bind failed uwuu");
                    // }

                    printf("Sending new packet %d\n", sender->swnd[j].seq_num);




                    // sendto(sockfd_temp, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                    sendto(g_sm->sm_entry[i].sock.udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));

                    // printf("SENDER SIDE src_ip: %s, src_port: %d, dest_ip: %s, dest_port: %d\n",
                        // inet_ntoa(g_sm->sm_entry[i].src_addr.sin_addr),
                        // ntohs(g_sm->sm_entry[i].src_addr.sin_port),
                        // inet_ntoa(g_sm->sm_entry[i].dest_addr.sin_addr),
                        // ntohs(g_sm->sm_entry[i].dest_addr.sin_port));
                    // close(sockfd_temp);
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



