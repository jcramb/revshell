////////////////////////////////////////////////////////////////////////////////
// sock.h
// author: jcramb@gmail.com

#ifndef sock_h
#define sock_h

#include <string>
#include <set>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// defines

#define SOCK_INVALID (0)
#define SOCK_SERVER  (1)
#define SOCK_CLIENT  (2)

////////////////////////////////////////////////////////////////////////////////
// helper func proto's

const char * sock_get_ip(std::string iface = "");
int sock_set_blocking(int fd, bool blocking = true);
bool sock_is_blocking(int fd);

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
    int broadcast(const char * buf, int len);
    int bind(int port);
    int poll_accept(int timeout_ms = 0);
    int accept();

    // getters 
    int sock() { return m_sock; }
    int src_port() { return s_port; }
    int dst_port() { return d_port; }
    const char * src_ip() { return s_ip.c_str(); }
    const char * dst_ip() { return d_ip.c_str(); }
    const std::set<int> & client_socks() { return m_client_socks; } 

    // TODO: 
    // class still in refactoring from single client focus
    // to multiple client support - fix artifacts of previous design
    // i.e. dst_port() isn't valid with multiple clients
    // group up sock/port maps for each client instead
    // this is needed to improve proxy addressing 

protected:

    // stream info
    int m_type;
    
    // connection info
    int s_port, d_port;
    std::string s_ip, d_ip;

    // connection state
    int m_sock;
    std::set<int> m_client_socks;
};

////////////////////////////////////////////////////////////////////////////////

#endif // sock_h
