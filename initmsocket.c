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


void* receiver_thread(void *arg);
void* sender_thread(void *arg);
void* garbage_collector_thread(void *arg);
int socket_function(pid_t pid);

void initializer_message(MTP_Message* msg, bool is_ack=false){
    msg->seq_num = -1;  // or 0 depending on design
    msg->wnd_size = -1; // if you use a bool
    msg->is_ack = is_ack;
    msg->next_val = -1;
    msg->data = malloc(is_ack ? 0 : MTP_MSG_SIZE);
    msg->sent_time = -1;
}

int main(){
    int shmid;
    MTP_SM *g_sm;

    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0666 | IPC_CREAT);
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
    g_sm->sm_entry = malloc(sizeof(MTP_Entry) * MAX_SOCKETS);
    if (g_sm->sm_entry == NULL) {
        perror("malloc failed");
        exit(1);
    }
    for(int i=0;i<MAX_SOCKETS;i++){
        g_sm->sm_entry[i].lock = (pthread_mutex_t)PTHREAD_MUTEX_INITIALIZER;
        g_sm->sm_entry[i].sock = malloc(sizeof(MTP_socket));
        g_sm->sm_entry[i].sock->udp_sockfd = -1;
        g_sm->sm_entry[i].free_slot = true;
        g_sm->sm_entry[i].pid_creation = -1;
        g_sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
        g_sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
        g_sm->sm_entry[i].sender = malloc(sizeof(MTP_Sender));
        init_recv_queue(&g_sm->sm_entry[i].sender->buffer);
        g_sm->sm_entry[i].sender->swnd_count = 0;
        for (int j = 0; j < SENDER_SWND; j++) {
            g_sm->sm_entry[i].sender->swnd[j].seq_num = -1;  // or 0 depending on design
            g_sm->sm_entry[i].sender->swnd[j].wnd_size = -1; // if you use a bool
            g_sm->sm_entry[i].sender->swnd[j].is_ack = false;
            g_sm->sm_entry[i].sender->swnd[j].next_val = -1;
            g_sm->sm_entry[i].sender->swnd[j].data = malloc(MTP_DATA_SIZE);
            g_sm->sm_entry[i].sender->swnd[j].sent_time = -1;            
        }

        g_sm->sm_entry[i].receiver = malloc(sizeof(MTP_Receiver));
        init_recv_queue(&g_sm->sm_entry[i].receiver->buffer);
        g_sm->sm_entry[i].receiver->rwnd_count = 0;
        g_sm->sm_entry[i].receiver->next_val = 1;
    }
    while(1){
        // Periodically check and clean up resources
        sleep(1);
    }
    shmdt(g_sm);
    return 0;
}

int socket_function(pid_t pid){
    pthread_t tid_R, tid_S, tid_garbage;
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
    if (pthread_create(&tid_garbage, NULL, garbage_collector_thread, NULL) != 0) {
        perror("Failed to create garbage collector thread");
        exit(1);
    }

    pthread_join(tid_R, NULL);
    pthread_join(tid_S, NULL);
    pthread_join(tid_garbage, NULL);

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
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if (g_sm->sm_entry[i].free_slot){
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            int fd = g_sm->sm_entry[i].udp_sockfd;
            FD_SET(fd, &rfds);
            if (fd > maxfd) maxfd = fd + 1;
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
        // pthread_mutex_unlock(&g_sm->lock_sm);
        int rv = select(maxfd, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("select failed");
            break;
        }
        if (rv == 0) {                  // WRITE THIS LATER, TIMEOUT CASE
            printf("Timeout occurred! No data after %d seconds.\n", TIME_SEC);
            continue;
        }
        if(rv > 0){
            // pthread_mutex_lock(&g_sm->lock_sm);
            for (int i = 0; i < MAX_SOCKETS; ++i) {
                pthread_mutex_lock(&g_sm->sm_entry[i].lock);
                if (g_sm->sm_entry[i].free_slot) {
                    pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                    continue;
                }
                int fd = g_sm->sm_entry[i].udp_sockfd;
                if (FD_ISSET(fd, &rfds)) {
                    // Data is available to read
                    MTP_Message *buf = malloc(sizeof(MTP_Message));
                    struct sockaddr_in from = g_sm->sm_entry[i].sock->dest_addr;
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
                        if(g_sm->sm_entry[i].receiver->rwnd_count == RECV_SWND){
                            perror("Receiver window is full");
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                        int ret = enqueue_recv(&g_sm->sm_entry[i].receiver->buffer, *msg);
                        if(ret < 0) {
                            perror("enqueue_recv failed");
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                        MTP_Message *ack_back = malloc(sizeof(MTP_Message));
                        initializer_message(ack_back, true);
                        int cnt = 0;
                        if(!is_empty(&g_sm->sm_entry[i].receiver->buffer)){
                            Node *current = g_sm->sm_entry[i].receiver->buffer.front;
                            while (current != NULL) {
                                if (current->msg.seq_num == buf->seq_num) {
                                    cnt = -1;
                                    break;
                                }
                                current = current->next;
                            }
                        }
                        if(cnt==0){             //NOT DUPLICATE
                            if(!is_full(&g_sm->sm_entry[i].receiver->buffer)){
                                ack_back->seq_num = buf->seq_num;
                                MTP_Queue *q = &g_sm->sm_entry[i].receiver->buffer;
                                Node *current = q->front;
                                if(q->count == 0)
                                    enqueue_recv(q, *ack_back);
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
                                Node *current = q->front;
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
                        int mn = sendto(g_sm->sm_entry[i].udp_sockfd, ack_back, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        if (mn < 0) {
                            perror("sendto failed");
                            free(ack_back);
                            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                            continue;
                        }
                    }
                    else{
                        // Process other packet types
                        MTP_Message *msg = (MTP_Message *)buf;
                        uint16_t seq_num = msg->seq_num;
                        for(int j=0; j<SENDER_SWND; j++){
                            if(g_sm->sm_entry[i].sender->swnd[j].seq_num == seq_num){
                                g_sm->sm_entry[i].sender->swnd[j].sent_time = -1;
                                break;
                            }
                        }
                        
                        printf("Received unknown packet from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    }
                }
            }
        }
        // process packet
        // pthread_mutex_unlock(&g_sm->lock_sm);
    }
    return NULL;
}

void* sender_thread(void *arg) {
    printf("Sender thread started\n");
    // loop: check send buffer, sendto() as needed, handle retransmission
    MTP_SM *g_sm = finder();
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return NULL;
    }
    while (1) {
        sleep(T/2);
        
        // pthread_mutex_lock(&g_sm->lock_sm);
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (g_sm->sm_entry[i].free_slot) {
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            MTP_Sender *sender = g_sm->sm_entry[i].sender;
            int retransmit = 0;
            // Check sliding window, send packets
            for (int j = 0; j < SENDER_SWND; ++j) {
                if (sender->swnd[j].sent_time != -1) {
                    // Handle retransmission
                    float elapsed = float(long((now.tv_sec - sender->swnd[j].sent_time.tv_sec) * 1000 + (now.tv_nsec - sender->swnd[j].sent_time.tv_nsec) / 1000000) / 1000.0);
                    if(elapsed >= T) {
                        retransmit = 1;
                        break;
                    }
                }
            }
            if(retransmit) {
                // Retransmit packet
                for (int j=0; j<SENDER_SWND; j++) {
                    if (sender->swnd[j].sent_time != -1) {
                        printf("Retransmitting packet %d\n", sender->swnd[j].seq_num);
                        sendto(g_sm->sm_entry[i].udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        sender->swnd[j].sent_time = now; // Update sent time
                    }
                }
            }
            int j = 0;
            while(sender->swnd_count != SENDER_SWND && is_empty(&sender->buffer)) {
                // Send new packet
                if(sender->swnd[j].sent_time == -1){
                    Message* buf = dequeue_recv(&sender->buffer, buf);
                    sender->swnd[j] = *buf;
                    sender->swnd_count++;
                    sender->swnd[j].sent_time = now;
                    printf("Sending new packet %d\n", sender->next_seq);
                    sendto(g_sm->sm_entry[i].udp_sockfd, &sender->swnd[j], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                }
                j++;
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
        // pthread_mutex_unlock(&g_sm->lock_sm);
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

void* reader_buffer_receiver(void* arg){
    while(1){
        sleep(T/2);
        pthread_mutex_lock(&g_sm->lock_sm);
        for(int i=0;i<MAX_SOCKETS;i++){ 
            pthread_mutex_lock(&g_sm->sm_entry[i].lock);
            if(g_sm->sm_entry[i].receiver->rwnd_count==0){
                pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
                continue;
            }
            Message* msg_buf = &(g_sm->sm_entry[i].receiver->rwnd[0]);
            uint_t val = msg_buf->seq_num;
            int rv = enqueue_recv(g_sm->sm_entry[i].receiver->buffer, msg_buf);
            g_sm->sm_entry[i].receiver->rwnd_count--;
            int j=1;
            g_sm->sm_entry[i].receiver->rwnd[j-1] = 1e9;
            while(!is_full(g_sm->sm_entry[i].receiver->buffer) || j!= RECV_SWND){
                msg_buf = &g_sm->sm_entry[i].receiver->rwnd[++j];
                if(val+1 != msg_buf->seq_num){
                    break;
                }
                val = msg_buf->seq_num;                
                rv = enqueue_recv(g_sm->sm_entry[i].receiver->buffer, msg_buf);
                g_sm->sm_entry[i].receiver->rwnd_count--;
                g_sm->sm_entry[i].receiver->rwnd[j-1] = 1e9;
            }
            for(int j_=j-1; j_<RECV_SWND; j_++){
                swap(g_sm->sm_entry[i].receiver->rwnd[j_], g_sm->sm_entry[i].receiver->rwnd[j_-j+1]);
            }
            pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
        }
        pthread_mutex_unlock(&g_sm->lock_sm);
    }
}

void populate_sender_buffer(MTP_Sender *sender, string filename) {
    FILE *file = fopen(filename.c_str(), "r");
    if (!file) {
        perror("Failed to open file");
        return;
    }
    char line[MTP_MSG_SIZE];
    int i = sender->buffer->counter;
    while (fgets(line, sizeof(line), file) && i < SENDER_BUFFER_SIZE) {
        MTP_Message msg;
        strncpy(msg.data, line, MTP_MSG_SIZE);
        msg.seq_num = i; // CHANGE THIS
        msg.is_ack = false; // Not an ACK
        msg.length = strlen(msg.data);
        int rv = enqueue_recv(&sender->buffer, msg);
        if (rv < 0) {
            perror("Failed to enqueue message");
            break;
        }
        i++;
    }
    fclose(file);
}

void write_receiver_buffer(MTP_Receiver *receiver, pid_t pid) {
    char filename[256];
    snprintf(filename, sizeof(filename), "output_%d.txt", pid);
    FILE *file = fopen(filename, "w");
    if (!receiver || !file) return;
    // Write each message in the receiver buffer to the file
    Node *current = receiver->buffer.front;
    while (current) {
        fprintf(file, "Received packet %d: %s\n", current->msg.seq_num, current->msg.data);
        current = current->next;
    }
    fclose(file);
}
