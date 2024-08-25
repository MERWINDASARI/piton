#include<iostream>
#include <winsock2.h>
#include <cassert>
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
const size_t k_max_msg = 4096;
static int32_t read_full(int fd, char *buf, size_t n){
    while(n>0){
         ssize_t rv = recv(fd, buf, n,0);
         if(rv<=0){
            //error or unexpected EOF
            return -1;
         }
         assert((size_t)rv <= n);
         n-=(size_t)rv;
         buf+=rv;
    }
    return 0;
}

static int32_t write_all(int fd, const char *buf, size_t n) {
    while (n > 0) {
        ssize_t rv = send(fd, buf, n,0);
        if (rv <= 0) {
            return -1;  // error
        }
        assert((size_t)rv <= n);
        n -= (size_t)rv;
        buf += rv;
    }
    return 0;
}

static int32_t one_request(int connfd) {
    //4 bytes header
    char rbuf[4 + k_max_msg + 1];
    errno = 0;

    int32_t err = read_full(connfd, rbuf, 4);

    if (err) {
        if (errno == 0) {
            msg("EOF");
        } else {
            msg("read() error");
        }
        return err;
    }

    uint32_t len = 0;
    memcpy(&len, rbuf, 4);  // assume little endian
    if (len > k_max_msg) {
        msg("too long");
        return -1;
    }
    // request body
    err = read_full(connfd, &rbuf[4], len);
    if (err) {
        msg("read() error");
        return err;
    }

    // do something
    rbuf[4 + len] = '\0';
    printf("client says: %s\n", &rbuf[4]);

    // reply using the same protocol
    const char reply[] = "read your msg ";
    char wbuf[4 + sizeof(reply)];
    len = (uint32_t)strlen(reply);
    memcpy(wbuf, &len, 4);
    memcpy(&wbuf[4], reply, len);
    return write_all(connfd, wbuf, 4 + len);
}

int main(){

    //windows related intialization
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


    //this is more like exception 
    if (rv == SOCKET_ERROR) {
        printf("bind failed with error: %d\n", WSAGetLastError());
        WSACleanup();
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

        while(true){
            //server serves one client at ones
            int32_t err = one_request(connfd);
            if(err){
                break;
            }
        }

        closesocket(connfd);
    }
    WSACleanup();
    return 0;

}