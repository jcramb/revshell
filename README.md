# revshell

### Upcoming Changes
* TCP proxy support
* Cleaned up libvterm (remove glib requirement) 

### Compiling for Linux
1. install dependencies as per your distro
   * python (2 or 3)
   * libssl-dev
   * ncurses-dev
2. make
3. ... profit

### Compiling for OSX
1. brew install python
2. brew install gsl
3. brew install glib
4. make 
5. ... profit

### Features:
* Written in C/C++
* Terminal emulation (vt100) using modified libvterm
* Communications encrypted using OpenSSL (TLSv1)
* Tested on Arch/Kali/Ubuntu and OSX

### Goals:
* (done) OSX or Linux endpoint control
* (done) Connects back and provides a shell to a hardcoded C2
* (done) Provides full tty emulation
* (done) Communication is encrypted

### Stretch Goals:
* (done) Communications to C2 mimic legitimate traffic (i.e. SSL on port 443)
* (done) Platform independent across OSX and Linux
* (todo) Ability to proxy traffic through endpoint

### Screenshots:

![revshell_osx](https://github.com/jcramb/revshell/blob/master/screenshots/revshell_osx.png)

![revshell_linux](https://github.com/jcramb/revshell/blob/master/screenshots/revshell_linux.png)

### Helpful Resources:
* https://sourceforge.net/projects/libvterm/
* http://stackoverflow.com/questions/16382252/how-to-render-a-remote-ncurses-console
* http://www.linusakesson.net/programming/tty/
* http://stackoverflow.com/questions/11705815/client-and-server-communication-using-ssl-c-c-ssl-protocol-dont-works
* http://www.lundman.net/cvs/viewvc.cgi/lundman/lion/src/tls.c?revision=1.10&view=markup
* http://funcptr.net/2012/04/08/openssl-as-a-filter-%28or-non-blocking-openssl%29/
* http://h71000.www7.hp.com/doc/83final/ba554_90007/ch04s03.html
* http://stackoverflow.com/questions/16381628/openssl-non-blocking-sockets-ssl-read-still-blocks

## Documentation

TBC.
