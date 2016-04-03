////////////////////////////////////////////////////////////////////////////////
// core.h
// author: jcramb@gmail.com

#ifndef core_h
#define core_h

#ifdef __APPLE__    
    #include <util.h> // forkpty (POSIX - ain't nobody got time for dat)
#else
    #include <pty.h>
#endif

#include <string>
#include <memory>
#include <map>
#include <set>

////////////////////////////////////////////////////////////////////////////////
// types / defines

#define TPT_CLIENT 2
#define TPT_SERVER 1
#define TPT_CLOSE  0
#define TPT_EMPTY -1
#define TPT_ERROR -2

#define LOG_BUFSIZE 8192
#define LOG_FILE (1<<1)
#define LOG_ECHO (1<<2)

#define MSG_INVALID (0)
#define MSG_RVSHELL (1)
#define MSG_WNDSIZE (2)
#define MSG_PROXY_INIT (3)
#define MSG_PROXY_PASS (4)
#define MSG_PROXY_FAIL (5)
#define MSG_PROXY_DATA (6)
#define MSG_PROXY_DEAD (7)

#ifndef MAX
#define MAX(a, b) (a > b ? a : b)
#endif

#ifndef MIN
#define MIN(a, b) (a < b ? a : b)
#endif

#ifndef CLAMP
#define CLAMP(val, min, max) MAX(min, MIN(max, val))
#endif

#ifndef LOG_DISABLE
#define LOG(...) log_print(__VA_ARGS__)
#else
#define LOG(...) 
#endif

// type proto
class transport;
class message;

// func proto
void log_init(const char * prefix, int flags);
void log_flags(int flags = 0);
void log_print(const char * fmt, ...);
void hexdump(const char * buf, int len, int cols = 16, bool ascii = true); 

////////////////////////////////////////////////////////////////////////////////
// abstract transport interface

class transport {
public:

    // ctors / dtors
    transport() {}
    virtual ~transport() {}

    // abstract interface
    virtual int init(int type) = 0;
    virtual int send(message & msg) = 0;
    virtual int recv(message & msg) = 0;
    virtual void setopt(int opt, std::string value) = 0;
    virtual void close() = 0;
};

////////////////////////////////////////////////////////////////////////////////
// transport message container

class message {
public:

    // class 
    enum { header_len = sizeof(int) * 2 }; 
    enum { body_max_len = 1024 }; 

    // ctors / dtors
    message(int type = MSG_INVALID, size_t len = 0);
    message(int type, const char * buf, size_t len);
    message(const char * buf, size_t len);

    // buffer getters
    char * data();
    char * body();
    const char * data() const;
    const char * body() const;
    size_t data_len() const;
    size_t body_len() const;

    // misc getters / setters
    int type() const;
    size_t resize(size_t len);

protected:

    // internal data struct
    struct {
        int type;
        int body_len;
        char body[body_max_len];
    } m_data;
};

////////////////////////////////////////////////////////////////////////////////

#endif // core_h

