////////////////////////////////////////////////////////////////////////////////
// sock.h
// author: jcramb@gmail.com

#ifndef sock_h
#define sock_h

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// defines

#define SOCK_SERVER 1
#define SOCK_CLIENT 2

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
    int send(const char * buf, int len);
    int recv(char * buf, int len);
    void close();

    // client functions
    int connect(std::string host, int port);

    // server functions
    int bind(int port);
    int accept();

    // getters 
    int sock() { return m_sock; }
    int src_port() { return s_port; }
    int dst_port() { return d_port; }
    const char * src_ip() { return s_ip.c_str(); }
    const char * dst_ip() { return d_ip.c_str(); }

protected:

    // networking members
    int m_sock;
    int s_port, d_port;
    std::string s_ip, d_ip;
};

////////////////////////////////////////////////////////////////////////////////

#endif // sock_h
