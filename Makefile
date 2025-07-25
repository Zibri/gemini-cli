# Makefile for the portable Gemini gemini-cli client
# Automatically detects the OS and builds accordingly.

# --- Common Configuration ---
#CC = gcc
TARGET_NAME = gemini-cli
# Assume cJSON source files are in the current directory
SRC_COMMON = gemini-cli.c cJSON.c
OBJ_COMMON = $(SRC_COMMON:.c=.o)
# Common compiler and linker flags
CFLAGS += -std=c99 -Wall -Wextra -g -O2 -I.
#LDFLAGS =

# --- Platform-Specific Configuration ---

# Default to POSIX (Linux, macOS)
OS_TYPE = POSIX

# Check if we are on Windows
# The 'OS' environment variable is 'Windows_NT' on Windows systems.
ifeq ($(OS),Windows_NT)
	OS_TYPE = WINDOWS
else
	UNAME_S := $(shell uname -s)
endif

# --- Windows Build ---
ifeq ($(OS_TYPE),WINDOWS)
	TARGET = $(TARGET_NAME).exe
	# On Windows, we compile linenoise.c directly into our program
	SRC = $(SRC_COMMON) linenoise.c
	# On Windows, libcurl often needs the sockets and crypto libraries
	LIBS = -lcurl -lz -lws2_32 -lbcrypt
	RM = del /Q
	STRIP = strip -s

# --- POSIX Build ---
else
	TARGET = $(TARGET_NAME)
	# On POSIX, we don't compile linenoise.c
	SRC = $(SRC_COMMON)
	# On POSIX, we link against the installed readline library
	LIBS = -lcurl -lz -lreadline
	RM = rm -f
	ifeq ($(UNAME_S),Darwin)
		STRIP = strip
	else
		STRIP = strip -s
	endif
endif

OBJ = $(SRC:.c=.o)

# --- Build Rules ---

all: clean $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)
	$(STRIP) $@
	$(RM) $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
ifeq ($(OS_TYPE),WINDOWS)
	$(RM) *.o *.exe
else
	$(RM) *.o $(TARGET)
endif

.PHONY: all clean
