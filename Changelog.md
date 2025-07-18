### **Changelog**

#### **Version 1.0.1**

This version introduced foundational features for giving users more control over the API's behavior and improved the robustness of configuration handling.

*   **Features**
    *   **Grounding and URL Context Control:** Added `google_grounding` and `url_context` flags. Users can now disable these features using the `--no-grounding` (`-ng`) and `--no-url-context` (`-nu`) command-line arguments.
    *   **Enhanced Configuration:** The client can now read all settings, including the new grounding and URL context flags, from the JSON configuration file.
    *   **Safe JSON Parsing:** Implemented new helper functions (`json_read_*`) to safely read different data types from the configuration file, preventing crashes from malformed JSON.
*   **Improvements**
    *   API requests now only include the `googleSearch` and `urlContext` tools if they are enabled in the application's state.

#### **Version 1.0.2**

This release focused on improving the "thinking budget" feature and providing better feedback to the user upon startup.

*   **Improvements**
    *   **Automatic Thinking Budget:** The default thinking budget is now set to "automatic" (`-1`), allowing the Gemini API to manage it unless overridden by the user. The `/budget 0` command in an interactive session now resets the budget to automatic.
    *   **Startup Information:** The client now clearly displays the status (ON/OFF) of Google Search grounding and URL context processing when an interactive session begins.
    *   **Removed Model-Specific Budget:** Removed the logic that automatically set a fixed thinking budget based on the model name, favoring the new "automatic" default.

#### **Version 1.0.3**

This was a critical bugfix release that addressed a major issue in how the client handled responses from the API.

*   **Fixes**
    *   **Multi-Part Response Handling:** Fixed a significant bug where only the first part of a multi-part API response was being displayed. The client now correctly processes and prints all parts, ensuring the complete output from the model is shown.
    *   **Cleaner Shell Output:** A final newline is now consistently added after a response is received in non-interactive mode, improving integration with shell scripts.

#### **Version 1.0.4**

This version marked a major internal refactoring to streamline the codebase, reduce duplication, and simplify API communication logic.

*   **Refactoring**
    *   **Centralized API Logic:** All API request logic was consolidated into a single `send_api_request` function. This function now handles JSON creation, compression, request execution, and error handling for both interactive (streaming) and non-interactive modes, significantly reducing code duplication.
    *   **Simplified Response Handling:** The main interactive and non-interactive functions were refactored to use the new centralized API function, simplifying their logic. Manual JSON response parsing in the non-interactive mode was removed in favor of the unified streaming callback.

#### **Version 1.0.5**

This version delivered a host of user experience improvements for the interactive mode and further refactored the codebase for better maintainability.

*   **Features**
    *   **Centralized Option Parsing:** A new `parse_common_options` function was created to handle all standard command-line arguments (e.g., `--model`, `--temp`) in one place for both modes.
    *   **Improved Interactive Commands:**
        *   `/system`, `/budget`, and `/maxtokens` can now be used without an argument to display their current values.
        *   A new `/temp` command was added to allow viewing and changing the temperature during a session.
*   **Improvements**
    *   **Accurate Token Count:** The `/stats` command now includes pending file attachments in its token calculation, giving a more accurate preview of the context size.
    *   **Secure Session Management:** A new `build_session_path` function was introduced to validate session names and construct file paths safely, preventing path traversal vulnerabilities.
*   **Refactoring**
    *   Redundant argument-parsing code was removed from the main functions in favor of the new `parse_common_options` function.
