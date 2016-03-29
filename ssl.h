////////////////////////////////////////////////////////////////////////////////
// ssl.h
// author: jcramb@gmail.com

#ifndef ssl_h
#define ssl_h

#include <openssl/ssl.h>

#include <string>

#include "core.h"
#include "sock.h"

////////////////////////////////////////////////////////////////////////////////
// defines

#define SSL_OPT_HOST 1
#define SSL_OPT_PORT 2

////////////////////////////////////////////////////////////////////////////////
// OpenSSL implementation of transport interface

class ssl_transport : public transport {
public:

    // ctors / dtors
    ssl_transport();
    virtual ~ssl_transport();

    // transport interface
    virtual int init(int type);
    virtual int send(message & msg);
    virtual int recv(message & msg);
    virtual void setopt(int opt, std::string value);
    virtual void close();

protected:

    // transport options
    int m_opt_port;
    std::string m_opt_host;

    // socket wrapper
    int m_ssl_sock;
    tcp_stream m_tcp;

    // openssl members
    SSL * m_ssl;
    SSL_CTX * m_ctx;
};

////////////////////////////////////////////////////////////////////////////////

#endif // ssl_h
