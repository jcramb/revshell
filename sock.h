////////////////////////////////////////////////////////////////////////////////
// sock.h
// author: jcramb@gmail.com

#ifndef sock_h
#define sock_h

#include <netinet/in.h>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// defines

#define SOCK_INVALID (0)
#define SOCK_SERVER  (1)
#define SOCK_CLIENT  (2)
#define SOCK_LIMIT   (-3)

////////////////////////////////////////////////////////////////////////////////
// helper func / type proto's

const char * sock_get_ip(std::string iface = "");
int sock_set_blocking(int fd, bool blocking = true);
bool sock_is_blocking(int fd);

struct sock_info {
    char s_ip[INET6_ADDRSTRLEN];
    char d_ip[INET6_ADDRSTRLEN];
    int s_port;
    int d_port;

    sock_info(std::string s_ip = "", int s_port = -1, 
              std::string d_ip = "", int d_port = -1);
};

////////////////////////////////////////////////////////////////////////////////
// tcp socket

class tcp_stream {
public:

    // ctors / dtors
    tcp_stream();
    ~tcp_stream();

    // common functions 
    int send(const char * buf, int len, int sock = -1);
    int recv(char * buf, int len, int sock = -1);
    void close(int sock = -1);

    // client functions
    int connect(std::string host, int port);

    // server functions
    void conn_limit(int limit = -1);
    void disconnect_clients();
    int broadcast(const char * buf, int len);
    int bind(int port);
    int poll_accept(int timeout_ms = 0);
    int accept();

    // getters 
    int sock() { return m_sock; }
    const std::set<int> & client_socks() { return m_client_socks; } 
    const sock_info * sockinfo(int sock = -1);
    const char * src_ip(int sock = -1);
    const char * dst_ip(int sock = -1);
    int src_port(int sock = -1);
    int dst_port(int sock = -1);

protected:

    // stream info
    int m_type;
    int m_connlimit;
    
    // connection state
    int m_sock;
    std::set<int> m_client_socks;
    std::map<int, std::shared_ptr<sock_info>> m_sockinfo;
};

////////////////////////////////////////////////////////////////////////////////

#endif // sock_h
