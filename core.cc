////////////////////////////////////////////////////////////////////////////////
// core.cc
// author: jcramb@gmail.com

#include <string>
#include <cstring>
#include <cstdarg>
#include <cstdio>
#include <ctype.h>

#include "core.h"

////////////////////////////////////////////////////////////////////////////////
// debugging log global vars

int g_logflags = 0;
std::string g_logpath;

////////////////////////////////////////////////////////////////////////////////
// set log flags and filename

void log_init(const char * prefix, int flags) {
    char * p = (char*)prefix;
    while (!isalpha(*p)) p++;
    g_logpath = ".";
    g_logpath += p;
    g_logpath += "_log";
    g_logflags = flags;
}

////////////////////////////////////////////////////////////////////////////////
// debug log printing (to file / echo)

void log_print(const char * fmt, ...) {
    char logbuf[LOG_BUFSIZE] = {0};
    static bool created = false;

    va_list va;
    va_start(va, fmt);
    vsnprintf(logbuf, LOG_BUFSIZE, fmt, va);
    va_end(va);

    if (g_logflags & LOG_ECHO) {
        printf("%s", logbuf);
        fflush(stdout);
    }

    if (g_logflags & LOG_FILE) {
        FILE * f;
        if (created) {
            f = fopen(g_logpath.c_str(), "a");
        } else {
            f = fopen(g_logpath.c_str(), "w");
            created = true;
        }
        if (f) {
            fprintf(f, "%s", logbuf);
            fclose(f);
        } else {
            fprintf(stderr, "WARNING: failed to open logfile\n");
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
// prints binary buffer in hex + ascii format

void hexdump(const char * buf, int len, int cols, bool ascii) {
    int pos = 0;
    int bytes_left = len;
    const int word_size = 4;
    while (bytes_left) {

        // determine how many columns for this row
        int c = MIN(bytes_left, cols);
        bytes_left -= c;
        LOG(" %4x:", pos);

        // print hex bytes
        for (int i = 0; i < c; i++) {
            if (i % word_size == 0) LOG(" ");
            LOG(" %02x", buf[pos+i] & 0xff);
        }

        // do we need to add the ascii column?
        if (ascii) {
            
            // pad spacing for missing hex bytes
            int empty_bytes = cols - c;
            for (int i = 0; i <= empty_bytes; i++) {
                LOG("   ");
            }

            // pad for missing word column spacing 
            for (int i = 0; i < empty_bytes / word_size; i++) {
                LOG(" ");
            }

            // print ascii bytes 
            for (int i = 0; i < c; i++) {
                LOG("%c", isprint(buf[pos+i]) ? buf[pos+i] & 0xff : '.');
            }
        }

        pos += c;
        LOG("\n");
    }
}

////////////////////////////////////////////////////////////////////////////////
// transport message implementation

message::message(int type, size_t len) {
    memset(&m_data, 0, sizeof(m_data));
    m_data.type = type;
    resize(len);
}

message::message(int type, const char * buf, size_t len) {
    m_data.type = type;
    resize(len);
    memcpy(body(), buf, body_len());
}

message::message(const char * buf, size_t len) {
    memcpy(data(), buf, header_len);
    m_data.body_len = MAX(0, MIN(len, body_max_len));
    memcpy(body(), buf + header_len, m_data.body_len);
}

char * message::data() {
    return (char*)&m_data;
}
const char * message::data() const {
    return (char*)&m_data;
}
char * message::body() {
    return m_data.body;
}
const char * message::body() const {
    return m_data.body;
}
int message::type() const {
    return m_data.type;
}
size_t message::data_len() const {
    return header_len + m_data.body_len;
}
size_t message::body_len() const {
    return m_data.body_len;
}
size_t message::resize(size_t len) {
    m_data.body_len = MIN(len, body_max_len);
    return m_data.body_len;
}

////////////////////////////////////////////////////////////////////////////////
