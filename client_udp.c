#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define PORT 9000
#define SERVER_ADDR "127.0.0.1"

int main() {
    int sockfd;
    struct sockaddr_in servaddr;
    const char *msg = "hello select";
    while(1){
    // Create UDP socket
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    printf("kawai %d \n", sockfd);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(1);
    }

    memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_port = htons(PORT);
    servaddr.sin_addr.s_addr = inet_addr(SERVER_ADDR);

    // Print server address
    // getpeername(sockfd, (struct sockaddr *)&servaddr, &len);
    printf("Server Address: %s:%d\n", inet_ntoa(servaddr.sin_addr), ntohs(servaddr.sin_port));
    // Send message
    int n = sendto(sockfd, msg, strlen(msg), 0,
                   (struct sockaddr *)&servaddr, sizeof(servaddr));
    if (n < 0) {
        perror("sendto failed");
        // close(sockfd);
        exit(1);
    }

    printf("Sent: %s\n", msg);

    sleep(2);
}
    // close(sockfd);
    return 0;
}
