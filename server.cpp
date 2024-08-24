#include<iostream>
#include <winsock2.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#pragma comment (lib, "Ws2_32.lib")

using namespace std;

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}

static void do_something(int connfd) {
    char rbuf[64] = {};
    ssize_t n = recv(connfd, rbuf, sizeof(rbuf) - 1,0);
    if (n < 0) {
        msg("read() error");
        return;
    }
    printf("client says: %s\n", rbuf);

    char wbuf[] = "read your msg";
    send(connfd, wbuf, strlen(wbuf),0);
}

int main(){

    WSADATA wsaData;
    int iResult;
    iResult = WSAStartup(MAKEWORD(2,2), &wsaData);
    if (iResult != 0) {
        printf("WSAStartup failed with error: %d\n", iResult);
        return 1;
    }

    //get a socket
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int val=1;
    //configure
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR, (char *) &val, sizeof(val));

    //bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);
    addr.sin_addr.s_addr = ntohl(0);    // wildcard address 0.0.0.0
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));

    if (rv == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        WSACleanup();
        return 1;
    }

    //this is more like exception 
    if (rv) {
        die("bind()");
    }

    //listen
    rv = listen(fd, SOMAXCONN);
    if (rv) {
        die("listen()");
    }
    cout<<"running";
    //processes each client connection
    while (true) {
        // accept
        struct sockaddr_in client_addr = {};
        int addrlen = sizeof(client_addr);
        int connfd = accept(fd, (struct sockaddr *)&client_addr, &addrlen);
        if (connfd < 0) {
            continue;   // error
        }

        do_something(connfd);
        closesocket(connfd);
    }
    WSACleanup();

}