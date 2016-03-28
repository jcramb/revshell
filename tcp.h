////////////////////////////////////////////////////////////////////////////////
// tcp.h
// author: jcramb@gmail.com

#ifndef tcp_h
#define tcp_h

#include <string>

////////////////////////////////////////////////////////////////////////////////

std::string get_ip(std::string iface = "");

class tcp_stream {
public:
    tcp_stream();
    ~tcp_stream();

    int send(const char * buf, int len);
    int recv(char * buf, int len, bool waitall = false);

    int connect(std::string host, int port);
    int bind(int port);
    int accept();
    void close();

    int src_port() { return s_port; }
    int dst_port() { return d_port; }
    std::string src_ip() { return s_ip; }
    std::string dst_ip() { return d_ip; }

protected:
    int m_sock;
    int s_port, d_port;
    std::string s_ip, d_ip;
};

////////////////////////////////////////////////////////////////////////////////

#endif // tcp_h
