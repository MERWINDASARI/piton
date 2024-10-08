#include<iostream>
#include <winsock2.h>
#include <cassert>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <vector>
#include <unordered_map>
#pragma comment (lib, "Ws2_32.lib")

const size_t k_max_msg = 4096;
const bool blocking = false;
enum {
    STATE_REQ = 0,  // reading state
    STATE_RES = 1,  // sending state
    STATE_END = 2,  // mark the connection for deletion
};

struct Conn {
    int fd = -1;
    // either STATE_REQ or STATE_RES
    uint32_t state = 0;     
    // buffer for reading
    size_t rbuf_size = 0;
    char rbuf[4 + k_max_msg];

    // buffer for writing
    size_t wbuf_size = 0;
    // Bytes sent till now
    size_t wbuf_sent = 0;
    char wbuf[4 + k_max_msg];
};

static void die(const char *msg) {
    int err = errno;
    fprintf(stderr, "[%d] %s\n", err, msg);
    abort();
}
static void msg(const char *msg) {
    fprintf(stderr, "%s\n", msg);
}






static bool try_flush_buffer(Conn *conn) {
    ssize_t rv = 0;
    do {
        size_t remain = conn->wbuf_size - conn->wbuf_sent;
        rv = send(conn->fd, &conn->wbuf[conn->wbuf_sent], remain,0);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN(not ready), stop and no state change is needed.
        return false;
    }
    if (rv < 0) {
        msg("write() error");
        conn->state = STATE_END;
        return false;
    }
    conn->wbuf_sent += (size_t)rv;
    assert(conn->wbuf_sent <= conn->wbuf_size);
    if (conn->wbuf_sent == conn->wbuf_size) {
        // response was fully sent, change state back
        conn->state = STATE_REQ;
        conn->wbuf_sent = 0;
        conn->wbuf_size = 0;
        return false;
    }
    // still got some data in wbuf, could try to write again
    return true;
}

static void state_req(Conn *conn);
static void state_res(Conn *conn);
const size_t k_max_args = 1024;

static int32_t parse_req(const char *data, size_t len, std::vector<std::string> &out){
    if (len < 4) {
        return -1;
    }
    uint32_t n = 0;
    memcpy(&n, &data[0], 4);
    if (n > k_max_args) {
        return -1;
    }

    size_t pos = 4;
    while (n--) {
        if (pos + 4 > len) {
            return -1;
        }
        uint32_t sz = 0;
        memcpy(&sz, &data[pos], 4);
        if (pos + 4 + sz > len) {
            return -1;
        }
        out.push_back(std::string((char *)&data[pos + 4], sz));
        pos += 4 + sz;
    }
    if (pos != len) {
        return -1;  // trailing garbage
    }
    return 0;
}

static bool cmd_is(const std::string &word, const char *cmd) {
    return 0 == strcasecmp(word.c_str(), cmd);
}
enum {
    RES_OK = 0,
    RES_ERR = 1,
    RES_NX = 2,
};

static std::unordered_map<std::string, std::string> g_map;

static uint32_t do_get(const std::vector<std::string> &cmd, char *res, uint32_t *reslen){
    if (!g_map.count(cmd[1])) {
        return RES_NX;
    }
    std::string &val = g_map[cmd[1]];
    assert(val.size() <= k_max_msg);
    memcpy(res, val.data(), val.size());
    *reslen = (uint32_t)val.size();
    return RES_OK;
}

static uint32_t do_set(const std::vector<std::string> &cmd, char *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map[cmd[1]] = cmd[2];
    return RES_OK;
}

static uint32_t do_del(const std::vector<std::string> &cmd, char *res, uint32_t *reslen)
{
    (void)res;
    (void)reslen;
    g_map.erase(cmd[1]);
    return RES_OK;
}

static int32_t do_request(const char *req, uint32_t reqlen,
    uint32_t *rescode, char *res, uint32_t *reslen){
        std::vector<std::string> cmd;
        if (0 != parse_req(req, reqlen, cmd)) {
            msg("bad req");
            return -1;
        }
        if (cmd.size() == 2 && cmd_is(cmd[0], "get")) {
        *rescode = do_get(cmd, res, reslen);
        } else if (cmd.size() == 3 && cmd_is(cmd[0], "set")) {
        *rescode = do_set(cmd, res, reslen);
        } else if (cmd.size() == 2 && cmd_is(cmd[0], "del")) {
        *rescode = do_del(cmd, res, reslen);
        } else {
            // cmd is not recognized
            *rescode = RES_ERR;
            const char *msg = "Unknown cmd";
            strcpy((char *)res, msg);
            *reslen = strlen(msg);
            return 0;
        }
    return 0;
}

static bool try_one_request(Conn *conn) {
    // try to parse a request from the buffer
    if (conn->rbuf_size < 4) {
        // not enough data in the buffer to get length of msg. Will retry in the next iteration
        return false;
    }
    uint32_t len = 0;
    memcpy(&len, &conn->rbuf[0], 4);
    if (len > k_max_msg) {
        msg("too long");
        conn->state = STATE_END;
        return false;
    }
    if (4 + len > conn->rbuf_size) {
        // buffer contains less data than mentiond. Will retry in the next iteration
        return false;
    }

    // got one request lets generate  the responce
    //printf("client says: %.*s\n", len, &conn->rbuf[4]);
    uint32_t rescode = 0;
    uint32_t wlen = 0;
    int32_t err = do_request(&conn->rbuf[4], len,&rescode, &conn->wbuf[4 + 4], &wlen);
    if (err) {
        conn->state = STATE_END;
        return false;
    }


    // generating echoing response
    wlen += 4;
    memcpy(&conn->wbuf[0], &wlen, 4);
    memcpy(&conn->wbuf[4], &rescode, 4);
    conn->wbuf_size = 4 + len;

    // remove the request from the buffer.
    // note: frequent memmove is inefficient.
    // note: need better handling for production code.
    size_t remain = conn->rbuf_size - 4 - len;
    if (remain) {
        memmove(conn->rbuf, &conn->rbuf[4 + len], remain);
    }
    conn->rbuf_size = remain;

    // change state
    conn->state = STATE_RES;
    state_res(conn);

    // continue the outer loop if the request was fully processed
    return (conn->state == STATE_REQ);
}

static bool try_fill_buffer(Conn *conn) {
    // try to fill the buffer
    assert(conn->rbuf_size < sizeof(conn->rbuf));
    ssize_t rv = 0;
    do {
        size_t cap = sizeof(conn->rbuf) - conn->rbuf_size;
        rv = recv(conn->fd, &(conn->rbuf[conn->rbuf_size]), cap,0);
    } while (rv < 0 && errno == EINTR);
    if (rv < 0 && errno == EAGAIN) {
        // got EAGAIN, stop.
        return false;
    }
    if (rv < 0) {
        if(10035 == WSAGetLastError()) msg("No data is queued to be read from the socket");
        else msg("read() error");
        conn->state = STATE_END;
        return false;
    }
    if (rv == 0) {
        if (conn->rbuf_size > 0) {
            msg("unexpected EOF");
        } else {
            msg("EOF");
        }
        conn->state = STATE_END;
        return false;
    }

    conn->rbuf_size += (size_t)rv;
    assert(conn->rbuf_size <= sizeof(conn->rbuf));

    // Try to process requests one by one.
    // "pipelining": we will process next request of client before sending responce to previous one
    while (try_one_request(conn)) {}
    return (conn->state == STATE_REQ);
}

static void state_req(Conn *conn) {
    while (try_fill_buffer(conn)) {}
}

static void state_res(Conn *conn) {
    while (try_flush_buffer(conn)) {}
}

static void fd_set_nb(int fd){
    unsigned long mode = blocking ? 0 : 1;
    int res=ioctlsocket(fd, FIONBIO, &mode);
    if(res!= NO_ERROR){
        die("nonblocking error");
        return;
    }
}
static void connection_io(Conn *conn) {
    if (conn->state == STATE_REQ) {
        state_req(conn);
    } else if (conn->state == STATE_RES) {
        state_res(conn);
    } else {
        assert(0);  // not expected
    }
}

static void conn_put(std::vector<Conn *> &fd2conn, struct Conn *conn) {
    if (fd2conn.size() <= (size_t)conn->fd) {
        fd2conn.resize(conn->fd + 1);
    }
    fd2conn[conn->fd] = conn;
}

static int32_t accept_new_conn(std::vector<Conn *> &fd2conn, int fd) {
    // accept
    struct sockaddr_in client_addr = {};
    int socklen = sizeof(client_addr);
    int connfd = accept(fd, (struct sockaddr *)&client_addr, &socklen);
    if (connfd < 0) {
        msg("accept() error");
        return -1;  // error
    }

    // set the new connection fd to nonblocking mode
    fd_set_nb(connfd);
    // creating the struct Conn
    struct Conn *conn = (struct Conn *)malloc(sizeof(struct Conn));
    if (!conn) {
        close(connfd);
        return -1;
    }
    conn->fd = connfd;
    conn->state = STATE_REQ;
    conn->rbuf_size = 0;
    conn->wbuf_size = 0;
    conn->wbuf_sent = 0;
    conn_put(fd2conn, conn);
    return 0;
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
    SOCKET fd = socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0) {
        die("socket()");
    }

    int val=1;
    //configure socket such that other server and reuse same port
    setsockopt(fd,SOL_SOCKET,SO_REUSEADDR, (char *) &val, sizeof(val));

    //bind
    struct sockaddr_in addr = {};
    //Ipv4
    addr.sin_family = AF_INET;
    addr.sin_port = ntohs(1234);

    // wildcard address 0.0.0.0
    addr.sin_addr.s_addr = ntohl(0);
    int rv = bind(fd, (const sockaddr *)&addr, sizeof(addr));
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
    msg("Server started listening successfully");

    //all client connections key is fd 
    //this can be a unordered map
    std::vector<Conn*> fd2conn;

    // set the listen fd to nonblocking mode
    fd_set_nb(fd);

    //Event Loop
    std::vector<struct pollfd> poll_args;

    while(true){
        // prepare the arguments of the poll()
        poll_args.clear();
        //the listening fd is put in the first position
        struct pollfd pfd = {fd, POLLIN, 0};
        poll_args.push_back(pfd);

        //connection fds
        for (auto *conn : fd2conn) {
            if(!conn){
                continue;
            }
            struct pollfd pfd = {};
            pfd.fd = conn->fd;
            //for reading state wait till data can be read(POLLIN) with out blocking
            // and for writing till data can be written(POLLOUT) with out blocking
            pfd.events = (conn->state == STATE_REQ) ? POLLIN : POLLOUT;
            poll_args.push_back(pfd);
        }

        // poll for active fds and timeout argument doesn't matter here
        int rv = WSAPoll(poll_args.data(), (ULONG)poll_args.size(), 1000);
        if (rv < 0) {
            die("poll");
        }

        for (size_t i = 1; i < poll_args.size(); ++i) {
            //If revents is set then that is non blocking action
            if (poll_args[i].revents) {
                Conn *conn = fd2conn[poll_args[i].fd];
                connection_io(conn);
                if (conn->state == STATE_END) {
                    // client closed normally, or something bad happened.
                    // destroy this connection
                    fd2conn[conn->fd] = NULL;
                    (void)closesocket(conn->fd);
                    free(conn);
                }
            }
        }

        if (poll_args[0].revents) {
            (void) accept_new_conn(fd2conn, fd);
        }
    }
    WSACleanup();
    return 0;

}