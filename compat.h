#ifndef COMPAT_H
#define COMPAT_H

// This header provides a compatibility layer for functions that differ
// between Windows (_WIN32) and POSIX-compliant systems (Linux, macOS).

#ifdef _WIN32
  // --- Windows-specific definitions ---
  #include <windows.h>
  #include <string.h> // For _stricmp
  #include <direct.h> // For _getcwd, _fullpath

  // Map POSIX function names to their Windows counterparts.
  #define strcasecmp _stricmp
  #define getcwd _getcwd

  // Define PATH_MAX if it's not already defined on Windows.
  #ifndef PATH_MAX
    #define PATH_MAX MAX_PATH
  #endif

  // Provide a wrapper for realpath(), as Windows uses _fullpath() with a
  // different signature. This wrapper mimics the POSIX behavior where
  // passing NULL for resolved_path allocates memory for the result.
  static inline char* realpath(const char* path, char* resolved_path) {
      char* full_path = _fullpath(NULL, path, MAX_PATH);
      if (full_path == NULL) {
          return NULL; // Path does not exist or another error occurred.
      }
      if (resolved_path != NULL) {
          strncpy(resolved_path, full_path, MAX_PATH);
          free(full_path);
          return resolved_path;
      }
      return full_path;
  }

#else
  // --- POSIX-specific definitions ---
  #include <unistd.h>     // For getcwd, realpath, STDIN_FILENO
  #include <limits.h>     // For PATH_MAX
  #include <strings.h>    // For strcasecmp
  #include <termios.h>    // For terminal control (secure input)
#endif

#endif // COMPAT_H
