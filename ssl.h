////////////////////////////////////////////////////////////////////////////////
// ssl.h
// author: jcramb@gmail.com

#ifndef ssl_h
#define ssl_h

#include <string>
#include <openssl/ssl.h>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// defines

#define SSL_CRTFILE "/root/revshell/vterm/ca/shell_crt.pem"
#define SSL_KEYFILE "/root/revshell/vterm/ca/shell_key.pem"
#define SSL_DEFAULT_HOST "127.0.0.1"
#define SSL_DEFAULT_PORT 443

////////////////////////////////////////////////////////////////////////////////
// func proto

const char * get_ip(std::string iface = "");
void ssl_set_host(std::string ip);
void ssl_set_port(int port);
const char * ssl_get_host();
int ssl_get_port();

////////////////////////////////////////////////////////////////////////////////
// OpenSSL implementation of transport interface

class ssl_stream : public transport {
public:

    // ctors / dtors
    ssl_stream();
    virtual ~ssl_stream();

    // transport interface
    virtual int init(int type);
    virtual int send(message & msg);
    virtual int recv(message & msg);

    // networking methods
    int connect(std::string host, int port);
    int bind(int port);
    int accept();
    void close();

    // getters 
    int src_port() { return s_port; }
    int dst_port() { return d_port; }
    const char * src_ip() { return s_ip.c_str(); }
    const char * dst_ip() { return d_ip.c_str(); }

protected:

    // networking members
    int m_sock;
    int s_port, d_port;
    std::string s_ip, d_ip;

    // ssl members
    SSL_CTX * ctx;
    SSL * ssl;
};

////////////////////////////////////////////////////////////////////////////////

#endif // ssl_h
