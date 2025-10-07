### **Version 2.3.2**

This is a usability and bugfix release that improves the behavior of the `/savelast` command.

*   **Fixes & Improvements:**
    *   **Input Trimming:** Trailing whitespace is now automatically trimmed from the filename argument for the `/savelast` command.
    *   **Usage Message:** The `/savelast` command now prints a helpful usage message if a filename is not provided.

### **Version 2.3.1**

This is a quality-of-life and security release that adds more explicit control over interactive mode and improves input handling.

*   **Features:**
    *   **Force Interactive Mode:** A new `--interactive` (or `-i`) flag has been added to force the application to run in interactive mode, even when stdin or stdout are redirected.
*   **Fixes & Improvements:**
    *   **Input Trimming:** Trailing whitespace is now automatically trimmed from session names and file paths when saving, preventing unexpected behavior.
    *   **Security:** Added a buffer overflow check to the session path construction logic for improved stability and security.

### **Version 2.3.0**

This is a feature release that gives users control over media processing to help manage token usage.

*   **Features:**
    *   **Media Resolution Control:** New command-line flags have been added to control the resolution at which the API processes media attachments (e.g., images). This is useful for managing token consumption.
        *   `--ml`: Sets media processing to **low** resolution.
        *   `--mm`: Sets media processing to **medium** resolution.

### **Version 2.2.9**

This is a usability release that adds convenient short aliases for common session management commands.

*   **Features:**
    *   **New Command Aliases:**
        *   `--list-sessions` can now be shortened to `--sl`.
        *   `--load-session` can now be shortened to `--ls`.
        *   `--save-session` can now be shortened to `--ss`.

### **Version 2.2.8**

This is a usability release that adds convenient short aliases for common session management commands.

*   **Features:**
    *   **New Command Aliases:**
        *   `--list-sessions` can now be shortened to `--sl`.
        *   `--save-session` can now be shortened to `--ss`.
        *   `--load-session` can now be shortened to `--ls`.

### **Version 2.2.7**

This is a major refactoring and simplification release that standardizes the project on a POSIX-compliant codebase and build system, removing native Windows support in favor of improved maintainability and a more robust interactive editing experience via the `readline` library.

*   **Major Refactoring & POSIX Standardization:**
    *   **Windows Support Removed:** All Windows-specific compatibility code (`#ifdef _WIN32`), API calls, and the `compat.h` header have been removed. The project now targets POSIX-compliant environments like Linux, macOS, and Windows Subsystem for Linux (WSL).