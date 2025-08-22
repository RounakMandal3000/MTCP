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

#define SHM_KEY 0x1334           // Shared memory key for inter-process communication

#define ENOBUFS 105  // No buffer space available error code
#define ENOTBOUND 106 // Socket is not bound error code
#define ENOMSG 107    // No message available error code
#define EPROTONOSUPPORT 108 // Protocol not supported error code



// ------------------- Message / ACK Formats -------------------


// Message packet sent over unreliable channel
typedef struct {
    uint16_t seq_num;               // 4-bit (wraps around after 15)
    uint16_t wnd_sz;
    bool is_ack;                     // true if this is an ACK
    int next_val;
    char *data;        // payload, fix later
    struct timespec sent_time;    
} MTP_Message;

// ------------------- Receiver/Sender State -------------------

typedef struct RecvNode {
    MTP_Message msg;
    struct RecvNode *next;
} Node;

typedef struct {
    Node *front;  // head of queue
    Node *rear;   // tail of queue
    int count;       // number of elements
} MTP_Queue;

typedef struct {
    MTP_Queue buffer;   // receiver buffer (out-of-order storage)
    int next_val;
    int rwnd_count;                 // number of entries in receiver window
} MTP_Receiver;

typedef struct {
    MTP_Queue buffer;   // sender buffer (window of unacked msgs)
    MTP_Message swnd[SENDER_SWND];
    int swnd_count;                 // number of entries in sender window
} MTP_Sender;

// ------------------- Socket State -------------------



/**
 * @struct MTP_socket
 * @brief Represents a minimal transport protocol (MTP) socket.
 * 
 * @var udp_sockfd
 * File descriptor for the underlying UDP socket.
 */

/**
 * @struct MTP_SM_entry
 * @brief Represents an entry in the MTP socket management table.
 * 
 * @var lock
 * Mutex lock to ensure thread-safe access to the socket entry.
 * 
 * @var sock
 * Pointer to the associated MTP_socket structure.
 * 
 * @var free_slot
 * Static boolean flag indicating whether this entry is available for use.
 * 
 * @var pid_creation
 * Process ID of the process that created this socket entry.
 * 
 * @var dest_addr
 * Destination address (IP and port) for the socket.
 * 
 * @var src_addr
 * Source address (IP and port) for the socket.
 * 
 * @var sender
 * Pointer to the MTP_Sender structure for managing outgoing data.
 * 
 * @var receiver
 * Pointer to the MTP_Receiver structure for managing incoming data.
 * 
 * @note Review this part of the code to ensure proper handling of the static 
 *       `free_slot` variable and its implications in a multi-threaded environment.
 */
typedef struct{
    int udp_sockfd;
} MTP_socket;

typedef struct{
    pid_t pid;
    char *filename;
} Node_pid;

typedef struct {
    Node_pid *front;   // points to the first element
    Node_pid *rear;    // points to the last element
    int size;          // optional, tracks number of elements
} PIDQueue;


typedef struct{
    pthread_mutex_t lock;
    MTP_socket sock;
    bool free_slot;
    pid_t pid_creation; // PID of the process that created this socket
    struct sockaddr dest_addr;   // Destination IP + portbut 
    struct sockaddr src_addr;    // Source IP + port
    MTP_Sender sender;
    MTP_Receiver receiver;
} MTP_SM_entry;



typedef struct{
    // PIDQueue pid_queue;
    pthread_mutex_t lock_sm;
    MTP_SM_entry sm_entry[MAX_SOCKETS];
    int count_occupied;
} MTP_SM;

// ------------------- API Functions -------------------
MTP_SM* finder(int shmid);
int m_socket(int domain, int type, int protocol);
int m_bind(int sockfd, struct sockaddr *src_addr, struct sockaddr *dest_addr, int addrlen);
int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen);
int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen);
int m_close(int sockfd);
int dropMessage(float p);
void init_recv_queue(MTP_Queue *q);
int enqueue_recv(MTP_Queue *q, MTP_Message msg);
int dequeue_recv(MTP_Queue *q, MTP_Message *msg);
int is_empty(MTP_Queue *q);
int is_full(MTP_Queue *q);
void traverse_recv(MTP_Queue *q);
void* file_to_sender_thread(void* arg);
void* receiver_to_file_thread(void* arg);
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
