#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <fcntl.h>

#define PORT 9000
#define BUF_SIZE 1024

void print_fdset(fd_set *set, int maxfd) {
    printf("FD_SET contents: [");
    for (int fd = 0; fd < maxfd; fd++) {
        if (FD_ISSET(fd, set)) {
            printf(" %d", fd);
        }
    }
    printf(" ]\n");
}
int main() {
    int sockfd;
    struct sockaddr_in servaddr;

    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(1);
    }
    printf("Created UDP socket fd=%d\n", sockfd);

    // Zero and setup server address
    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = INADDR_ANY;
    servaddr.sin_port = htons(PORT);

    // Bind socket to port
    if (bind(sockfd, (const struct sockaddr *)&servaddr, sizeof(servaddr)) < 0) {
        perror("bind failed");
        // close(sockfd);
        exit(1);
    }
    printf("Listening on port %d...\n", PORT);

    // Main loop with select
    while (1) {
        fd_set rfds;
        struct timeval tv;
        int maxfd, rv;

        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        maxfd = sockfd + 1;
        printf("SOCKFD: %d\n", sockfd);

        tv.tv_sec = 5;   // timeout of 5 sec
        tv.tv_usec = 0;
        
        printf("Waiting on select()...\n");
        rv = select(maxfd, &rfds, NULL, NULL, &tv);
        printf("SELECT WILL END\n");
        print_fdset(&rfds, sockfd);
        if (rv < 0) {
            perror("select failed");
            break;
        } else if (rv == 0) {
            printf("Timeout, no data.\n");
            continue;
        }

        if (FD_ISSET(sockfd, &rfds)) {
            char buffer[BUF_SIZE];
            struct sockaddr_in cliaddr;
            socklen_t len = sizeof(cliaddr);
            getpeername(sockfd, (struct sockaddr *)&cliaddr, &len);
            printf("Client Address: %s:%d\n", inet_ntoa(cliaddr.sin_addr), ntohs(cliaddr.sin_port));

            int n = recvfrom(sockfd, buffer, BUF_SIZE - 1, 0,
                             (struct sockaddr *)&cliaddr, &len);
            if (n < 0) {
                perror("recvfrom failed");
                continue;
            }
            buffer[n] = '\0';
            printf("Received: %s\n", buffer);
        }
    }

    // close(sockfd);
    return 0;
}
