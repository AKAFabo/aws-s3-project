CC      = gcc
CFLAGS  = -Wall -Wextra -g -std=c11 -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L

# Source files
COMMON_SRC  = common/protocol.c
SERVER_SRC  = server/server.c server/bucket.c $(COMMON_SRC)
CLIENT_SRC  = client/client.c $(COMMON_SRC)

# Output binaries
SERVER_BIN  = aws-s3_server
CLIENT_BIN  = aws-s3

.PHONY: all server client clean

all: server client

server: $(SERVER_SRC)
	$(CC) $(CFLAGS) -o $(SERVER_BIN) $(SERVER_SRC)
	@echo "Built: $(SERVER_BIN)"

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC)
	@echo "Built: $(CLIENT_BIN)"

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)
	@echo "Cleaned."
