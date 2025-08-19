#ifndef M_SOCKET_H
#define M_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
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

#define T 5                     // Timeout duration in seconds
#define DROP_PROB 0.5           // Packet drop probability
#define TIME_SEC 1              // Timeout duration in seconds
#define TIME_USEC 0             // Timeout duration in microseconds

#define SHM_KEY 0x1234           // Shared memory key for inter-process communication

#define ENOBUFS 105  // No buffer space available error code
#define ENOTBOUND 106 // Socket is not bound error code
#define ENOMSG 107    // No message available error code

// ------------------- Message / ACK Formats -------------------


// Message packet sent over unreliable channel
typedef struct {
    uint16_t seq_num;               // 4-bit (wraps around after 15)
    uint16_t length;                // number of bytes used (<= 1024)
    char data[MTP_MSG_SIZE];        // payload
    bool is_ack;                    // true if this is an ACK
} MTP_Message;

// Acknowledgement packet
typedef struct {
    uint16_t ack_num;               // last in-order seq received
    uint16_t rwnd;                  // free buffer slots available at receiver
    bool is_ack;                    // true if this is an ACK
} MTP_Ack;


// ------------------- Receiver/Sender State -------------------

typedef struct{
    int seq_num;
    struct timespec sent_time;
} sender_window_entry;

typedef struct{
    int seq_num;
} receiver_window_entry;

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
    bool occupied[RECV_BUFFER];        // marks if buffer slot is filled
    int rwnd_start;                 // base of the receiver window
    int expected_seq;               // next expected in-order seq
    receiver_window_entry rwnd[RECV_SWND];
} MTP_Receiver;

typedef struct {
    MTP_Queue buffer; // sender buffer (window of unacked msgs)
    int swnd_start;                  // base of the sender window
    int swnd_end;                    // next free slot for sending
    int next_seq;                   // sequence number for next message
    sender_window_entry swnd[SENDER_SWND];
    int swnd_count;                 // number of entries in sender window
} MTP_Sender;

// ------------------- Socket State -------------------
typedef struct{
    int udp_sockfd;
    struct sockaddr_in src_addr;    // Source IP + port
    struct sockaddr_in dest_addr;   // Destination IP + port
    MTP_SM *mtp_sm;
} MTP_socket;

typedef struct{
    pthread_mutex_t lock;
    MTP_socket *sock;
    bool free_slot;
    pid_t pid_creation; // PID of the process that created this socket
    int udp_sockfd; 
    struct sockaddr_in dest_addr;   // Destination IP + port
    MTP_Sender *sender;
    MTP_Receiver *receiver;
} MTP_SM_entry;

typedef struct{
    MTP_SM_entry sm_entry[MAX_SOCKETS];
    int any_free;
} MTP_SM;

MTP_SM sm;
// ------------------- API Functions -------------------

int m_socket(int domain, int type, int protocol);
int m_bind(int sockfd, struct sockaddr *my_addr, int addrlen);
int m_sendto(int sockfd, const void *msg, int len, unsigned int flags, const struct sockaddr *to, socklen_t tolen);
int m_recvfrom(int sockfd, void *buf, int len, unsigned int flags, struct sockaddr *from, int *fromlen);
int m_close(int sockfd);
int dropMessage(float p);
void init_recv_queue(MTP_Queue *q);
int enqueue_recv(MTP_Queue *q, MTP_Message msg);
int dequeue_recv(MTP_Queue *q, MTP_Message *msg);
int is_empty(MTP_Queue *q);
int is_full(MTP_Queue *q);

#endif // M_SOCKET_H
