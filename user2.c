#include "mysocket.h"
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


int main(int argc, char *argv[])
{
    if (argc < 5){
        printf("Usage: %s <ip1> <port1> <ip2> <port2>\n", argv[0]);
        exit(1);
    }

    char *ip1 = argv[1];
    char *port1 = argv[2];
    char *ip2 = argv[3];
    char *port2 = argv[4];
    char *filename_src = NULL;
    if(argc >= 6) 
        filename_src = argv[5];
    pid_t pid = getpid();   // get process ID
    char filename_dest[64];   
    sprintf(filename_dest, "output_%d.txt", pid);
    
    FILE *fp = fopen(filename_dest, "w");
    if (fp == NULL) {
        perror("Error creating destination file");
        exit(1);
    }

    FILE *fp = fopen(filename_src, "r");
    if (fp == NULL) {
        perror("Error opening source file");
        exit(1);
    }

    MTP_SM *mtp_sm = malloc(sizeof(MTP_SM));
    if (mtp_sm == NULL) {
        perror("Failed to allocate memory for MTP_SM");
        exit(1);
    }
    initmsocket();

    return 0;
}