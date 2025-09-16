#include "msocket.h"
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
#include <pthread.h>


int main(int argc, char *argv[])
{
    if (argc < 5){
        MTP_LOG_WARN("Usage: %s <ip1> <port1> <ip2> <port2>", argv[0]);
        exit(1);
    }

    char *ip1 = argv[1];
    char *port1 = argv[2];
    char *ip2 = argv[3];
    char *port2 = argv[4];

    pid_t pid = getpid();   // get process ID
    if (pid < 0) {
        perror("Failed to get process ID");
        exit(1);
    }
    struct sockaddr_in src_addr, dest_addr;
    src_addr.sin_family = AF_INET;
    src_addr.sin_port = htons(atoi(port1));
    inet_pton(AF_INET, ip1, &src_addr.sin_addr);

    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(atoi(port2));
    inet_pton(AF_INET, ip2, &dest_addr.sin_addr);
    MTP_LOG_INFO("creating MTP socket");
    int rv = m_socket(AF_INET, SOCK_MTP, 0);
    int sockfd = rv;
    if (rv < 0) {
        perror("Error creating MTP socket");
        exit(1);
    }
    MTP_LOG_INFO("socket ready fd=%d", sockfd);
    MTP_LOG_INFO("binding MTP socket");
    rv = m_bind(rv, (struct sockaddr *)&src_addr, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
    if (rv < 0) {
        perror("Error binding MTP socket");
        exit(1);
    }
    MTP_LOG_INFO("starting receiver thread for file output");
    pthread_t receiver_thread_;
    pthread_create(&receiver_thread_, NULL, receiver_to_file_thread, &sockfd);
    pthread_join(receiver_thread_, NULL);
    return 0;
}