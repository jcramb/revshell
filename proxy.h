////////////////////////////////////////////////////////////////////////////////
// proxy.h
// author: jcramb@gmail.com

#ifndef proxy_h
#define proxy_h

#include <netinet/in.h>

#include <map>
#include <memory>
#include <string>

#include "core.h"
#include "sock.h" 

#define PROXY_BUFSIZE 8192

////////////////////////////////////////////////////////////////////////////////
// message helper functions

struct proxy_header {
    char s_ip[INET6_ADDRSTRLEN];
    char d_ip[INET6_ADDRSTRLEN];
    int s_port;
    int d_port;
};
    
void set_proxy_header(proxy_header * h, std::string s_ip, int s_port, 
                                        std::string d_ip, int d_port);

////////////////////////////////////////////////////////////////////////////////
// tcp proxy client (downstream)

class proxy_listener {
public:
    proxy_listener();
    ~proxy_listener();

    int enable(int s_port, std::string d_ip, int d_port);
    void disable(int s_port);

    int poll(transport & tpt, int timeout_ms = 0);
    int handle_msg(message & msg);
    void close();

protected:
    std::map<int, std::shared_ptr<tcp_stream>> m_streams;
    std::map<int, std::shared_ptr<proxy_header>> m_headers;
};

////////////////////////////////////////////////////////////////////////////////
// tcp proxy server (upstream)

class proxy_dispatch {
public:
    proxy_dispatch();
    ~proxy_dispatch();

    int poll(int timeout_ms = 0);
    int handle_msg(message & msg);
    void close();

protected:
};

////////////////////////////////////////////////////////////////////////////////
// basic tcp proxy (blocking, single route connection)

class tcp_proxy {
public:

    // ctors / dtors
    tcp_proxy();
    ~tcp_proxy();

    // proxy interface
    int bind(int s_port);
    int establish(std::string d_ip, int d_port);
    int poll();
    void close();

protected:
    bool m_active;

    // poll state
    int u_len, d_len;
    int u_sock, d_sock;
    char u_buf[PROXY_BUFSIZE];
    char d_buf[PROXY_BUFSIZE];

    // proxy streams
    tcp_stream m_upstream;
    tcp_stream m_downstream;
};

////////////////////////////////////////////////////////////////////////////////

#endif // proxy_h
