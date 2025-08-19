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
    // shmdt(g_sm);
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
    // loop: recvfrom() on UDP socket, update rwnd
    fd_set rfds;
    FD_ZERO(&rfds);
    struct timeval tv;
    tv.tv_sec = TIME_SEC;
    tv.tv_usec = TIME_USEC;
    int maxfd = -1;
    while (1) {
        // blocking receive, e.g. recvfrom(sock, buf, ...);
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            if (g_sm->sm_entry[i].free_slot) continue;
            int fd = g_sm->sm_entry[i].udp_sockfd;
            if (fd >= 0) {
                FD_SET(fd, &rfds);
                if (fd > maxfd) maxfd = fd + 1;
            }
        }
        int rv = select(maxfd, &rfds, NULL, NULL, &tv);
        if (rv < 0) {
            perror("select failed");
            break;
        }
        if (rv == 0) {
            printf("Timeout occurred! No data after %d seconds.\n", TIME_SEC);
            continue;
        }
        if(rv > 0){
            for (int i = 0; i < MAX_SOCKETS; ++i) {
                if (g_sm->sm_entry[i].free_slot) continue;
                int fd = g_sm->sm_entry[i].udp_sockfd;
                if (FD_ISSET(fd, &rfds)) {
                    // Data is available to read
                    char buf[MTP_MSG_SIZE];
                    struct sockaddr_in from;
                    socklen_t fromlen = sizeof(from);
                    int n = m_recvfrom(fd, buf, sizeof(buf), 0, (struct sockaddr *)&from, &fromlen);
                    if (n < 0) {
                        perror("recvfrom failed");
                        continue;
                    }
                    if(buf[0] == 0x01) { // Check for specific packet type
                        // Process received packet
                        MTP_Message *msg = (MTP_Message *)buf;
                        int ret = enqueue_recv(&g_sm->sm_entry[i].receiver->buffer, *msg);
                        if(ret < 0) {
                            perror("enqueue_recv failed");
                            continue;
                        }
                        int m = m_sendto(g_sm->sm_entry[i].udp_sockfd, &g_sm->sm_entry[i].sender->buffer, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        printf("Received packet from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    }
                    else{
                        // Process other packet types
                        printf("Received unknown packet from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));
                    }
                }
            }
        }
        // process packet
    }
    return NULL;
}

void* sender_thread(void *arg) {
    printf("Sender thread started\n");
    // loop: check send buffer, sendto() as needed, handle retransmission

    while (1) {
        sleep(T/2);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (int i = 0; i < MAX_SOCKETS; ++i) {
            if (g_sm->sm_entry[i].free_slot) continue;
            MTP_Sender *sender = g_sm->sm_entry[i].sender;
            // Check sliding window, send packets
            for (int j = 0; j < SENDER_SWND; ++j) {
                if (sender->swnd[j].sent_time != -1) {
                    // Handle retransmission
                    float elapsed = float(long((now.tv_sec - sender->swnd[j].sent_time.tv_sec) * 1000 + (now.tv_nsec - sender->swnd[j].sent_time.tv_nsec) / 1000000) / 1000.0);
                    if(elapsed >= T) {
                        // Retransmit packet
                        printf("Retransmitting packet %d\n", sender->swnd[j].seq_num);
                        m_sendto(g_sm->sm_entry[i].udp_sockfd, &sender->buffer[sender->swnd[j].seq_num], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                        sender->swnd[j].sent_time = now; // Update sent time
                    }

                }
            }
            if(sender->swnd_count != SENDER_SWND) {
                // Send new packet
                printf("Sending new packet %d\n", sender->next_seq);
                m_sendto(g_sm->sm_entry[i].udp_sockfd, &sender->buffer[sender->next_seq], sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].dest_addr, sizeof(g_sm->sm_entry[i].dest_addr));
                sender->swnd[sender->next_seq].sent_time = now; // Update sent time
                sender->swnd_count++;
            }
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

