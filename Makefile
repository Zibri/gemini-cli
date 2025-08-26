# Makefile for the gemini-cli client
# Builds a stripped executable in a single step.
# Handles Windows (.exe) and POSIX executable names automatically.

CC = gcc
TARGET_NAME = gemini-cli
SRC = gemini-cli.c cJSON.c

# --- OS-Specific Target Naming ---
# Default to POSIX style
TARGET = $(TARGET_NAME)
# If on Windows, append .exe
ifeq ($(OS),Windows_NT)
	TARGET = $(TARGET_NAME).exe
	CC = clang
endif

# Compiler and Linker Flags
CFLAGS = -Wall -Wextra -O2
LIBS = -lcurl -lz -lreadline
# Add -s to LDFLAGS to strip the final executable during linking
LDFLAGS = -s

# Utility Commands
RM = rm -f

# --- Build Rules ---

all: clean $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	$(RM) $(TARGET_NAME) *.o

.PHONY: all clean
