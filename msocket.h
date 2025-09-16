#ifndef M_SOCKET_H
#define M_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#define MTP_MSG_SIZE   1024     // 1 KB fixed message size
#define SEQ_BITS       4        // 4-bit sequence number
#define MAX_SEQ_NUM    (1 << SEQ_BITS) // 16 unique seq numbers
#define SENDER_BUFFER     10        // sender window size
#define RECV_BUFFER       5        // receiver buffer size
#define TIMEOUT_SEC    2        // retransmission timeout (T seconds)
#define MAX_SOCKETS    25       // maximum number of sockets
#define SENDER_SWND 5           // sender sliding window size
#define RECV_SWND 5             // receiver sliding window size
#define SOCK_MTP 12345          // MTP socket type
#define T 5                     // Timeout duration in seconds
#define DROP_PROB 0.5           // Packet drop probability
#define TIME_SEC 5              // Timeout duration in seconds
#define TIME_USEC 0             // Timeout duration in microseconds
#define MAX_NODES 1024
#define SHM_KEY 0x2433           // Shared memory key for inter-process communication

#define ENOBUFS 105  // No buffer space available error code
#define ENOTBOUND 106 // Socket is not bound error code
#define ENOMSG 107    // No message available error code
#define EPROTONOSUPPORT 108 // Protocol not supported error code
#define SOCK_PORT 12000
#define SERVER_IP "127.0.0.1"

// Convert offset <-> pointer relative to base
#define OFFSET(base, ptr)   ((ptr) ? (size_t)((char*)(ptr) - (char*)(base)) : 0)
#define PTR(base, offset)   ((offset) ? (void*)((char*)(base) + (offset)) : NULL)

// struct timespec default_time = {0};

// ------------------- Message / ACK Formats -------------------


typedef struct {
    int seq_num;               // 4-bit (wraps around after 15)
    uint16_t wnd_sz;
    bool is_ack;                     // true if this is an ACK
    int next_val;
    char data[MTP_MSG_SIZE];        // payload, fix later
    struct timespec sent_time;    
} MTP_Message;

// ------------------- Receiver/Sender State -------------------
typedef struct{
    pid_t pid;
    char *filename;
} Node_pid;
typedef struct {
    Node_pid *front;   // points to the first element
    Node_pid *rear;    // points to the last element
    int size;          // optional, tracks number of elements
} PIDQueue;


// typedef struct RecvNode {
//     MTP_Message msg;
//     struct RecvNode *next;
// } Node;
typedef struct RecvNode {
    MTP_Message msg;
    size_t next;   
} Node;

typedef struct socketInfo
{
    int sockfd;
    struct sockaddr_in addr;
} socketInfo;


typedef struct {
    int used;
    Node node;
} NodeSlot;
// typedef struct {
//     Node *front;  // head of queue
//     Node *rear;   // tail of queue
//     int count;       // number of elements
// } MTP_Queue;
typedef struct {
    size_t front;  
    size_t rear;   
    int count;         
} MTP_Queue;

typedef struct {
    MTP_Message buffer[RECV_BUFFER];  
    int next_val;
    int rwnd_count; 
    int whether_taken[RECV_BUFFER];
} MTP_Receiver;

typedef struct {
    MTP_Message buffer[SENDER_BUFFER];  
    MTP_Message swnd[SENDER_SWND];
    int swnd_count;                 
} MTP_Sender;

typedef struct{
    int udp_sockfd;
} MTP_socket;


typedef struct{
    pthread_mutex_t lock;
    MTP_socket sock;
    bool free_slot;
    pid_t pid_creation; // PID of the process that created this socket
    struct sockaddr_in dest_addr;   // Destination IP + portbut 
    struct sockaddr_in src_addr;    // Source IP + port
    MTP_Sender sender;
    MTP_Receiver receiver;
    int to_bind;
} MTP_SM_entry;


typedef struct{
    pthread_mutex_t lock_sm;
    MTP_SM_entry sm_entry[MAX_SOCKETS];
    int count_occupied;
    NodeSlot node_pool[MAX_NODES]; 
    int bind_socket;
    int r_ack[MAX_NODES];
} MTP_SM;

// ------------------- API Functions -------------------

MTP_SM* finder(int shmid);
int m_socket(int domain, int type, int protocol);
int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen);
int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen);
int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen);
int m_close(int sockfd);
int dropMessage(float p);
void* file_to_sender_thread(void* arg);
void* receiver_to_file_thread(void* arg);
int is_empty_buffer(int socket_id, int flag);
int is_full_buffer(int socket_id, int flag);
int count_buffer(int socket_id, int flag);
int get_buffer_size(int socket_id, int flag);
int remove_from_buffer(int socket_id, MTP_Message *msg, int flag);
int add_to_buffer(int socket_id, MTP_Message *msg, int flag);
Node* get_node(void *base, int offset);
void* base_finder(int shmid);
void print_queue(int socket_id, int flag);
void initializer_message(MTP_Message* msg, bool is_ack);

// void initQueue_pid(PIDQueue *q);
// int is_Empty_pid(PIDQueue *q);
// void enqueue_pid(PIDQueue *q, pid_t pid, char* filename);
// Node_pid dequeue_pid(PIDQueue *q);

#endif // M_SOCKET_H




//  // Processing for Acknowledgment
//                         MTP_Message *ack_back = malloc(sizeof(MTP_Message));
//                         initializer_message(ack_back, true);
//                         int cnt = 0;
//                         for(int j=0; j < g_sm->sm_entry[i].receiver->rwnd_count; j++) {
//                             if (g_sm->sm_entry[i].receiver->rwnd[j].seq_num == buf->seq_num) {
//                                 cnt = -1;
//                                 break;
//                             }
//                         }
                        

//                         if(cnt!=-1){        //NOT DUPLICATE MESSAGE
//                             g_sm->sm_entry[i].receiver->rwnd_count++;
//                             g_sm->sm_entry[i].receiver->rwnd[g_sm->sm_entry[i].receiver->rwnd_count].seq_num = buf->seq_num;
//                             g_sm->sm_entry[i].receiver->rwnd_count++;
//                             int j_ = g_sm->sm_entry[i].receiver->rwnd_count - 1;
//                             for(int j=g_sm->sm_entry[i].receiver->rwnd_count-2; j >= 0; j--) {
//                                 if (g_sm->sm_entry[i].receiver->rwnd[j].seq_num > buf->seq_num) {
//                                     swap(g_sm->sm_entry[i].receiver->rwnd[j], g_sm->sm_entry[i].receiver->rwnd[j+1]);
//                                     j_ = j;
//                                 }
//                             }
//                             if(j_==0 || g_sm->sm_entry[i].receiver->rwnd[j_] == g_sm->sm_entry[i].receiver->rwnd[j_-1]) cnt = 1;
//                         } 
//                         else{              //DUPLICATE MESSAGE

//                         }
//                         ack_back.is_ack = true;
//                         if(cnt > 0)  ack_back.seq_num = buf->seq_num;
//                         else{
//                             ack_back.seq_num = 0;
//                             for(int j=1;j<g_sm->sm_entry[i].receiver->rwnd_count;j++){
//                                 if(g_sm->sm_entry[i].receiver->rwnd[j].seq_num == g_sm->sm_entry[i].receiver->rwnd[j-1].seq_num + 1) ack_back.seq_num = g_sm->sm_entry[i].receiver->rwnd[j].seq_num;
//                                 else break;
//                             }
//                         }
//                         ack_back.wnd_sz = g_sm->sm_entry[i].receiver->rwnd_count;
//                         pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
//                         int m = m_sendto(g_sm->sm_entry[i].sock->udp_sockfd, &ack_back, sizeof(MTP_Message), 0, (struct sockaddr *)&g_sm->sm_entry[i].sock->dest_addr, sizeof(g_sm->sm_entry[i].sock->dest_addr));
//                         pthread_mutex_lock(&g_sm->sm_entry[i].lock);
//                         if (m < 0) {
//                             perror("sendto failed");
//                             pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
//                             continue;
//                         }

//                         pthread_mutex_unlock(&g_sm->sm_entry[i].lock);
//                         printf("Received packet from %s:%d\n", inet_ntoa(from.sin_addr), ntohs(from.sin_port));