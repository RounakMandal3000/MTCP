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
    shmid = shmget(SHM_KEY, sizeof(MTP_SM), 0777);

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
    if(sm->count_occupied < MAX_SOCKETS){
        pthread_mutex_lock(&sm->lock_sm);
        sm->count_occupied++;
        pthread_mutex_unlock(&sm->lock_sm);

        for(int i = 0; i < MAX_SOCKETS; i++){
            pthread_mutex_lock(&(sm->sm_entry[i].lock));
            if(sm->sm_entry[i].free_slot){
                sm->sm_entry[i].pid_creation = getpid();
                sm->sm_entry[i].sock.udp_sockfd = -2;
                MTP_LOG_INFO("socket slot=%d reserved (pid=%d)", i, (int)sm->sm_entry[i].pid_creation);
                pthread_mutex_lock(&sm->lock_sm);
                sm->bind_socket = 0;
                pthread_mutex_unlock(&sm->lock_sm);

                pthread_mutex_unlock(&sm->sm_entry[i].lock);

                sleep(1);

                pthread_mutex_lock(&sm->sm_entry[i].lock);
                if(sm->sm_entry[i].sock.udp_sockfd < 0){
                    MTP_LOG_ERR("socket creation failed: %s", strerror(errno));
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
                MTP_LOG_INFO("socket fd=%d created in slot=%d", sm->sm_entry[i].sock.udp_sockfd, i);
                return sm->sm_entry[i].sock.udp_sockfd;
            }

            pthread_mutex_unlock(&sm->sm_entry[i].lock);
        }
    }
    else{
        errno = ENOBUFS;
        return -1;
    }
    return 0;
}

int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen){
    MTP_SM* sm = finder(SHM_KEY);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(sm->sm_entry[i].sock.udp_sockfd == sockfd){
            sm->sm_entry[i].src_addr = *(struct sockaddr_in *)src_addr;
            sm->sm_entry[i].dest_addr = *(struct sockaddr_in *)dest_addr;
            
            pthread_mutex_lock(&sm->lock_sm);
            sm->bind_socket = 1;
            sm->sm_entry[i].to_bind = 1;
            pthread_mutex_unlock(&sm->lock_sm);

            pthread_mutex_unlock(&sm->sm_entry[i].lock);

            sleep(1);

            pthread_mutex_lock(&sm->lock_sm);
            sm->bind_socket = -1;
            sm->sm_entry[i].to_bind = 0;
            pthread_mutex_unlock(&sm->lock_sm);

            return 0;
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    errno = EBADF;
    return -1;
}

int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen){
    MTP_SM* sm = finder(SHM_KEY);
    for(int i=0; i<MAX_SOCKETS; i++){
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(memcmp(&sm->sm_entry[i].dest_addr, to, sizeof(struct sockaddr)) == 0){
            if (count_buffer(i, 0) == SENDER_BUFFER) {
                MTP_LOG_WARN("send buffer full (slot=%d)", i);
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            int rv = -1;
            for(int k=0; k<SENDER_BUFFER; k++){
                if(sm->sm_entry[i].sender.buffer[k].seq_num == -1){
                    rv = 1;
                    sm->sm_entry[i].sender.buffer[k] = *(MTP_Message *)msg;
                    break;
                }
            }
            if (rv < 0) {
                errno = ENOBUFS;
                pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            sm->sm_entry[i].free_slot = false;
            mtp_print_buffer_sender(&sm->sm_entry[i].sender);
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
        // pthread_mutex_lock(&sm->sm_entry[i].lock);
        if(memcmp(&sm->sm_entry[i].dest_addr, from, sizeof(struct sockaddr_in)) == 0){
            if (count_buffer(i, 1) == 0) {
                errno = ENOMSG;
                // pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            if(sm->sm_entry[i].receiver.buffer[0].seq_num == -1){
                errno = ENOMSG;
                // pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            int rv = -1;
            if(sm->sm_entry[i].receiver.buffer[0].seq_num != sm->r_ack[i]){
                return rv;   
            }
            *(MTP_Message *) buf = sm->sm_entry[i].receiver.buffer[0];
            for(int k=0; k<RECV_BUFFER; k++){
                if(sm->sm_entry[i].receiver.buffer[k].seq_num != -1 ){
                    rv = 1;
                    *(MTP_Message *)buf = sm->sm_entry[i].receiver.buffer[k];
                    sm->sm_entry[i].receiver.buffer[k].seq_num = -1; // Mark as dequeued
                    sm->sm_entry[i].receiver.whether_taken[k] = 0;
                    break;
                }
            }
            
            for(int k=1; k<RECV_BUFFER; k++){
                sm->sm_entry[i].receiver.buffer[k-1] = sm->sm_entry[i].receiver.buffer[k];
            }
            
            sm->sm_entry[i].receiver.buffer[RECV_BUFFER-1].seq_num = -1;
            sm->r_ack[i]++;
            sm->r_ack[i] %= MAX_SEQ_NUM;
            MTP_LOG_DEBUG("recv deliver seq=%d next_expected=%d (slot=%d)", ((MTP_Message*)buf)->seq_num, sm->r_ack[i], i);

            if (rv < 0) {
                errno = ENOBUFS;
                // pthread_mutex_unlock(&sm->sm_entry[i].lock);
                return -1;
            }
            // pthread_mutex_unlock(&sm->sm_entry[i].lock);
            return rv;
        }
        // pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    errno = ENOTBOUND;  // HAVE SOME DOUBT HERE, SEE LATER
    return -1;
}

int m_close(int sockfd) {
    MTP_SM* sm = finder(SHM_KEY);
    int i;
    for (i = 0; i < MAX_SOCKETS; i++) {
        pthread_mutex_lock(&sm->sm_entry[i].lock);
        if (!sm->sm_entry[i].free_slot && sm->sm_entry[i].sock.udp_sockfd == sockfd) {
            pthread_mutex_unlock(&sm->sm_entry[i].lock);
            break;
        }
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
    }
    if (i == MAX_SOCKETS) {
        errno = EBADF; // invalid socket fd
        return -1;
    }
    if (close(sm->sm_entry[i].sock.udp_sockfd) < 0) {
    MTP_LOG_ERR("close failed fd=%d: %s", sm->sm_entry[i].sock.udp_sockfd, strerror(errno));
        pthread_mutex_unlock(&sm->sm_entry[i].lock);
        return -1;
    }
    sm->sm_entry[i].sock = (MTP_socket){0};
    sm->sm_entry[i].sock.udp_sockfd = -1;
    sm->sm_entry[i].free_slot = true;
    sm->sm_entry[i].pid_creation = -1;
    sm->sm_entry[i].src_addr = (struct sockaddr_in){0};
    sm->sm_entry[i].dest_addr = (struct sockaddr_in){0};
    memset(sm->sm_entry[i].sender.buffer, 0, sizeof(sm->sm_entry[i].sender.buffer));
    for(int k=0;k<SENDER_BUFFER;k++){
        sm->sm_entry[i].sender.buffer[k].seq_num = -1;
    }
    sm->sm_entry[i].sender.swnd_count = 0;
    memset(sm->sm_entry[i].sender.swnd, 0, sizeof(sm->sm_entry[i].sender.swnd));
    memset(sm->sm_entry[i].receiver.buffer, 0, sizeof(sm->sm_entry[i].receiver.buffer));
    for(int k=0;k<RECV_BUFFER;k++){
        sm->sm_entry[i].receiver.buffer[k].seq_num = -1;
    }
    sm->sm_entry[i].receiver.buffer->seq_num = -1;
    sm->sm_entry[i].receiver.rwnd_count = 0;
    sm->sm_entry[i].receiver.next_val = 1;
    memset(sm->sm_entry[i].receiver.whether_taken, 0, sizeof(sm->sm_entry[i].receiver.whether_taken));
    sm->sm_entry[i].to_bind = 0;
    pthread_mutex_unlock(&sm->sm_entry[i].lock);
    return 0;
}

int dropMessage(float p){
    float random_num = (float)rand() / RAND_MAX;
    if (random_num < p)
        return 1; // Message dropped
    else
        return 0; // Message not dropped
}


void* file_to_sender_thread(void* arg) {
    int sockfd = *(int *)arg;

    MTP_SM *g_sm = finder(SHM_KEY);
    FILE *file = fopen("large_file.txt", "r");
    if (!file) {
        perror("Failed to open file");
        return NULL;
    }
    // Determine file size for progress metrics
    fseek(file, 0, SEEK_END);
    long total_bytes = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    int cnt=0;
    char line[MTP_MSG_SIZE];
    int i = 0, j_ = 0;
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
    // Initialize progress counters if not yet
    pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
    if(!g_sm->sm_entry[j_].progress_initialized){
        g_sm->sm_entry[j_].total_bytes = total_bytes;
        g_sm->sm_entry[j_].sent_bytes = 0;
        clock_gettime(CLOCK_MONOTONIC, &g_sm->sm_entry[j_].start_time);
        g_sm->sm_entry[j_].progress_initialized = 1;
        MTP_LOG_INFO("progress init slot=%d total=%ldB", j_, total_bytes);
    }
    pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
    MTP_LOG_INFO("file->sender thread started slot=%d fd=%d", j_, sockfd);
    while(true){
        sleep(1);
        int rv=0;
        MTP_Message msg;
        while (rv<0 || fgets(line, sizeof(line), file)) {
            pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
            if(rv>=0){
                msg = (MTP_Message){0};
                strncpy(msg.data, line, sizeof(msg.data) - 1);
                msg.data[sizeof(msg.data) - 1] = '\0';
                msg.seq_num = i; // CHANGE THIS
                MTP_LOG_DEBUG("queue line seq=%d", msg.seq_num);
                msg.is_ack = false; // Not an ACK
                msg.wnd_sz= -1;
                msg.next_val = -1;
            }
            pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            rv = m_sendto(sockfd, (const void *)&msg, sizeof(msg), 0, (const struct sockaddr *)&g_sm->sm_entry[j_].dest_addr, sizeof(g_sm->sm_entry[j_].dest_addr));
            
            if(rv>=0){
                char filename[64] = "sender_buffer.txt";
                FILE *file = fopen(filename, "a");
                if (!file) {
                    MTP_LOG_ERR("cannot append sender_buffer.txt: %s", strerror(errno));
                    return NULL; 
                }
                fprintf(file, "%s\n", msg.data);
                fclose(file);
                // Progress update
                pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
                if(g_sm->sm_entry[j_].progress_initialized){
                    g_sm->sm_entry[j_].sent_bytes += (long)strnlen(msg.data, MTP_MSG_SIZE);
                    if(g_sm->sm_entry[j_].sent_bytes - g_sm->sm_entry[j_].last_progress_bytes >= 32*1024 || g_sm->sm_entry[j_].sent_bytes >= g_sm->sm_entry[j_].total_bytes){
                        g_sm->sm_entry[j_].last_progress_bytes = g_sm->sm_entry[j_].sent_bytes;
                        mtp_progress_log(&g_sm->sm_entry[j_], j_);
                    }
                }
                pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
            }
            if (rv < 0) {
                MTP_LOG_WARN("send attempt failed (slot=%d) will retry: %s", j_, strerror(errno));
                sleep(2);
                continue;
            }
            MTP_LOG_INFO("sent seq=%d", msg.seq_num);
            i = (i+1)%MAX_SEQ_NUM;
            // sleep(3);
        }
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
        // sleep(40);
        sleep(5);
        pthread_mutex_lock(&g_sm->sm_entry[j_].lock);
        MTP_Message buf;
        int addrlen = sizeof(g_sm->sm_entry[j_].dest_addr);
        if(g_sm->sm_entry[j_].receiver.buffer[0].seq_num == g_sm->r_ack[j_]){
            for(int j=0; j<RECV_BUFFER; j++){

                int rv = m_recvfrom(sockfd, (void *)&buf, sizeof(buf), 0, (struct sockaddr *)&g_sm->sm_entry[j_].dest_addr, &addrlen);
                if (rv < 0) {
                    perror("Failed to receive message");
                    break;
                }
                fprintf(file, "%s\n", buf.data);
                            MTP_LOG_INFO("delivered seq=%d to file", buf.seq_num);
                fflush(file);
            }
        }
        pthread_mutex_unlock(&g_sm->sm_entry[j_].lock);
        // sleep(T);
    }
    fclose(file);
    return NULL;
}

int count_buffer(int socket_id, int flag){
    MTP_SM *g_sm = finder(SHM_KEY);
    if (g_sm == NULL) {
        perror("Failed to find shared memory");
        return -1;
    }
    int cnt=0;
    if(flag==1){
        for(int k=0;k<RECV_BUFFER;k++){
            if(g_sm->sm_entry[socket_id].receiver.buffer[k].seq_num != -1) cnt++;
        }
    }
    else{
        for(int k=0;k<SENDER_BUFFER;k++){
            if(g_sm->sm_entry[socket_id].sender.buffer[k].seq_num != -1) cnt++;
        }
    }

    return cnt;

}

void order(MTP_Message arr[], int n) {
    int idx = 0; 
    for (int i = 0; i < n; i++) {
        if (arr[i].seq_num != -1) {
            arr[idx++] = arr[i];
        }
    }
    while (idx < n) {
        arr[idx++] = (MTP_Message){-1};
    }
}