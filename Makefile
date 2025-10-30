# Makefile for the server and client programs

# Compiler and flags
CC = gcc
CFLAGS = -o2 -Wall
LDFLAGS = -pthread

# Source files
SERVER_SRC = server.c
CLIENT_SRC = client.c

# Output binaries
SERVER_BIN = server
CLIENT_BIN = client

# Default target
all: $(SERVER_BIN) $(CLIENT_BIN)

# Rule to build the server
$(SERVER_BIN): $(SERVER_SRC)
	$(CC) $(CFLAGS) $(SERVER_SRC) -o $(SERVER_BIN) $(LDFLAGS)

# Rule to build the client
$(CLIENT_BIN): $(CLIENT_SRC)
	$(CC) $(CFLAGS) $(CLIENT_SRC) -o $(CLIENT_BIN) $(LDFLAGS)

# Clean up build artifacts
clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)

# Rule to clean and rebuild everything
rebuild: clean all

# Install (example - copies the binaries to /usr/local/bin)
install: $(SERVER_BIN) $(CLIENT_BIN)
	install -m 755 $(SERVER_BIN) /usr/local/bin
	install -m 755 $(CLIENT_BIN) /usr/local/bin

.PHONY: all clean rebuild install
