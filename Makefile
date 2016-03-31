CXXFLAGS = -std=c++11 -Wno-write-strings
LDFLAGS = 

all: CXXFLAGS += -ggdb
final: CXXFLAGS += -O2 -DLOG_DISABLE

CC = g++
INCLUDE = $(shell pkg-config --cflags glib-2.0)
BUILD_DIR = build
COMMON_SRC = cert.cc core.cc sock.cc ssl.cc proxy.cc
SERVER_SRC = server.cc $(COMMON_SRC) vterm.cc
CLIENT_SRC = client.cc $(COMMON_SRC)
SERVER_OBJ = $(SERVER_SRC:%.cc=$(BUILD_DIR)/%.o)
CLIENT_OBJ = $(CLIENT_SRC:%.cc=$(BUILD_DIR)/%.o)
SERVER_LIB += -lutil -lncurses -lglib-2.0 -lssl -lcrypto
CLIENT_LIB += -lutil -lssl -lcrypto
CERTS = cert.h cert.cc

MKCERT = ./bin2cc.py cert crt:ca/shell_crt.pem key:ca/shell_key.pem 

.PHONY: all final mkdir clean

all: mkdir server client
final: clean mkdir server client

cert.h: 
	@$(MKCERT) 

cert.cc: 
	@$(MKCERT) 

server: $(CERTS) $(SERVER_OBJ)
	@echo LINK $@ 
	@$(CC) $(CXXFLAGS) $(LDFLAGS) $(SERVER_OBJ) -o $@ $(SERVER_LIB)

client: $(CERTS) $(CLIENT_OBJ)
	@echo LINK $@ 
	@$(CC) $(CXXFLAGS) $(LDFLAGS) $(CLIENT_OBJ) -o $@ $(CLIENT_LIB)

$(BUILD_DIR)/%.o: %.cc
	@echo CC $<
	@$(CC) $(CXXFLAGS) $(INCLUDE) -c $< -o $@

mkdir: $(BUILD_DIR)
$(BUILD_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
	rm -f $(CERTS)
	rm -f server
	rm -f client
	rm -f .*_log
