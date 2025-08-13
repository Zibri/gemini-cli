### **Version 2.2.1**

This is a reliability and accuracy release that refines the new "Deep Mode" feature and improves token calculation.

*   **Improvements:**
    *   **Model-Aware Context Limits:** The `/deep` and `/deeper` commands are now smarter. They automatically adjust the internal thinking budget to `16384` for Flash models and `32768` for other models, ensuring that the refinement process respects the context window of the specific model being used.
    *   **More Accurate Token Counting:** The client now removes safety settings from the payload before sending it to the `countTokens` endpoint. This results in a more accurate token count and prevents potential errors.

### **Version 2.2.0**

This is a major feature release that introduces "Deep Mode," an innovative, multi-step prompting technique designed to elicit higher-quality, more refined responses from the model.

*   **Features:**
    *   **New `/deep <prompt>` Command:** This command initiates an automated, two-step refinement process. It first generates an initial answer to your prompt, then silently asks the model to critique and improve its own response. Finally, it synthesizes both into a single, optimal answer. This entire process is invisible to the user, who only sees the final, high-quality output.
    *   **New `/deeper <prompt>` Command:** An enhanced version of `/deep` that uses a three-step refinement process for even more thorough and robust answers. It generates a response, critiques it, critiques the critique, and then synthesizes all three into a final solution.
    *   **Dynamic Temperature Control:** The deep modes automatically use a schedule of increasing temperatures (e.g., 0.2, then 1.0 for `/deep`) for each step. This encourages the model to generate a precise initial answer and then use more creativity for the subsequent improvement and synthesis steps.
*   **Improvements:**
    *   **Silent Operation:** Implemented a cross-platform method to suppress all console output during the intermediate steps of the `/deep` and `/deeper` commands, ensuring a clean and focused user experience.

### **Version 2.1.0**

This is a usability and feature enhancement release focused on streamlining the workflow for YouTube video analysis and improving the utility of conversation exports.

*   **Features:**
    *   **Automatic YouTube URL Handling:** In interactive mode, you can now paste a YouTube URL directly into the chat. The client will automatically detect the URL, attach it as context for the next prompt, and remove it from the input text. This eliminates the need to attach URLs as separate command-line arguments.
    *   **Enhanced Markdown Export:** The `/export` command has been improved. It now decodes and embeds the content of any `text/plain` file attachments directly into the exported Markdown file. This is especially useful for preserving text that was pasted or piped into a session, making the exported history more complete and readable.
*   **Improvements:**
    *   A new `base64_decode` utility function was added to support the improved export functionality.

### **Version 2.0.9**

This is a user-safety and content filtering release that gives users explicit control over the API's safety settings.

*   **Features:**
    *   **Default Safety-Off:** The client now automatically disables the API's safety filters by default. It sends `BLOCK_NONE` thresholds for `HARM_CATEGORY_HARASSMENT`, `HARM_CATEGORY_HATE_SPEECH`, `HARM_CATEGORY_SEXUALLY_EXPLICIT`, and `HARM_CATEGORY_DANGEROUS_CONTENT`. This prevents the API from preemptively blocking responses, which is often desirable in a direct command-line interface.
    *   **Opt-In Safety Flag:** A new `--safety` command-line flag has been introduced. When this flag is used, the client does *not* send the custom safety settings, thereby re-enabling the Gemini API's default content filtering. This allows users to opt back into the default safety measures if they prefer.

### **Version 2.0.8**

This is a major feature release that introduces the ability to use YouTube videos as direct context for analysis.

*   **Features:**
    *   **YouTube URL Attachments:** You can now provide a YouTube URL as a command-line argument, just like a file path. The client will automatically recognize it and attach it as context for your query.
    *   This allows the model to directly analyze the content of the YouTube video without requiring you to download it first.
    *   Example: `gemini-cli "https://www.youtube.com/watch?v=dQw4w9WgXcQ" "What is this video about?"`
*   **Improvements:**
    *   **New Attachment Type:** The internal data structures were updated to support URI-based attachments (`PART_TYPE_URI`), which are sent to the API using the `fileData` field.
    *   **Enhanced Attachment Management:** All interactive commands for managing attachments (`/attach list`, `/attach remove`, `/history files`) have been updated to correctly display and manage URL attachments alongside traditional file attachments.

### **Version 2.0.7**

This is a reliability release that expands file type support for common audio formats.

*   **Improvements:**
    *   **Expanded MIME Type Detection:** The client can now correctly identify the MIME types for several common audio file formats:
        *   `.wav` is now recognized as `audio/wav`.
        *   `.mp3` is now recognized as `audio/mpeg`.
        *   `.ogg` is now recognized as `audio/ogg`.
    *   This improves the tool's ability to handle audio file attachments correctly when interacting with the Gemini API.

### **Version 2.0.6**

This is a usability and flexibility release that refines how the maximum output token limit is managed by introducing a more flexible "automatic" mode.

*   **Improvements:**
    *   **Automatic Token Limit:** The `max_output_tokens` setting now defaults to an "automatic" mode (`0`). When active, the client omits the `maxOutputTokens` parameter from the API request, allowing the Gemini API to use its own default limit. This provides more flexibility and avoids setting an arbitrary high limit.
    *   **Enhanced `/maxtokens` Command:** The interactive `/maxtokens` command has been improved.
        *   You can now set `/maxtokens 0` to explicitly enable the new automatic mode.
        *   Using `/maxtokens` without an argument will now correctly report the status, printing "Max output tokens: automatic" when it's enabled.
    *   **Smarter Configuration:** The `max_output_tokens` value is no longer saved to `config.json` when set to automatic. This ensures that the application's default behavior is preserved across sessions unless a user explicitly sets a specific limit.

### **Version 2.0.5**

This is a flexibility and developer-focused release that introduces custom API host support.

*   **Features:**
    *   **Custom API Host:** The client can now be configured to send API requests to a custom host instead of the default `generativelanguage.googleapis.com`. This is a powerful feature for users who need to target specific regional endpoints or use the client with self-hosted or proxy-based API environments.
    *   **Configuration Methods:** The API host can be set in multiple ways for maximum flexibility:
        *   **Command-Line Flag:** Use the `-H` or `--host` flag to set the host for a single run.
        *   **Interactive Command:** Use the new `/host <hostname>` command to change the host during an interactive session. Use `/host` to view the current setting.
        *   **Configuration File:** The `host` setting is now saved to and loaded from the `config.json` file.

### **Version 2.0.4**

This is a reliability and accuracy release focused on improving file type detection.

*   **Improvements:**
    *   **Smarter MIME Type Detection:** The `get_mime_type` function was completely rewritten to be content-aware. It now intelligently analyzes the first few bytes of a file (the "magic bytes") to accurately identify common image formats (`jpeg`, `png`, `gif`, `webp`) or to heuristically determine if a file is plain text.
    *   **Fallback to Extension:** If the content analysis is inconclusive (e.g., the file is empty or doesn't have a recognized signature), the function falls back to the previous method of checking the file extension.
    *   **Robustness:** This new approach makes file handling more reliable, as it no longer depends solely on the file's name. It correctly identifies files even if they have an incorrect or missing extension.

### **Version 2.0.3**

This is a user experience release that streamlines the handling of file attachments in interactive mode.

*   **Improvements:**
    *   **Automatic Loading of Command-Line Attachments:** When starting an interactive session, any files attached via command-line arguments (e.g., `gemini-cli report.txt`) are now immediately loaded into the chat history. Previously, a user had to enter an initial prompt to process the files. This makes the workflow more intuitive, allowing users to attach files and instantly begin asking questions about them.

### **Version 2.0.2**

This is a usability and reliability release focused on improving how the tool handles standard input (`stdin`) across different operating systems.

*   **Features:**
    *   **Direct `stdin` Piping:** You can now use a single hyphen (`-`) as a command-line argument to treat `stdin` as a file to be attached. This provides a conventional and powerful way to pipe data from other commands (e.g., `cat report.txt | gemini-cli - "Summarize this for me" `).
*   **Improvements:**
    *   **Interactive Windows Input:** The client now correctly handles multi-line input pasted directly into an interactive Windows terminal. It prints a helpful message and properly detects the end of input when you press `Ctrl+Z` and then `Enter`, fixing a major usability issue.
    *   **Robust Stream Handling:** The logic for reading from streams was completely rewritten. It now intelligently differentiates between regular files and non-seekable streams (like pipes or the terminal), using the most efficient method for each.
    *   **Cross-Platform End-of-File:** The underlying code now uses platform-specific methods to reliably detect the end of input on both Windows (`Ctrl+Z`) and Unix-like systems (`Ctrl+D`), ensuring consistent behavior everywhere.
*   **Refactoring:**
    *   The `handle_attachment_from_stream` function was refactored to separate the logic for file reading vs. pipe reading, making the code cleaner and easier to maintain.

### **Version 2.0.1**

This is a major feature release that introduces robust, multi-key management to the client. The tool can now store and rotate through multiple API keys, improving reliability and making it easier to manage rate limits.

*   **Features:**
    *   **Multi-Key Support:** The client can now manage an array of API keys and their corresponding origins.
        *   **Automatic Key Rotation:** The client automatically cycles to the next available key for each API request, distributing usage.
        *   **Configuration:** The `config.json` file now stores keys and origins in an array (`"api_keys": [...]`, `"origins": [...]`). The client maintains backward compatibility and will automatically read the old single-key format.
    *   **Comprehensive Key Management:**
        *   **Command-Line Flags:**
            *   `--list-keys`: List all keys stored in the configuration file.
            *   `--add-key`: Interactively prompts to add a new key and origin to the configuration.
            *   `--remove-key <index>`: Removes a key from the configuration by its index.
            *   `--check-keys`: Validates all configured keys by making a test API call with each one.
        *   **Interactive Commands:**
            *   `/keys list`: List the keys currently loaded for the session.
            *   `/keys add <key>`: Add a new key for the current session (use `/config save` to make it permanent).
            *   `/keys remove <index>`: Remove a key by its index for the current session.
            *   `/keys check`: Validate all keys currently loaded in the session.
*   **Improvements:**
    *   **Smarter Error Handling:** If an API call fails with a `403 Unauthorized` error, the client now reports which specific key failed, making it easier to identify and remove invalid keys.
*   **Refactoring:**
    *   **Command-Line Parsing:** The entire command-line option parsing logic was refactored into a more robust and maintainable system using an `enum` and a `switch` statement, replacing a long series of `if-else` blocks.
    *   **State Management:** The central `AppState` struct was updated to handle arrays of keys and origins instead of single static fields. All related functions for API requests, configuration, and cleanup were updated accordingly.

### **Version 2.0.0**

This is a landmark release that introduces an unofficial, key-free API mode, making the tool accessible to everyone. It also adds significant features for robustness, scripting, and connectivity, alongside a major internal refactoring to improve code quality and maintainability.

*   **Major Features:**
    *   **Key-Free API Mode (`-f`, `--free`):** A new "free mode" has been added that uses an unofficial Google API endpoint. This allows users to chat with the model without needing an API key, making the tool instantly usable.
    *   **Proxy Support (`-p`, `--proxy`):** The client can now route all API requests through a specified HTTP/HTTPS proxy, enabling usage in restricted network environments.
    *   **Enhanced Scripting & Non-Interactive Mode:**
        *   **Quiet Mode (`-q`, `--quiet`):** A new quiet flag suppresses all informational output to `stderr`, printing only the final model response to `stdout`.
        *   **Execute Mode (`-e`, `--execute`):** A new execute flag forces a non-interactive run for a single prompt, even if `stdin` and `stdout` are connected to a terminal.
    *   **Location & Map Fetching (`--loc`, `--map`):** New experimental flags (for use with `--free` mode) to fetch location information and a corresponding map URL.

*   **Reliability & Robustness:**
    *   **Automatic API Retries:** All API calls (for generating content, listing models, and counting tokens) now automatically retry up to 3 times upon receiving a `503 Service Unavailable` error. This makes the client significantly more resilient to transient server issues.
    *   **Production-Ready Attachment Handling:** The `handle_attachment_from_stream` function was completely rewritten for production-level robustness. It now uses safer memory management, handles both regular files and piped streams (like `stdin`) more reliably, and includes a `goto`-based cleanup pattern to prevent resource leaks.
    *   **Safer Path and Session Management:** Path construction logic has been hardened to pre-calculate buffer sizes, preventing potential overflows.

*   **Refactoring & Code Quality:**
    *   **Massive Code Documentation:** The entire codebase has been documented with Doxygen-style comments, explaining the purpose, parameters, and return values of nearly every function.
    *   **Platform-Specific Logic:** Added system language detection and locale normalization for better cross-platform compatibility, which is used by the new free API mode.
    *   **Unified Main Loop:** The main `generate_session` function was heavily refactored to accommodate the new free mode, scripting flags, and a more complex interactive command structure.

### **Initial Version (Pre-1.0.0)**

This was the foundational version of the `gemini-cli` tool, establishing the core interactive chat functionality.

*   **Features:**
    *   Interactive chat session using a `readline`-like interface.
    *   Basic command-line argument parsing for model, temperature, seed, max output tokens, and thinking budget.
    *   Support for loading and saving chat history to JSON files.
    *   Ability to attach files as context for the model.
    *   Loading of API key and other settings from a configuration file or environment variables.

### **Version 1.0.0**

This version introduced a significant refactoring to improve command-line argument handling and make the initial experience more intuitive.

*   **Refactoring:**
    *   **Centralized Option Parsing:** A new `parse_common_options` function was created to handle all standard command-line arguments (e.g., `--model`, `--temp`) in one place for both interactive and non-interactive modes. This removed redundant parsing logic from the main functions.
    *   **File Handling:** The client can now intelligently handle command-line arguments that are file paths, automatically treating them as attachments.
*   **Improvements:**
    *   **Help Message:** A `print_usage` function was added to display a helpful message with all available options when the user provides the `-h` or `--help` argument.

### **Version 1.0.1**

This version introduced foundational features for giving users more control over the API's behavior and improved the robustness of configuration handling.

*   **Features:**
    *   **Grounding and URL Context Control:** Added `google_grounding` and `url_context` flags. Users can now disable these features using the `--no-grounding` (`-ng`) and `--no-url-context` (`-nu`) command-line arguments.
    *   **Enhanced Configuration:** The client can now read all settings, including the new grounding and URL context flags, from the JSON configuration file.
    *   **Safe JSON Parsing:** Implemented new helper functions (`json_read_*`) to safely read different data types from the configuration file, preventing crashes from malformed JSON.
*   **Improvements:**
    *   API requests now only include the `googleSearch` and `urlContext` tools if they are enabled in the application's state.

### **Version 1.0.2**

This release focused on improving the "thinking budget" feature and providing better feedback to the user upon startup.

*   **Improvements:**
    *   **Automatic Thinking Budget:** The default thinking budget is now set to "automatic" (`-1`), allowing the Gemini API to manage it unless overridden by the user. The `/budget 0` command in an interactive session now resets the budget to automatic.
    *   **Startup Information:** The client now clearly displays the status (ON/OFF) of Google Search grounding and URL context processing when an interactive session begins.
    *   **Removed Model-Specific Budget:** Removed the logic that automatically set a fixed thinking budget based on the model name, favoring the new "automatic" default.

### **Version 1.0.3**

This was a critical bugfix release that addressed a major issue in how the client handled responses from the API.

*   **Fixes:**
    *   **Multi-Part Response Handling:** Fixed a significant bug where only the first part of a multi-part API response was being displayed. The client now correctly processes and prints all parts, ensuring the complete output from the model is shown.
    *   **Cleaner Shell Output:** A final newline is now consistently added after a response is received in non-interactive mode, improving integration with shell scripts.

### **Version 1.0.4**

This version marked a major internal refactoring to streamline the codebase, reduce duplication, and simplify API communication logic.

*   **Refactoring:**
    *   **Centralized API Logic:** All API request logic was consolidated into a single `send_api_request` function. This function now handles JSON creation, compression, request execution, and error handling for both interactive (streaming) and non-interactive modes, significantly reducing code duplication.
    *   **Simplified Response Handling:** The main interactive and non-interactive functions were refactored to use the new centralized API function, simplifying their logic. Manual JSON response parsing in the non-interactive mode was removed in favor of the unified streaming callback.

### **Version 1.0.5**

This version delivered a host of user experience improvements for the interactive mode and further refactored the codebase for better maintainability.

*   **Features:**
    *   **Improved Interactive Commands:**
        *   `/system`, `/budget`, and `/maxtokens` can now be used without an argument to display their current values.
        *   A new `/temp` command was added to allow viewing and changing the temperature during a session.
*   **Improvements:**
    *   **Accurate Token Count:** The `/stats` command now includes pending file attachments in its token calculation, giving a more accurate preview of the context size.
    *   **Secure Session Management:** A new `build_session_path` function was introduced to validate session names and construct file paths safely, preventing path traversal vulnerabilities.
*   **Refactoring:**
    *   Redundant argument-parsing code was removed from the main functions in favor of the `parse_common_options` function.

### **Version 1.0.6**

This version focused on improving the command-line experience by allowing users to provide an initial prompt directly as an argument and enhancing mode detection.

*   **Features:**
    *   **Initial Prompt from Arguments:** Users can now provide a prompt directly on the command line (e.g., `gemini-cli "What is the capital of France?"`). The client will send this prompt to the API and then enter an interactive session.
*   **Improvements:**
    *   **Smarter Mode Detection:** The client now checks if both `stdin` and `stdout` are connected to a terminal to decide whether to run in interactive mode. This prevents it from getting stuck waiting for input if either is redirected.
    *   **Cleaner Output:** The newline before the API response is now only printed in interactive mode, resulting in cleaner output for scripting.

### **Version 1.0.7**

This release marked a final major refactoring of the main application logic, unifying the interactive and non-interactive modes into a single, more maintainable function.

*   **Refactoring:**
    *   **Unified Main Function:** The separate `generate_interactive_session` and `generate_non_interactive_response` functions were merged into a single `generate_session` function that accepts a boolean `interactive` flag. This eliminates significant code duplication and simplifies the program's control flow.
    *   **Code Cleanup:** The now-unused `generate_non_interactive_response` function was removed, and related code was cleaned up to reflect the unified structure.
*   **Improvements:**
    *   **Consistent Behavior:** Both modes now share the exact same setup, argument parsing, and API request logic, ensuring more consistent behavior regardless of how the tool is invoked.

### **Version 1.0.8**

This version introduced significant robustness improvements to file handling and further refined the internal structure of the code.

*   **Improvements:**
    *   **Robust Attachment Handling:** The `handle_attachment_from_stream` function was completely rewritten to be more robust. It now includes better error checking, safer memory allocation, proper handling of empty files, and clearer error messages.
*   **Refactoring:**
    *   **Centralized cURL Logic:** A new `perform_api_curl_request` function was created to abstract the core cURL execution, further reducing code duplication within the API-calling functions (`send_api_request`, `get_token_count`).
    *   **Path Management:** The logic for finding the application's configuration directory was centralized into a `get_base_app_path` function, simplifying path construction throughout the application.

### **Version 1.0.9**

This version added new features for exploring the API's capabilities and exporting conversations, along with more granular control over generation parameters.

*   **Features:**
    *   **Model Listing:** A new `/models` command was added to fetch and display a list of all available models from the API.
    *   **Markdown Export:** A `/export <file.md>` command was introduced to save the entire conversation history into a human-readable Markdown file.
    *   **Top-K and Top-P Control:** Users can now set the `topK` and `topP` sampling parameters via command-line arguments (`--topk`, `--topp`) or interactive commands (`/topk`, `/topp`).
*   **Improvements:**
    *   The help message (`/help`) was updated to include all the new commands.
    *   Configuration loading was updated to support the new `top_k` and `top_p` settings.

### **Version 1.0.10**

This version added more interactive commands for managing configuration and settings, and improved the overall usability of the tool.

*   **Features:**
    *   **Interactive Configuration Management:** New commands `/config save` and `/config load` allow saving the current settings to the `config.json` file or reloading them during a session.
    *   **Custom Configuration Path:** A `-c` or `--config` command-line flag allows users to specify a custom path for the configuration file.
    *   **Interactive Grounding and URL Context Control:** New commands `/grounding [on|off]` and `/urlcontext [on|off]` were added to toggle Google Search grounding and URL context fetching during a session.
*   **Improvements:**
    *   **README Update:** The `README.md` file was updated to reflect all new features, commands, and the change of the executable name to `gemini-cli`.
    *   **Enhanced Help Message:** The `/help` command output was expanded to include the new configuration and settings commands.

### **Version 1.0.11**

This version focused on improving the security and user experience of entering credentials.

*   **Improvements:**
    *   **Secure API Key Input:** The `get_api_key_securely` function was completely rewritten. It now properly disables terminal echoing while the user types their API key. Instead of a blank screen, it prints an asterisk (`*`) for each character typed, providing visual feedback without exposing the key on-screen. It also correctly handles backspaces for editing.

### **Version 1.0.12**

This is a bugfix and reliability release focused on improving piped input and simplifying the main application logic.

*   **Fixes:**
    *   **Correct Ctrl+D Handling:** The `handle_attachment_from_stream` function was changed to use a low-level `read()` call instead of `fread()`. This fixes a critical bug where pressing `Ctrl+D` in an interactive terminal to signal the end of input was not being correctly detected due to stdio buffering.
*   **Refactoring:**
    *   **Simplified Mode Detection:** The logic in the `main` function for determining whether to run in interactive or non-interactive mode was simplified. It now uses a single boolean variable, `is_interactive`, making the code cleaner and the control flow easier to understand.

### **Version 1.0.13**

This version adds command-line session management features for a more flexible workflow.

*   **Features:**
    *   **Command-Line Session Management:**
        *   `--list-sessions`: A new flag to list all saved sessions directly from the command line and exit.
        *   `--load-session <name>`: A new flag to load a previous chat session by name upon starting the client.
*   **Improvements:**
    *   **Enhanced Help Message:** The `print_usage` function was updated to include the new session management flags.
    *   **Refined Mode Detection:** The check for non-interactive mode was improved to trigger if either input or output is piped.

### **Version 1.0.14**

This is a reliability and user experience release that improves argument handling, cross-platform support, and the initial setup process.

*   **Improvements:**
    *   **Robust Argument Handling:** Command-line arguments are now checked more reliably to differentiate between file paths and prompt text. The client now attempts to open arguments as files, and only if that fails or the path points to a non-regular file (like a directory), is it treated as part of the initial prompt.
    *   **Interactive Origin Input:** The secure credential input process has been enhanced. If the API key is not set by an environment variable or config file, the tool will now prompt for the `Origin` immediately after prompting for the API key, streamlining the first-time setup.
    *   **Cross-Platform Path Compatibility:** Session file paths are now constructed with the correct directory separator (`/` or `\`) based on the operating system, improving reliability on Windows.

### **Version 1.0.15**

This release enhances security and refactors the credential input process for better code maintainability.

*   **Improvements:**
    *   **Secure Origin Input:** The prompt for the API `Origin` now masks user input with asterisks, similar to the API key prompt. This prevents credentials from being accidentally exposed on-screen during the initial setup.
*   **Refactoring:**
    *   **Modularized Secure Input:** A new `get_masked_input` helper function was created to handle all terminal-masked input. This abstracts the platform-specific (Windows/Unix) logic for disabling terminal echo.
    *   **Simplified Credential Logic:** The `get_api_key_securely` function was refactored to use the new `get_masked_input` helper, which reduces code duplication and makes the credential-gathering process cleaner and more secure.