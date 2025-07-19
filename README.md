# Gemini-CLI: A Portable Command-Line Client for the Google Gemini API

**Gemini-CLI** is a powerful, portable, and feature-rich command-line client for the Google Gemini API. It provides a fluid, shell-like interface for conversing with large language models, designed for developers, writers, and power-users who live in the terminal.

It is written in C for maximum performance and portability, with no dependencies beyond standard system libraries and a few common open-source projects.

## Features

*   **Dual-Mode Operation:** Functions as both a fully featured **interactive chat client** and a non-interactive, **scriptable command-line tool**. The mode is automatically detected based on whether input is being piped to the program.
*   **Scripting & Piping:** Pipe content directly to the program (`cat file | gemini-cli "summarize this"`) or provide prompts as arguments for seamless integration into shell scripts and automation.
*   **Streaming Responses:** In interactive mode, see the model's response generated in real-time, just like in web UIs.
*   **File Attachments:** Attach images, source code, PDFs, and other files to your prompts. Supports local files (`/attach`) and pasting directly from stdin (`/paste`).
*   **Session Management:** Save, load, list, and delete entire conversation sessions, allowing you to easily switch between different projects and contexts. The current session name is always visible in the prompt.
*   **Conversation History:** Your conversation is maintained in memory. You can export the entire chat to a JSON file (`/save`), a Markdown file (`/export`), and import it later (`/load`).
*   **History Management:** List and selectively remove individual file attachments from the current conversation history.
*   **System Prompts:** Guide the model's behavior for the entire session with a persistent system prompt (`/system`).
*   **Secure & Configurable:**
    *   Securely prompts for your API key without echoing it to the screen.
    *   Supports configuration via environment variables (`GEMINI_API_KEY`, `GEMINI_API_KEY_ORIGIN`).
    *   **Handles Origin-Restricted Keys:** Set the `Origin` header via environment variable or config file, a crucial feature for using API keys secured by HTTP referrers.
    *   Can be configured via a JSON file (`config.json`) for persistence of model, temperature, seed, API key, and other settings.
    *   Load configurations from a custom path using the `-c` flag.
    *   Save the current session's settings to your configuration file with `/config save`.
*   **Cross-Platform:** Designed to compile and run seamlessly on both POSIX systems (Linux, macOS) and Windows.
*   **Efficient:** Uses Gzip compression for all API requests to reduce network latency and bandwidth.
*   **Informative:** Get session statistics, including the total token count of your conversation context (including pending attachments), with the `/stats` command.
*   **Model Exploration:** List all available models from the API with the `/models` command.
*   **Advanced Generation Control:** Fine-tune the model's output with temperature, `topK`, `topP`, and other parameters, both at startup and interactively during a session.
*   **API Feature Toggling:** Interactively enable or disable Google Search grounding and URL context processing.

## Getting Started

### 1. Clone the Repository
First, get the source code using git:```bash
git clone https://github.com/Zibri/gemini-cli.git
cd gemini-cli
```

### 2. Install Prerequisites
You will need a C compiler (`gcc` or `clang`) and the development headers for the following libraries:

*   **cURL:** For making HTTP requests.
*   **zlib:** For Gzip compression.
*   **readline (POSIX only):** For advanced line editing and persistent history.

**On Debian/Ubuntu:**
```bash
sudo apt-get update
sudo apt-get install build-essential libcurl4-openssl-dev libreadline-dev zlib1g-dev
```

**On openSUSE/Tumbleweed:**
```bash
sudo zypper refresh
sudo zypper install gcc make libcurl-devel readline-devel zlib-devel
```

**On Fedora/CentOS/RHEL:**
```bash
sudo dnf install gcc make curl-devel readline-devel zlib-devel
```

**On macOS (using Homebrew):**
```bash
brew install curl readline zlib
# You may need to provide linker flags if they aren't found automatically```

**On Windows:**
The easiest way to build on Windows is by using a POSIX-like environment such as [MSYS2](https://www.msys2.org/). Once you have MSYS2 installed, open the **UCRT64** terminal and install the necessary packages:
```bash
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-make mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-zlib
```

### 3. Compile the Program
The project includes a `Makefile` that automatically detects your operating system and compiles the program with the correct settings. Simply run:
```bash
make
```
This will create an executable named `gemini-cli` (or `gemini-cli.exe` on Windows). You can move this file to a directory in your system's `PATH` (e.g., `/usr/local/bin` or `~/bin`) for easy access.

### 4. Configure Your API Key
You need to provide your Google Gemini API key. You can get one from [Google AI Studio](https://aistudio.google.com/app/apikey). The client will load settings in the following order of priority: **1. Environment Variable**, **2. Configuration File**, **3. Interactive Prompt**.

1.  **Environment Variable (Highest Priority):**
    Set the `GEMINI_API_KEY` environment variable.
    ```bash
    export GEMINI_API_KEY="your_api_key_here"
    ```
    **For Restricted Keys (Optional):**
    If you have secured your API key to an HTTP referrer, set the `GEMINI_API_KEY_ORIGIN` environment variable. This sends the required `Origin` HTTP header.
    ```bash
    export GEMINI_API_KEY_ORIGIN="https://my-app.com"
    ```

2.  **Configuration File:**
    The client will look for a `config.json` file. You can create this file to set a default model, temperature, API key, or system prompt.

    *   **POSIX:** `~/.config/gemini-cli/config.json`
    *   **Windows:** `%APPDATA%\gemini-cli\config.json`

    *Example `config.json`:*
    ```json
    {
      "api_key": "your_api_key_here",
      "origin": "https://your-allowed-origin.com",
      "model": "gemini-2.5-flash",
      "temperature": 0.75,
      "seed": 42,
      "system_prompt": "You are a helpful and concise assistant.",
      "google_grounding": true,
      "url_context": true,
      "max_output_tokens": 8192,
      "top_k": 40,
      "top_p": 0.95
    }
    ```

3.  **Interactive Prompt (Lowest Priority):**
    If no key is found, the program will securely prompt you to enter it when it first runs in interactive mode.

## Usage

### Command-Line Arguments
You can control the model and generation parameters at startup using these flags. Any other arguments are treated as an initial prompt or file attachments.

| Flag | Alias | Description | Example |
|---|---|---|---|
| `-h`, `--help` | | Show the help message and exit. | `./gemini-cli --help` |
| `-c`, `--config` | | Load configuration from a specific file path. | `./gemini-cli -c /path/to/myconfig.json` |
| `-m`, `--model` | | Specify the model name to use. | `./gemini-cli -m gemini-2.5-pro` |
| `-t`, `--temp` | | Set the generation temperature (e.g., 0.0 to 2.0). | `./gemini-cli -t 0.25` |
| `-s`, `--seed` | | Set the generation seed for reproducible outputs. | `./gemini-cli -s 1234` |
| `-o`, `--max-tokens` | | Set the maximum number of tokens in the response. | `./gemini-cli -o 2048` |
| `-b`, `--budget` | | Set the model's max 'thinking' token budget. | `./gemini-cli -b 8192` |
| | `--topk` | Set the Top-K sampling parameter. | `./gemini-cli --topk 40` |
| | `--topp` | Set the Top-P sampling parameter. | `./gemini-cli --topp 0.95` |
| `-ng`, `--no-grounding` | | Disable Google Search grounding. | `./gemini-cli -ng` |
| `-nu`, `--no-url-context`| | Disable URL context processing. | `./gemini-cli -nu` |

### Interactive Mode
To start a conversation, simply run the executable. This is the default mode when not piping data. You can combine flags with an initial prompt and files to attach. The program will process these and then drop you into an interactive session:
```bash
# Start a simple session
./gemini-cli

# Start with a specific model and an initial prompt
./gemini-cli -m gemini-2.5-pro "Tell me about the C programming language."

# Start with initial attachments and a prompt
./gemini-cli my_image.png my_code.py "Describe the code and the image."```

### Non-Interactive / Scripting Mode
Gemini-CLI automatically enters non-interactive mode if you pipe data to it.

**Piping Content:**
The piped content is treated as a text attachment, and any arguments are used as the prompt.
```bash
# Summarize a source file
cat my_complex_function.c | ./gemini-cli "Explain what this C code does in simple terms"

# Generate a git commit message from the staged changes
git diff --staged | ./gemini-cli "Write a concise, imperative git commit message for these changes"
```

### Interactive Commands

Type `/help` at the prompt to see a list of available commands.

| Command | Description |
| --- | --- |
| **General** | |
| `/help` | Show this help message. |
| `/exit`, `/quit` | Exit the program. |
| `/clear` | Clear the current conversation history and any pending attachments. |
| `/stats` | Show session statistics (model, temperature, token count, etc.). |
| `/models` | List all available models from the API. |
| `/config <save\|load>` | Save the current settings to the config file or load them from it. |
| **Conversation Control** | |
| `/system [prompt]` | Set or show the system prompt that influences the model's behavior. |
| `/clear_system` | Remove the system prompt. |
| `/temp [value]` | Set or show the temperature. |
| `/maxtokens [value]`| Set or show the maximum output tokens. |
| `/budget [value]` | Set or show the max thinking budget (0 for automatic). |
| `/topk [value]` | Set or show the topK sampling parameter. |
| `/topp [value]` | Set or show the topP sampling parameter. |
| `/grounding [on\|off]` | Set or show the status of Google Search grounding. |
| `/urlcontext [on\|off]`| Set or show the status of URL context fetching. |
| **Attachments & I/O** | |
| `/attach <file> [prompt]` | Attach a file. You can optionally add a text prompt on the same line. |
| `/paste` | Paste text from stdin as a `text/plain` attachment (Ctrl+D/Ctrl+Z to end). |
| `/savelast <file.txt>`| Save only the last model response to a text file. |
| `/save <file.json>` | (Export) Save the current conversation history to a JSON file. |
| `/load <file.json>` | (Import) Load a conversation history from a JSON file. |
| `/export <file.md>` | Export the conversation to a human-readable Markdown file. |
| **Pending Attachment Management** | |
| `/attachments list` | List all pending attachments for the next prompt. |
| `/attachments remove <index>`| Remove a pending attachment by its index. |
| `/attachments clear` | Remove all pending attachments. |
| **History Management** | |
| `/history attachments list` | List all file attachments currently in the conversation history, with their IDs. |
| `/history attachments remove <id>` | Remove an attachment from history using its ID (e.g., `2:1`). |
| **Session Management** | |
| `/session new` | Start a new, unsaved session (same as `/clear`). |
| `/session list` | List all saved conversation sessions. |
| `/session save <name>` | Save the current chat history to a named session. (Note: name cannot contain `/`, `\`, or `.`) |
| `/session load <name>` | Load a conversation from a named session. |
| `/session delete <name>` | Delete a named session. |

**Note on Session Storage:** Saved sessions are stored as `.json` files inside a `sessions` subdirectory within your configuration folder (e.g., `~/.config/gemini-cli/sessions/`).

## License

This project is licensed under the Creative Commons Attribution-NonCommercial-ShareAlike 4.0

## Acknowledgments

This tool would not be possible without these fantastic open-source libraries:
*   [cURL](https://curl.se/) for robust network transfers.
*   [cJSON](https://github.com/DaveGamble/cJSON) for easy JSON parsing.
*   [zlib](https://zlib.net/) for data compression.
*   [readline](https://tiswww.case.edu/php/chet/readline/rltop.html) for a superior command-line experience on POSIX.
*   [linenoise](https://github.com/antirez/linenoise) for a lightweight readline alternative on Windows.