#ifndef M_SOCKET_H
#define M_SOCKET_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>   // for fprintf, stderr in logging helpers
#include <time.h>     // for struct timespec, clock_gettime

// Fallback for environments lacking CLOCK_MONOTONIC (e.g., some MinGW variants)
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

#if defined(_WIN32) && !defined(__MINGW32__)
// Simple emulation using clock() (low resolution). For real high-res use QueryPerformanceCounter.
static inline int mtp_clock_gettime_fake(int clk_id, struct timespec *ts){
    (void)clk_id;
    clock_t c = clock();
    ts->tv_sec = c / CLOCKS_PER_SEC;
    ts->tv_nsec = (long)((c % CLOCKS_PER_SEC) * (1000000000L / CLOCKS_PER_SEC));
    return 0;
}
#define clock_gettime(a,b) mtp_clock_gettime_fake(a,b)
#endif
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
#define DROP_PROB 0.3           // Packet drop probability
#define TIME_SEC 5              // Timeout duration in seconds
#define TIME_USEC 0             // Timeout duration in microseconds
#define MAX_NODES 1024
#define SHM_KEY 0x2434          // Shared memory key for inter-process communication

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
    // --- Progress / stats ---
    long total_bytes;              // total payload bytes intended to send (sender side)
    long sent_bytes;               // bytes enqueued/sent (approx)
    long retransmissions;          // count of retransmissions
    long last_progress_bytes;      // last bytes value when we logged progress
    struct timespec start_time;    // when first progress initialized
    int progress_initialized;      // flag: 1 if total_bytes set
} MTP_SM_entry;


typedef struct{
    pthread_mutex_t lock_sm;
    MTP_SM_entry sm_entry[MAX_SOCKETS];
    int count_occupied;
    NodeSlot node_pool[MAX_NODES]; 
    int bind_socket;
    int r_ack[MAX_NODES];
} MTP_SM;

// ------------------- Logging Utilities -------------------
#ifndef MTP_NO_COLOR
#define MTP_CLR_RESET   "\x1b[0m"
#define MTP_CLR_DIM     "\x1b[2m"
#define MTP_CLR_RED     "\x1b[31m"
#define MTP_CLR_GREEN   "\x1b[32m"
#define MTP_CLR_YELLOW  "\x1b[33m"
#define MTP_CLR_BLUE    "\x1b[34m"
#define MTP_CLR_MAGENTA "\x1b[35m"
#define MTP_CLR_CYAN    "\x1b[36m"
#define MTP_CLR_BOLD    "\x1b[1m"
#else
#define MTP_CLR_RESET   ""
#define MTP_CLR_DIM     ""
#define MTP_CLR_RED     ""
#define MTP_CLR_GREEN   ""
#define MTP_CLR_YELLOW  ""
#define MTP_CLR_BLUE    ""
#define MTP_CLR_MAGENTA ""
#define MTP_CLR_CYAN    ""
#define MTP_CLR_BOLD    ""
#endif

#define MTP_TAG(tag,color) MTP_CLR_BOLD color tag MTP_CLR_RESET

#define MTP_LOG_INFO(fmt, ...)  fprintf(stderr, MTP_TAG("[INFO] ", MTP_CLR_CYAN) fmt "%s", ##__VA_ARGS__, "\n")
#define MTP_LOG_WARN(fmt, ...)  fprintf(stderr, MTP_TAG("[WARN] ", MTP_CLR_YELLOW) fmt "%s", ##__VA_ARGS__, "\n")
#define MTP_LOG_ERR(fmt, ...)   fprintf(stderr, MTP_TAG("[ERR ] ", MTP_CLR_RED) fmt "%s", ##__VA_ARGS__, "\n")
#define MTP_LOG_DEBUG(fmt, ...) fprintf(stderr, MTP_TAG("[DBG ] ", MTP_CLR_DIM) fmt "%s", ##__VA_ARGS__, "\n")
#define MTP_LOG_PROGRESS(fmt, ...) fprintf(stderr, MTP_TAG("[PROG] ", MTP_CLR_GREEN) fmt "%s", ##__VA_ARGS__, "\n")

// Compact window snapshot helpers (implemented inline for header-only ease)
static inline void mtp_print_window_sender(const MTP_Sender *s){
    fprintf(stderr, MTP_TAG("[SWND] ", MTP_CLR_GREEN));
    for(int i=0;i<SENDER_SWND;i++){
        int sn = s->swnd[i].seq_num;
        if(sn==-1) fprintf(stderr, " __"); else fprintf(stderr, " %02d", sn);
    }
    fprintf(stderr, " | count=%d\n", s->swnd_count);
}

static inline void mtp_print_buffer_sender(const MTP_Sender *s){
    fprintf(stderr, MTP_TAG("[SBUF] ", MTP_CLR_BLUE));
    for(int i=0;i<SENDER_BUFFER;i++){
        int sn = s->buffer[i].seq_num;
        if(sn==-1) fprintf(stderr, " __"); else fprintf(stderr, " %02d", sn);
    }
    fprintf(stderr, "\n");
}

static inline void mtp_print_buffer_receiver(const MTP_Receiver *r){
    fprintf(stderr, MTP_TAG("[RBUF] ", MTP_CLR_MAGENTA));
    for(int i=0;i<RECV_BUFFER;i++){
        int sn = r->buffer[i].seq_num;
        if(sn==-1) fprintf(stderr, " __"); else fprintf(stderr, " %02d", sn);
    }
    fprintf(stderr, "\n");
}

// Progress bar helper (not thread-safe by itself; call under appropriate locks)
static inline void mtp_progress_log(const MTP_SM_entry *e, int slot){
    if(!e->progress_initialized || e->total_bytes <= 0) return;
    double pct = (e->sent_bytes <= 0) ? 0.0 : (double)e->sent_bytes * 100.0 / (double)e->total_bytes;
    if(pct > 100.0) pct = 100.0; // clamp
    const int barw = 30;
    int fill = (int)(pct/100.0 * barw);
    if(fill > barw) fill = barw;
    char bar[barw+1];
    for(int i=0;i<barw;i++) bar[i] = (i<fill)?'#':'-';
    bar[barw] = '\0';
    // ETA estimation
    struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now);
    double elapsed = (now.tv_sec - e->start_time.tv_sec) + (now.tv_nsec - e->start_time.tv_nsec)/1e9;
    double eta = 0.0;
    if(e->sent_bytes > 0 && elapsed > 0.0){
        double rate = (double)e->sent_bytes / elapsed; // bytes per sec
        if(rate > 0.0){
            eta = (double)(e->total_bytes - e->sent_bytes) / rate;
        }
    }
    int eta_min = (int)(eta/60.0);
    int eta_sec = (int)eta - eta_min*60;
    double kbps = (elapsed>0 && e->sent_bytes>0) ? ((double)e->sent_bytes/1024.0)/elapsed : 0.0;
    MTP_LOG_PROGRESS("slot=%d %6.2f%% [%s] %ld/%ldB eta=%02d:%02d thr=%0.2fKB/s rtx=%ld", slot, pct, bar, e->sent_bytes, e->total_bytes, eta_min, eta_sec, kbps, e->retransmissions);
}

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
void order(MTP_Message arr[], int n);

#endif // M_SOCKET_H




