/**
 * @file gemini-cli.c
 * @brief An interactive, portable command-line client for the Google Gemini API.
 *
 * This program provides a feature-rich, shell-like interface for conversing
 * with the Gemini large language model. It supports conversation history,
 * configurable models and temperature, file attachments (including paste),
 * system prompts, Gzip compression, graceful error handling, and full
 * line-editing capabilities. It can be configured via a file in
 * ~/.config/gemini-cli/config.json (POSIX) or
 * %APPDATA%\gemini-cli\config.json (Windows).
 *
 * It is designed to be portable between POSIX systems and Windows.
 * gcc -s -O3 gemini-cli.c cJSON.c -o gemini-cli -lcurl -lz -lreadline
 * or
 * clang -s -O3 gemini-cli.c cJSON.c -o gemini-cli -lcurl -lz -lreadline
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <locale.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <zlib.h>
#include "cJSON.h"

#include <limits.h>

// --- Portability Layer ---
#ifdef _WIN32
  #include <windows.h>
  #include <direct.h>
  #include "linenoise.h"
  #include <conio.h>
  #define MKDIR(path) _mkdir(path)
  #define STRCASECMP _stricmp
  #define PATH_MAX MAX_PATH
  #define stat _stat
#else
  #include <unistd.h>
  #include <termios.h>
  #include <limits.h>
  #include <readline/readline.h>
  #include <readline/history.h>
  #include <dirent.h> 
  #define MKDIR(path) mkdir(path, 0755)
  #define STRCASECMP strcasecmp
#endif

// --- Configuration Constants ---
#define DEFAULT_MODEL_NAME "gemini-2.5-pro"
#define API_URL_FORMAT "https://%s/v1beta/models/%s:%s"
#define FREE_API_URL "https://gemini.google.com/_/BardChatUi/data/assistant.lamda.BardFrontendService/StreamGenerate?bl=&f.sid=&hl=en&_reqid=&rt=c"
#define GZIP_CHUNK_SIZE 16384
#define ATTACHMENT_LIMIT 1024
#define MAX_FREE_MODE_CONTEXT_SIZE 102400

// --- Data Structures ---
typedef struct { unsigned char* data; size_t size; } GzipResult;
typedef enum { PART_TYPE_TEXT, PART_TYPE_FILE } PartType;
typedef struct { PartType type; char* text; char* mime_type; char* base64_data; char* filename; } Part;
typedef struct { char* role; Part* parts; int num_parts; } Content;
typedef struct { Content* contents; int num_contents; } History;
typedef struct { char* buffer; size_t size; char* full_response; size_t full_response_size; } MemoryStruct;
typedef struct AppState {
    char api_key[128];
    char origin[128];
    char** api_keys;
    char** origins;
    int num_api_keys;
    int next_key_index;
    char model_name[128];
    char proxy[256];
    float temperature;
    int max_output_tokens;
    int thinking_budget;
    bool google_grounding;
    bool url_context;
    History history;
    char* last_model_response;
    char* system_prompt;
    Part attached_parts[ATTACHMENT_LIMIT];
    int num_attached_parts;
    int seed;
    int topK;
    float topP;
    char current_session_name[128];
    bool free_mode;
    char* last_free_response_part;
    unsigned int loc_tile;
    bool loc_gathered;
    char* save_session_path;
    char* final_code;
    char *host;
} AppState;

typedef struct {
    MemoryStruct* mem;
    AppState* state;
} FreeCallbackData;

// --- Forward Declarations ---
void save_history_to_file(AppState* state, const char* filepath);
void load_history_from_file(AppState* state, const char* filepath);
void add_content_to_history(History* history, const char* role, Part* parts, int num_parts);
void free_history(History* history);
void free_content(Content* content);
int get_token_count(AppState* state);
char* base64_encode(const unsigned char* data, size_t input_length);
const char* get_mime_type(const char* filename);
GzipResult gzip_compress(const unsigned char* input_data, size_t input_size);
cJSON* build_request_json(AppState* state);
bool is_path_safe(const char* path);
void get_api_key_securely(AppState* state);
void parse_and_print_error_json(const char* error_buffer);
void load_configuration(AppState* state);
void get_config_path(char* buffer, size_t buffer_size);
void handle_attachment_from_stream(FILE* stream, const char* stream_name, const char* mime_type, AppState* state);
void get_sessions_path(char* buffer, size_t buffer_size);
bool is_session_name_safe(const char* name);
void list_sessions();
void clear_session_state(AppState* state);
static size_t write_to_memory_struct_callback(void* contents, size_t size, size_t nmemb, void* userp);
void free_pending_attachments(AppState* state);
void initialize_default_state(AppState* state);
void print_usage(const char* prog_name);
int parse_common_options(int argc, char* argv[], AppState* state);
static void json_read_string(const cJSON* obj, const char* key, char* buffer, size_t buffer_size);
static void json_read_float(const cJSON* obj, const char* key, float* target);
static void json_read_int(const cJSON* obj, const char* key, int* target);
static void json_read_bool(const cJSON* obj, const char* key, bool* target);
static void json_read_strdup(const cJSON* obj, const char* key, char** target);
bool send_api_request(AppState* state, char** full_response_out);
bool build_session_path(const char* session_name, char* path_buffer, size_t buffer_size);
long perform_api_curl_request(AppState* state, const char* endpoint, const char* compressed_payload, size_t payload_size, size_t (*callback)(void*, size_t, size_t, void*), void* callback_data);
void export_history_to_markdown(AppState* state, const char* filepath);
void list_available_models(AppState* state);
void save_configuration(AppState* state);
void load_configuration_from_path(AppState* state, const char* filepath);
void get_masked_input(const char* prompt, char* buffer, size_t buffer_size);

bool send_free_api_request(AppState* state, const char* prompt);
static void process_free_line(char* line, AppState* state);
static size_t write_free_memory_callback(void* contents, size_t size, size_t nmemb, void* userp);
char* build_free_request_payload(AppState* state, const char* current_prompt, bool is_pro_model);

/**
 * @brief Parses a single line from the API's streaming response.
 * @details This function is designed to handle a Server-Sent Event (SSE)
 *          line from the Gemini API. It looks for lines starting with "data: ",
 *          parses the following JSON, extracts the text content, prints it to
 *          stdout, and appends it to the full response buffer.
 * @param line The null-terminated string containing the line to process.
 * @param mem A pointer to the MemoryStruct which holds the buffer for the
 *            complete model response. The `full_response` field will be updated.
 */
static void process_line(char* line, MemoryStruct* mem) {
    // We are only interested in lines that are part of the SSE data stream.
    if (strncmp(line, "data: ", 6) != 0) {
        return;
    }

    cJSON* json_root = cJSON_Parse(line + 6);
    if (!json_root) return;

    cJSON* candidates = cJSON_GetObjectItem(json_root, "candidates");
    if (!cJSON_IsArray(candidates)) {
        cJSON_Delete(json_root);
        return;
    }

    cJSON* candidate = cJSON_GetArrayItem(candidates, 0);
    if (!candidate) {
        cJSON_Delete(json_root);
        return;
    }

    cJSON* content = cJSON_GetObjectItem(candidate, "content");
    if (!content) {
        cJSON_Delete(json_root);
        return;
    }

    cJSON* parts = cJSON_GetObjectItem(content, "parts");
    if (!cJSON_IsArray(parts)) {
        cJSON_Delete(json_root);
        return;
    }

    cJSON* part = cJSON_GetArrayItem(parts, 0);
    if (!part) {
        cJSON_Delete(json_root);
        return;
    }

    cJSON* text = cJSON_GetObjectItem(part, "text");
    if (cJSON_IsString(text) && text->valuestring) {
        // Print the incoming text chunk to the user in real-time.
        printf("%s", text->valuestring);
        fflush(stdout);

        // Append the chunk to the complete response buffer.
        size_t text_len = strlen(text->valuestring);
        char* new_full_response = realloc(mem->full_response, mem->full_response_size + text_len + 1);

        if (new_full_response) {
            mem->full_response = new_full_response;
            memcpy(mem->full_response + mem->full_response_size, text->valuestring, text_len);
            mem->full_response_size += text_len;
            mem->full_response[mem->full_response_size] = '\0';
        } else {
            fprintf(stderr, "\nError: realloc failed while building full response.\n");
        }
    }

    cJSON_Delete(json_root);
}

/**
 * @brief A libcurl write callback function for handling streaming API data.
 * @details This function is called by libcurl whenever new data is received from
 *          the API stream. It appends the new data to a buffer and then
 *          processes the buffer line-by-line, which is crucial for handling
 *          Server-Sent Events (SSE). Any partial line at the end of the buffer
 *          is preserved for the next call.
 * @param contents A pointer to the data received from the stream.
 * @param size The size of each data member (always 1 for text streams).
 * @param nmemb The number of data members received.
 * @param userp A user-defined pointer, which in this case points to a
 *              MemoryStruct to store the buffered data and the final response.
 * @return The total number of bytes (realsize) successfully handled. Returning a
 *         different value will signal an error to libcurl.
 */
static size_t write_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;

    // Expand the buffer to hold the new data.
    char* ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Error: realloc failed in stream callback.\n");
        return 0; // Returning 0 signals an error to libcurl.
    }
    mem->buffer = ptr;

    // Append the new data to the end of the buffer.
    memcpy(mem->buffer + mem->size, contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = '\0'; // Ensure the buffer is always null-terminated.

    // Process the buffer line by line, as SSE sends data in chunks separated by newlines.
    char* line_end;
    while ((line_end = strchr(mem->buffer, '\n')) != NULL) {
        // Temporarily terminate the line at the newline character to treat it as a single string.
        *line_end = '\0';

        // Process the complete line.
        process_line(mem->buffer, mem);

        // Calculate the length of the processed line, including the newline character.
        size_t line_len = (line_end - mem->buffer) + 1;

        // Move the unprocessed part of the buffer (any subsequent lines or partial
        // lines) to the beginning, ready for the next iteration.
        memmove(mem->buffer, line_end + 1, mem->size - line_len);
        mem->size -= line_len;
        mem->buffer[mem->size] = '\0';
    }

    return realsize;
}


// --- Main Application Logic ---
/**
 * @brief Main function to initialize and run a chat session.
 * @details This function orchestrates the entire application lifecycle. It initializes
 *          the application state, loads configuration from files and environment
 *          variables, parses command-line arguments, handles non-interactive
 *          (piped) input, and runs the main interactive command loop.
 * @param argc The argument count from main().
 * @param argv The argument vector from main().
 * @param interactive A boolean indicating if the session is interactive (true) or
 *                    part of a script/pipe (false).
 * @param is_stdin_a_terminal A boolean indicating if stdin is connected to a
 *                            terminal, used to decide if piped input should be read.
 */
void generate_session(int argc, char* argv[], bool interactive, bool is_stdin_a_terminal) {
    AppState state = {0};

    // --- 1. Initialization ---
    // Set all application state variables to their default values.
    initialize_default_state(&state);

    // --- 2. Configuration Loading ---
    // Check for a custom configuration file path provided via command line.
    const char* custom_config_path = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (STRCASECMP(argv[i], "-c") == 0 || STRCASECMP(argv[i], "--config") == 0) {
            custom_config_path = argv[i + 1];
            break;
        }
    }

    // Load from the custom path if provided, otherwise load from the default location.
    if (custom_config_path) {
        load_configuration_from_path(&state, custom_config_path);
        fprintf(stderr, "Loaded configuration from: %s\n", custom_config_path);
    } else {
        load_configuration(&state);
    }

    // --- 3. Argument Processing ---
    // Parse standard options like --model, --temp, etc.
    int first_arg_index = parse_common_options(argc, argv, &state);

    // Buffer to aggregate prompt text from command line arguments.
    char initial_prompt_buffer[16384] = {0};
    size_t initial_prompt_len = 0;

    // Process all remaining arguments. They can be file paths to attach,
    // .json history files to load, or plain text to form an initial prompt.
    for (int i = first_arg_index; i < argc; i++) {

        if (strcmp(argv[i], "-") == 0) {
            // Treat stdin as the file stream to be attached.
            handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
            // Skip the rest of the loop and move to the next argument.
            continue;
        }
        
        // Load conversation history from a .json file.
        if (strlen(argv[i]) > 5 && strcmp(argv[i] + strlen(argv[i]) - 5, ".json") == 0) {
            load_history_from_file(&state, argv[i]);
            continue;
        }

        // Try to open the argument as a file to attach it.
        FILE* file_arg = fopen(argv[i], "rb");
        if (file_arg) {
            struct stat st;
            // Check if it's a regular file.
            if (fstat(fileno(file_arg), &st) == 0 && S_ISREG(st.st_mode)) {
                handle_attachment_from_stream(file_arg, argv[i], get_mime_type(argv[i]), &state);
            } else {
                // If it's not a regular file (e.g., a directory), treat it as prompt text.
                size_t arg_len = strlen(argv[i]);
                if (initial_prompt_len + arg_len + 2 < sizeof(initial_prompt_buffer)) {
                    if (initial_prompt_len > 0) initial_prompt_buffer[initial_prompt_len++] = ' ';
                    strcpy(initial_prompt_buffer + initial_prompt_len, argv[i]);
                    initial_prompt_len += arg_len;
                } else {
                    fprintf(stderr, "Warning: Initial prompt from arguments is too long, argument ignored: %s\n", argv[i]);
                }
            }
            fclose(file_arg); // The file must be closed here.
        } else {
            // If fopen failed, it's not a file path, so treat as prompt text.
            size_t arg_len = strlen(argv[i]);
            if (initial_prompt_len + arg_len + 2 < sizeof(initial_prompt_buffer)) {
                if (initial_prompt_len > 0) initial_prompt_buffer[initial_prompt_len++] = ' ';
                strcpy(initial_prompt_buffer + initial_prompt_len, argv[i]);
                initial_prompt_len += arg_len;
            } else {
                fprintf(stderr, "Warning: Initial prompt from arguments is too long, argument ignored: %s\n", argv[i]);
            }
        }
    }

    // If --loc or --map is used, force free mode and clear any command-line prompt.
    if (state.loc_tile > 0) {
        state.free_mode = true; // These flags are only for free mode.
        if (initial_prompt_len > 0) {
            fprintf(stderr, "Note: --loc/--map used; ignoring initial prompt text.\n");
            initial_prompt_buffer[0] = '\0';
            initial_prompt_len = 0;
        }
        // If no prompt was provided, we still need to trigger an API call.
        // Setting a minimal prompt ensures the request is sent.
        if (initial_prompt_len == 0) {
             strcpy(initial_prompt_buffer, "echo 'hello'");
             initial_prompt_len = 1;
        }
    }
    
    // Enforce model-specific token limits.
    if ((strstr(state.model_name, "flash") != NULL) && (state.thinking_budget > 16384)) {
        state.thinking_budget = 16384;
    }

    // --- 4. Piped Input Handling ---
    // If the session is not interactive and stdin is not a terminal,
    // it means data is being piped in. Treat it as a text attachment.
    if (!interactive && !is_stdin_a_terminal) {
        // If there's no prompt from arguments, the piped data IS the prompt.
        if (initial_prompt_len == 0) {
            size_t bytes_read;
            size_t total_read = 0;
            // Read all of stdin into the initial prompt buffer.
            while ((bytes_read = fread(initial_prompt_buffer + total_read, 1, sizeof(initial_prompt_buffer) - total_read - 1, stdin)) > 0) {
                total_read += bytes_read;
            }
            initial_prompt_buffer[total_read] = '\0';
            initial_prompt_len = total_read;

            // Trim trailing newline from commands like `echo`
            if (initial_prompt_len > 0 && initial_prompt_buffer[initial_prompt_len - 1] == '\n') {
                initial_prompt_buffer[initial_prompt_len - 1] = '\0';
                initial_prompt_len--;
            }
        } else {
            // A prompt already exists, so treat piped data as an attachment.
            handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
        }
    }

    // --- 5. API Key Finalization ---
    // Load API key and origin from environment variables if they exist.
    char* origin_from_env = getenv("GEMINI_API_KEY_ORIGIN");
    if (origin_from_env) {
        strncpy(state.origin, origin_from_env, sizeof(state.origin) - 1);
    }
    char* key_from_env = getenv("GEMINI_API_KEY");

    // The -f flag forces free mode, overriding any API keys.
    if (!state.free_mode) {
        if (key_from_env) {
            // If key is from env, it takes precedence and becomes the only key for this session
            if (state.api_keys) { // Free any keys loaded from config
                for (int i = 0; i < state.num_api_keys; i++) free(state.api_keys[i]);
                free(state.api_keys);
            }
            if (state.origins) { // Free any keys loaded from config
                for (int i = 0; i < state.num_api_keys; i++) free(state.origins[i]);
                free(state.origins);
            }
            state.num_api_keys = 1;
            state.api_keys = malloc(sizeof(char*));
            state.api_keys[0] = strdup(key_from_env);
            state.origins = malloc(sizeof(char*));
            if (origin_from_env) {
                state.origins[0] = strdup(origin_from_env);
            } else {
                state.origins[0] = strdup("default");
            }
            state.next_key_index = 0;
        }

        // If no key is found in config or environment, securely prompt the user.
        if (state.num_api_keys == 0) {
             if (interactive) get_api_key_securely(&state);
             // If still no key after prompting, fall back to free mode.
             if (state.num_api_keys == 0) {
                 state.free_mode = true;
             }
        }
    }

    // Print a startup banner with session settings in interactive mode.
    if (interactive) {
        if (state.free_mode) {
            fprintf(stderr, "--- Running in key-free mode. API key features are disabled. ---\n");
        } else {
            fprintf(stderr,"Using model: %s, Temperature: %.2f, Seed: %d\n", state.model_name, state.temperature, state.seed);
            if (state.max_output_tokens > 0) fprintf(stderr,"Max Output Tokens: %d\n", state.max_output_tokens);
            if (state.thinking_budget > 0) fprintf(stderr,"Thinking Budget: %d tokens\n", state.thinking_budget);
            else fprintf(stderr,"Thinking Budget: automatic\n");
            fprintf(stderr,"Google grounding: %s\n", state.google_grounding?"ON":"OFF");
            fprintf(stderr,"URL Context: %s\n", state.url_context?"ON":"OFF");

            if (key_from_env) fprintf(stderr,"API Key loaded from environment variable.\n");
            else if (state.api_key[0] != '\0') fprintf(stderr,"API Key loaded from configuration file.\n");
            if (origin_from_env) fprintf(stderr,"Origin loaded from environment variable: %s\n", state.origin);
        }
    }

    // --- 6. Initial Prompt Execution ---
    // If a prompt was constructed from command-line args, send it to the API immediately.
    if (initial_prompt_len > 0) {
        if (interactive) fprintf(stderr, "Initial prompt provided. Sending request...\n");

        int total_parts = state.num_attached_parts + 1;
        Part* current_turn_parts = malloc(sizeof(Part) * total_parts);
        if (!current_turn_parts) {
            fprintf(stderr, "Error: Failed to allocate memory for initial prompt parts.\n");
        } else {
            // Combine any pending file attachments with the text prompt.
            for (int j = 0; j < state.num_attached_parts; j++) {
                current_turn_parts[j] = state.attached_parts[j];
            }
            current_turn_parts[state.num_attached_parts] = (Part){ .type = PART_TYPE_TEXT, .text = initial_prompt_buffer };

            add_content_to_history(&state.history, "user", current_turn_parts, total_parts);

            // Clear the pending attachments list as they are now in history.
            free_pending_attachments(&state);
            free(current_turn_parts);

            if (interactive) printf("\n");


            if (state.free_mode) {
                // Handle initial prompt in free mode
                if(state.last_free_response_part) {
                    free(state.last_free_response_part);
                    state.last_free_response_part = NULL;
                }
                if (interactive) printf("\n");
                bool success = send_free_api_request(&state, initial_prompt_buffer);
                if (interactive) printf("\n");

                if (success) {
                    // The user prompt is already in history, now add the model response
                    if (state.last_free_response_part) {
                         Part model_part = { .type = PART_TYPE_TEXT, .text = state.last_free_response_part };
                         add_content_to_history(&state.history, "model", &model_part, 1);
                    }
                } else {
                    // If the API call failed, remove the user's prompt from history.
                    if (state.history.num_contents > 0) {
                        state.history.num_contents--;
                        free_content(&state.history.contents[state.history.num_contents]);
                    }
                }
            } else {
                // Original logic for the official API
                char* model_response_text = NULL;
                if (send_api_request(&state, &model_response_text)) {
                    if (interactive) printf("\n");
                    if (state.last_model_response) free(state.last_model_response);
                    state.last_model_response = model_response_text;

                    Part model_part = { .type = PART_TYPE_TEXT, .text = strdup(state.last_model_response) };
                    add_content_to_history(&state.history, "model", &model_part, 1);
                    free(model_part.text);
                } else {
                    // If the API call failed, remove the user's prompt from history.
                    if (state.history.num_contents > 0) {
                        state.history.num_contents--;
                        free_content(&state.history.contents[state.history.num_contents]);
                    }
                }
            }
        }
    }

    // --- 7. Main Interactive Loop ---
    // This section only runs if the program was launched in interactive mode.
    #ifdef _WIN32
        char history_path[PATH_MAX];
        snprintf(history_path, sizeof(history_path), "%s\\gemini-cli\\history.txt", getenv("APPDATA"));
        linenoiseHistoryLoad(history_path);
    #endif

    if (interactive && state.num_attached_parts > 0 && initial_prompt_len == 0) {
        //fprintf(stderr, "Note: Files attached via command line have been loaded into the history.\n");
        add_content_to_history(&state.history, "user", state.attached_parts, state.num_attached_parts);
        free_pending_attachments(&state);
    }

    if (interactive) {
        char* line;
        char prompt_buffer[16384];

        while (1) {
            // Display the prompt, e.g., "([unsaved])>: "
            snprintf(prompt_buffer, sizeof(prompt_buffer), "\n(%s)>: ", state.current_session_name);

            #ifdef _WIN32
                line = linenoise(prompt_buffer);
                if (line == NULL) break; // EOF on Windows (Ctrl+Z, Enter)
            #else
                line = readline(prompt_buffer);
                if (line == NULL) { // EOF on POSIX (Ctrl+D)
                    printf("\n");
                    break;
                }
            #endif

            // Trim leading whitespace.
            char* p = line;
            while(isspace((unsigned char)*p)) p++;

            // Add non-empty lines to the readline history.
            if (*p) {
                #ifndef _WIN32
                    add_history(line);
                #else
                    linenoiseHistoryAdd(line);
                    linenoiseHistorySave(history_path);
                #endif
            }

            // If the line is empty and there are no attachments, just show a new prompt.
            if (strcmp(p, "") == 0 && state.num_attached_parts == 0) {
                free(line);
                continue;
            }

            // Handle exit commands.
            if (strcmp(p, "/exit") == 0 || strcmp(p, "/quit") == 0) {
                free(line);
                break;
            }

            // Check if the input is a command (starts with '/') or a prompt.
            bool is_command = false;
            if (p[0] == '/') {
                is_command = true;
                char command_buffer[64];
                sscanf(p, "%63s", command_buffer);

                char* arg_start = p + strlen(command_buffer);
                while(isspace((unsigned char)*arg_start)) arg_start++;

                // This large block handles all the different slash commands.
                // (The logic for each command remains the same as the original code).
                if (strcmp(command_buffer, "/help") == 0) {
                    fprintf(stderr,"Commands:\n"
                       "  /help                      - Show this help message.\n"
                       "  /exit, /quit               - Exit the program.\n"
                       "  /clear                     - Clear history and attachments for a new chat.\n"
                       "  /stats                     - Show session statistics (tokens, model, etc.).\n"
                       "  /config <save|load>        - Save or load settings to the config file.\n"
                       "  /system [prompt]           - Set/show the system prompt for the conversation.\n"
                       "  /clear_system              - Remove the system prompt.\n"
                       "  /budget [tokens]           - Set/show the max thinking budget for the model.\n"
                       "  /maxtokens [tokens]        - Set/show the max output tokens for the response.\n"
                       "  /temp [temperature]        - Set/show the temperature for the response.\n"
                       "  /topp [float]              - Set/show the topK for the response.\n"
                       "  /topk [integer]            - Set/show the topP for the response.\n"
                       "  /grounding [on|off]        - Set/show Google Search grounding.\n"
                       "  /urlcontext [on|off]       - Set/show URL context fetching.\n"
                       "  /attach <file> [prompt]    - Attach a file. Optionally add prompt on same line.\n"
                       "  /paste                     - Paste text from stdin as an attachment.\n"
                       "  /savelast <file.txt>       - Save the last model response to a text file.\n"
                       "  /save <file.json>          - (Export) Save history to a specific file path.\n"
                       "  /load <file.json>          - (Import) Load history from a specific file path.\n"
                       "  /export <file.md>          - Export the conversation to a Markdown file.\n"
                       "  /models                    - List all available models from the API.\n"
                       "\nKey Management:\n"
                       "  /keys [list]               - List the currently loaded API keys.\n"
                       "  /keys add <key>            - Add a new API key for the current session.\n"
                       "  /keys remove <index>       - Remove an API key by its index for the session.\n"
                       "  /keys check                - Validate the currently loaded API keys.\n"
                       "\nHistory Management:\n"
                       "  /history attachments list    - List all file attachments in the conversation history.\n"
                       "  /history attachments remove <id> - Remove an attachment from history (e.g., 2:1).\n"
                       "\nAttachment Management:\n"
                       "  /attachments list          - List all pending attachments for the next prompt.\n"
                       "  /attachments remove <index>- Remove a pending attachment by its index.\n"
                       "  /attachments clear         - Remove all pending attachments.\n"
                       "\nSession Management:\n"
                       "  /session new               - Start a new, unsaved session (same as /clear).\n"
                       "  /session list              - List all saved sessions.\n"
                       "  /session save <name>       - Save the current chat to a named session.\n"
                       "  /session load <name>       - Load a named session.\n"
                       "  /session delete <name>     - Delete a named session.\n");
                } else if (strcmp(command_buffer, "/keys") == 0) {
                    char sub_command[64] = {0};
                    char arg_str[256] = {0}; // Buffer for key or index
                    sscanf(arg_start, "%63s %255s", sub_command, arg_str);

                    if (strcmp(sub_command, "list") == 0 || sub_command[0] == '\0') {
                        fprintf(stderr, "--- API Keys ---\n");
                        if (state.num_api_keys == 0) {
                            fprintf(stderr, "  (No API keys are currently loaded)\n");
                        } else {
                            for (int i = 0; i < state.num_api_keys; i++) {
                                const char* key = state.api_keys[i];
                                const char* origin = state.origins[i];
                                size_t key_len = strlen(key);
                                fprintf(stderr, "  [%d] ", i);
                                if (key_len > 11) { // Mask the key for security
                                    fprintf(stderr, "%.4s...%.4s (%s)", key + 6, key + key_len - 4, origin);
                                } else {
                                    fprintf(stderr, "%.*s... (%s)", (int)key_len, key, origin);
                                }
                                if (i == state.next_key_index) {
                                    fprintf(stderr, " (next)");
                                }
                                fprintf(stderr, "\n");
                            }
                        }
                        fprintf(stderr, "----------------\n");
                    } else if (strcmp(sub_command, "add") == 0) {
                        if (arg_str[0] == '\0') {
                            fprintf(stderr, "Usage: /keys add <api_key_string>\n");
                        } else {
                            char** new_keys = realloc(state.api_keys, sizeof(char*) * (state.num_api_keys + 1));
                            if (!new_keys) {
                                fprintf(stderr, "Error: Failed to allocate memory for new key.\n");
                            } else {
                                state.api_keys = new_keys;
                                state.api_keys[state.num_api_keys] = strdup(arg_str);
                                state.num_api_keys++;
                                fprintf(stderr, "Added key (index %d). Use '/config save' to make it permanent.\n", state.num_api_keys - 1);
                            }
                        }
                    } else if (strcmp(sub_command, "remove") == 0) {
                        if (arg_str[0] == '\0') {
                            fprintf(stderr, "Usage: /keys remove <index>\n");
                        } else {
                            long index = strtol(arg_str, NULL, 10);
                            if (index < 0 || index >= state.num_api_keys) {
                                fprintf(stderr, "Error: Invalid key index.\n");
                            } else {
                                free(state.api_keys[index]);
                                if (index < state.num_api_keys - 1) {
                                    memmove(&state.api_keys[index], &state.api_keys[index + 1], sizeof(char*) * (state.num_api_keys - index - 1));
                                }
                                state.num_api_keys--;
                                // Adjust next_key_index if it's now out of bounds
                                if (state.next_key_index >= state.num_api_keys && state.num_api_keys > 0) {
                                    state.next_key_index = 0;
                                }
                                fprintf(stderr, "Removed key at index %ld. Use '/config save' to make it permanent.\n", index);
                            }
                        }
                    } else if (strcmp(sub_command, "check") == 0) {
                         fprintf(stderr, "Checking API keys...\n");
                         if (state.num_api_keys == 0) {
                            fprintf(stderr, "  (No keys to check)\n");
                         } else {
                            int original_next_index = state.next_key_index;
                            for (int i = 0; i < state.num_api_keys; i++) {
                                state.next_key_index = i; // Temporarily set the index to the key we want to test
                                int token_count = get_token_count(&state);

                                const char* key = state.api_keys[i];
                                size_t key_len = strlen(key);
                                fprintf(stderr, "  [%d] ", i);
                                if (key_len > 11) fprintf(stderr, "%.4s...%.4s", key + 6, key + key_len - 4);
                                else fprintf(stderr, "%.*s...", (int)key_len, key);

                                if (token_count >= 0) {
                                    fprintf(stderr, " : OK\n");
                                } else {
                                    fprintf(stderr, " : FAILED\n");
                                }
                            }
                            state.next_key_index = original_next_index; // Restore the original index
                         }
                    } else {
                        fprintf(stderr, "Unknown command for '/keys'. Use list, add, remove, or check.\n");
                    }
                } else if (strcmp(command_buffer, "/export") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "Usage: /export <filename.md>\n");
                    } else {
                        export_history_to_markdown(&state, arg_start);
                    }
                } else if (strcmp(command_buffer, "/clear") == 0) {
                    clear_session_state(&state);
                } else if (strcmp(command_buffer, "/session") == 0) {
                    char sub_command[64] = {0};
                    char session_name[128] = {0};
                    sscanf(arg_start, "%63s %127s", sub_command, session_name);

                    if (strcmp(sub_command, "new") == 0) {
                        clear_session_state(&state);
                    } else if (strcmp(sub_command, "list") == 0) {
                        list_sessions();
                    } else if (strcmp(sub_command, "save") == 0) {
                        if (session_name[0] == '\0') {
                            fprintf(stderr, "Usage: /session save <name>\n");
                        } else {
                            char file_path[PATH_MAX];
                            if (build_session_path(session_name, file_path, sizeof(file_path))) {
                                save_history_to_file(&state, file_path);
                                strncpy(state.current_session_name, session_name, sizeof(state.current_session_name) - 1);
                                state.current_session_name[sizeof(state.current_session_name) - 1] = '\0';
                            }
                        }
                    } else if (strcmp(sub_command, "load") == 0) {
                        if (session_name[0] == '\0') {
                            fprintf(stderr, "Usage: /session load <name>\n");
                        } else {
                            char file_path[PATH_MAX];
                            if (build_session_path(session_name, file_path, sizeof(file_path))) {
                                load_history_from_file(&state, file_path);
                                strncpy(state.current_session_name, session_name, sizeof(state.current_session_name) - 1);
                                state.current_session_name[sizeof(state.current_session_name) - 1] = '\0';
                            }
                        }
                    } else if (strcmp(sub_command, "delete") == 0) {
                        if (session_name[0] == '\0') {
                            fprintf(stderr, "Usage: /session delete <name>\n");
                        } else {
                            char file_path[PATH_MAX];
                            if (build_session_path(session_name, file_path, sizeof(file_path))) {
                                if (remove(file_path) == 0) {
                                    fprintf(stderr, "Session '%s' deleted.\n", session_name);
                                } else {
                                    perror("Error deleting session");
                                }
                            }
                        }
                    } else {
                        fprintf(stderr,"Unknown session command: '%s'. Use '/help' to see options.\n", sub_command);
                    }
                } else if (strcmp(command_buffer, "/config") == 0) {
                    char sub_command[64] = {0};
                    sscanf(arg_start, "%63s", sub_command);

                    if (strcmp(sub_command, "save") == 0) {
                        save_configuration(&state);
                    } else if (strcmp(sub_command, "load") == 0) {
                        load_configuration(&state);
                        fprintf(stderr, "Configuration reloaded from file.\n");
                    } else {
                        fprintf(stderr, "Usage: /config <save|load>\n");
                    }
                } else if (strcmp(command_buffer, "/host") == 0) {
                    if (*arg_start == '\0') {
                        if (state.host) {
                            fprintf(stderr, "Host is https://%s\n", state.host);
                        } else {
                            fprintf(stderr, "Host is empty.\n");
                        }
                    } else {
                        if (state.host != NULL) free(state.host);
                        state.host = strdup(arg_start);
                        if (!state.host) {
                            fprintf(stderr, "Error: Failed to allocate memory for host.\n");
                        } else {
                            fprintf(stderr, "Host set to: '%s'\n", state.host);
                        }
                    }
                } else if (strcmp(command_buffer, "/models") == 0) {
                    list_available_models(&state);
                } else if (strcmp(command_buffer, "/stats") == 0) {
                    fprintf(stderr,"--- Session Stats ---\n");
                    fprintf(stderr,"Model: %s\n", state.model_name);
                    fprintf(stderr,"Temperature: %.2f\n", state.temperature);
                    fprintf(stderr,"Seed: %d\n", state.seed);
                    fprintf(stderr,"System Prompt: %s\n", state.system_prompt ? state.system_prompt : "Not set");
                    fprintf(stderr,"Messages in history: %d\n", state.history.num_contents);
                    fprintf(stderr,"Pending attachments: %d\n", state.num_attached_parts);

                    if (state.history.num_contents == 0 && state.num_attached_parts == 0) {
                        fprintf(stderr,"---------------------\n");
                        continue;
                    }

                    // Temporarily add pending attachments to history for an accurate token count
                    if (state.num_attached_parts > 0) {
                        add_content_to_history(&state.history, "user", state.attached_parts, state.num_attached_parts);
                    }

                    int tokens = get_token_count(&state);

                    // Clean up the temporary history modification by removing the last entry
                    if (state.num_attached_parts > 0) {
                        free_content(&state.history.contents[state.history.num_contents - 1]);
                        state.history.num_contents--;
                    }

                    if (tokens >= 0) fprintf(stderr,"Total tokens in context (incl. pending): %d\n", tokens);
                    else fprintf(stderr,"Could not retrieve token count.\n");
                    fprintf(stderr,"---------------------\n");
                } else if (strcmp(command_buffer, "/system") == 0) {
                    if (*arg_start == '\0') {
                        if (state.system_prompt) {
                            fprintf(stderr, "System prompt is:\n%s\n", state.system_prompt);
                        } else {
                            fprintf(stderr, "System prompt is empty.\n");
                        }
                    } else {
                        if (state.system_prompt) free(state.system_prompt);
                        state.system_prompt = strdup(arg_start);
                        if (!state.system_prompt) {
                            fprintf(stderr, "Error: Failed to allocate memory for system prompt.\n");
                        } else {
                            fprintf(stderr,"System prompt set to: '%s'\n", state.system_prompt);
                        }
                    }
                } else if (strcmp(command_buffer, "/clear_system") == 0) {
                    if (state.system_prompt) {
                        free(state.system_prompt);
                        state.system_prompt = NULL;
                        fprintf(stderr,"System prompt cleared.\n");
                    } else {
                        fprintf(stderr,"No system prompt was set.\n");
                    }

                } else if (strcmp(command_buffer, "/budget") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "Thinking budget: %d tokens.\n", state.thinking_budget);
                    } else {
                        char* endptr;
                        long budget = strtol(arg_start, &endptr, 10);
                        if (endptr == arg_start || *endptr != '\0' || budget < 0) {
                            fprintf(stderr, "Error: Invalid budget value.\n");
                        } else {
                            state.thinking_budget = (int)budget;
                            if (state.thinking_budget<1) {
                                state.thinking_budget=-1;
                                fprintf(stderr, "Thinking budget set to automatic.\n");
                            } else {
                              fprintf(stderr, "Thinking budget set to %d tokens.\n", state.thinking_budget);
                            }
                        }
                    }
                } else if (strcmp(command_buffer, "/maxtokens") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "Max output tokens: %d tokens.\n", state.max_output_tokens);
                    } else {
                        char* endptr;
                        long tokens = strtol(arg_start, &endptr, 10);
                        if (endptr == arg_start || *endptr != '\0' || tokens <= 0) {
                            fprintf(stderr, "Error: Invalid max tokens value.\n");
                        } else {
                            state.max_output_tokens = (int)tokens;
                            fprintf(stderr, "Max output tokens set to %d.\n", state.max_output_tokens);
                        }
                    }
                } else if (strcmp(command_buffer, "/topk") == 0) {
                    if (*arg_start == '\0') {
                        if (state.topK > 0) {
                            fprintf(stderr, "topK is set to: %d\n", state.topK);
                        } else {
                            fprintf(stderr, "topK is not set.\n");
                        }
                    } else {
                        char* endptr;
                        long val = strtol(arg_start, &endptr, 10);
                        if (endptr == arg_start || *endptr != '\0' || val <= 0) {
                            fprintf(stderr, "Error: Invalid topK value. Must be a positive integer.\n");
                        } else {
                            state.topK = (int)val;
                            fprintf(stderr, "topK set to %d.\n", state.topK);
                        }
                    }
                } else if (strcmp(command_buffer, "/topp") == 0) {
                    if (*arg_start == '\0') {
                         if (state.topP > 0) {
                            fprintf(stderr, "topP is set to: %.2f\n", state.topP);
                        } else {
                            fprintf(stderr, "topP is not set.\n");
                        }
                    } else {
                        char* endptr;
                        float val = strtof(arg_start, &endptr);
                        if (endptr == arg_start || *endptr != '\0' || val <= 0.0f || val > 1.0f) {
                            fprintf(stderr, "Error: Invalid topP value. Must be between 0.0 and 1.0.\n");
                        } else {
                            state.topP = val;
                            fprintf(stderr, "topP set to %.2f.\n", state.topP);
                        }
                    }
                } else if (strcmp(command_buffer, "/temp") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "Temperature: %.2f.\n", state.temperature);
                    } else {
                        char* endptr;
                        float temp = strtof(arg_start, &endptr);
                        if (endptr == arg_start || *endptr != '\0' || temp <= 0) {
                            fprintf(stderr, "Error: Invalid temperature value.\n");
                        } else {
                            state.temperature = temp;
                            fprintf(stderr, "Temperature set to %.2f.\n", state.temperature);
                        }
                    }
                } else if (strcmp(command_buffer, "/grounding") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "Google grounding is %s.\n", state.google_grounding ? "ON" : "OFF");
                    } else if (STRCASECMP(arg_start, "on") == 0) {
                        state.google_grounding = true;
                        fprintf(stderr, "Google grounding turned ON.\n");
                    } else if (STRCASECMP(arg_start, "off") == 0) {
                        state.google_grounding = false;
                        fprintf(stderr, "Google grounding turned OFF.\n");
                    } else {
                        fprintf(stderr, "Usage: /grounding [on|off]\n");
                    }
                } else if (strcmp(command_buffer, "/urlcontext") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr, "URL context is %s.\n", state.url_context ? "ON" : "OFF");
                    } else if (STRCASECMP(arg_start, "on") == 0) {
                        state.url_context = true;
                        fprintf(stderr, "URL context turned ON.\n");
                    } else if (STRCASECMP(arg_start, "off") == 0) {
                        state.url_context = false;
                        fprintf(stderr, "URL context turned OFF.\n");
                    } else {
                        fprintf(stderr, "Usage: /urlcontext [on|off]\n");
                    }
                } else if (strcmp(command_buffer, "/save") == 0) {
                    if (!is_path_safe(arg_start)) {
                        fprintf(stderr, "Error: Unsafe or absolute file path specified: %s\n", arg_start);
                    } else {
                        save_history_to_file(&state, arg_start);
                    }
                } else if (strcmp(command_buffer, "/load") == 0) {
                    if (!is_path_safe(arg_start)) {
                        fprintf(stderr, "Error: Unsafe or absolute file path specified: %s\n", arg_start);
                    } else {
                        load_history_from_file(&state, arg_start);
                    }
                } else if (strcmp(command_buffer, "/savelast") == 0) {
                    if (state.last_model_response) {
                        if (!is_path_safe(arg_start)) {
                            fprintf(stderr, "Error: Unsafe file path for saving last response.\n");
                        } else {
                            FILE *f = fopen(arg_start, "w");
                            if (f) {
                                fputs(state.last_model_response, f);
                                fclose(f);
                                fprintf(stderr,"Last response saved to %s\n", arg_start);
                            } else {
                                perror("Failed to save last response");
                            }
                        }
                    } else {
                        fprintf(stderr,"No last response to save.\n");
                    }
                } else if (strcmp(command_buffer, "/attach") == 0) {
                    if (*arg_start == '\0') {
                        fprintf(stderr,"Usage: /attach <filename>\n");
                    } else {
                        handle_attachment_from_stream(NULL, arg_start, get_mime_type(arg_start), &state);
                    }
                } else if (strcmp(command_buffer, "/attachments") == 0) {
                    char sub_command[64] = {0};
                    char arg_str[64] = {0};
                    sscanf(arg_start, "%63s %63s", sub_command, arg_str);

                    if (strcmp(sub_command, "list") == 0 || sub_command[0] == '\0') {
                        if (state.num_attached_parts == 0) {
                            fprintf(stderr,"No pending attachments.\n");
                        } else {
                            fprintf(stderr,"Pending Attachments:\n");
                            for (int i = 0; i < state.num_attached_parts; i++) {
                                fprintf(stderr,"  [%d] %s (MIME: %s)\n", i, state.attached_parts[i].filename, state.attached_parts[i].mime_type);
                            }
                        }
                    } else if (strcmp(sub_command, "clear") == 0) {
                        free_pending_attachments(&state);
                        fprintf(stderr,"All pending attachments cleared.\n");
                    } else if (strcmp(sub_command, "remove") == 0) {
                        if (arg_str[0] == '\0') {
                            fprintf(stderr,"Usage: /attachments remove <index>\n");
                        } else {
                            char* endptr;
                            long index_to_remove = strtol(arg_str, &endptr, 10);
                            if (endptr == arg_str || *endptr != '\0' || index_to_remove < 0 || index_to_remove >= state.num_attached_parts) {
                                fprintf(stderr,"Error: Invalid attachment index.\n");
                            } else {
                                fprintf(stderr,"Removing attachment: %s\n", state.attached_parts[index_to_remove].filename);
                                free(state.attached_parts[index_to_remove].filename);
                                free(state.attached_parts[index_to_remove].mime_type);
                                free(state.attached_parts[index_to_remove].base64_data);

                                if (index_to_remove < state.num_attached_parts - 1) {
                                    memmove(&state.attached_parts[index_to_remove],
                                            &state.attached_parts[index_to_remove + 1],
                                            (state.num_attached_parts - index_to_remove - 1) * sizeof(Part));
                                }
                                state.num_attached_parts--;
                            }
                        }
                    } else {
                        fprintf(stderr,"Unknown attachments command: '%s'. Use list, remove, or clear.\n", sub_command);
                    }

                } else if (strcmp(command_buffer, "/history") == 0) {
                    char sub_command[64] = {0};
                    sscanf(arg_start, "%63s", sub_command);

                    if (strcmp(sub_command, "attachments") == 0) {
                        char action[64] = {0};
                        char id_str[64] = {0};
                        char* attachments_arg_start = arg_start + strlen(sub_command);
                        while(isspace((unsigned char)*attachments_arg_start)) attachments_arg_start++;
                        sscanf(attachments_arg_start, "%63s %63s", action, id_str);

                        if (strcmp(action, "list") == 0 || action[0] == '\0') {
                            fprintf(stderr,"--- Attachments in History ---\n");
                            bool found = false;
                            for (int i = 0; i < state.history.num_contents; i++) {
                                Content* content = &state.history.contents[i];
                                for (int j = 0; j < content->num_parts; j++) {
                                    Part* part = &content->parts[j];
                                    if (part->type == PART_TYPE_FILE) {
                                        if (!found) {
                                            fprintf(stderr,"  ID      | Role  | Filename / Description\n");
                                            fprintf(stderr,"----------|-------|----------------------------------------\n");
                                            found = true;
                                        }
                                        fprintf(stderr,"  [%-2d:%-2d] | %-5s | %s (MIME: %s)\n", i, j, content->role, part->filename ? part->filename : "Pasted/Loaded Data", part->mime_type);
                                    }
                                }
                            }
                            if (!found) {
                                fprintf(stderr,"  (No file attachments found in history)\n");
                            }
                            fprintf(stderr,"------------------------------\n");
                        } else if (strcmp(action, "remove") == 0) {
                            if (id_str[0] == '\0') {
                                fprintf(stderr,"Usage: /history attachments remove <msg_idx:part_idx>\n");
                            } else {
                                int msg_idx = -1, part_idx = -1;
                                if (sscanf(id_str, "%d:%d", &msg_idx, &part_idx) != 2) {
                                    fprintf(stderr,"Error: Invalid ID format. Use <msg_idx:part_idx>.\n");
                                } else if (msg_idx < 0 || msg_idx >= state.history.num_contents) {
                                    fprintf(stderr,"Error: Invalid message index %d.\n", msg_idx);
                                } else if (part_idx < 0 || part_idx >= state.history.contents[msg_idx].num_parts) {
                                    fprintf(stderr,"Error: Invalid part index %d for message %d.\n", part_idx, msg_idx);
                                } else {
                                    Content* content = &state.history.contents[msg_idx];
                                    Part* part_to_remove = &content->parts[part_idx];
                                    if (part_to_remove->type != PART_TYPE_FILE) {
                                        fprintf(stderr,"Error: Part [%d:%d] is not a file attachment.\n", msg_idx, part_idx);
                                    } else {
                                        fprintf(stderr,"Removing attachment [%d:%d]: %s\n", msg_idx, part_idx, part_to_remove->filename ? part_to_remove->filename : "Pasted Data");

                                        if (part_to_remove->filename) free(part_to_remove->filename);
                                        if (part_to_remove->mime_type) free(part_to_remove->mime_type);
                                        if (part_to_remove->base64_data) free(part_to_remove->base64_data);
                                        if (part_to_remove->text) free(part_to_remove->text);

                                        if (part_idx < content->num_parts - 1) {
                                            memmove(&content->parts[part_idx], &content->parts[part_idx + 1], (content->num_parts - part_idx - 1) * sizeof(Part));
                                        }
                                        content->num_parts--;
                                    }
                                }
                            }
                        } else {
                            fprintf(stderr,"Unknown command for '/history attachments'. Use 'list' or 'remove'.\n");
                        }
                    } else {
                        fprintf(stderr,"Unknown command for '/history'. Try '/history attachments'.\n");
                    }
                } else if (strcmp(command_buffer, "/paste") == 0) {
                    #ifdef _WIN32
                        fprintf(stderr, "Pasting content. Press Ctrl+Z then Enter when done.\n");
                    #else
                        fprintf(stderr, "Pasting content. Press Ctrl+D when done.\n");
                    #endif
                    handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
                } else {
                    fprintf(stderr,"Unknown command: %s. Type /help for a list of commands.\n", command_buffer);
                }
            } else {
                is_command = false;
            }

            // If the input was a command, we are done with this iteration.
            if (is_command) {
                free(line);
                continue;
            }

            // The input is a prompt. Process it based on whether we are in free mode or not.
            if (state.free_mode) {
                // Logic for handling prompts in free mode.
                size_t current_turn_len = 0;
                for (int i = 0; i < state.num_attached_parts; i++) {
                    if (state.attached_parts[i].text) {
                        current_turn_len += strlen(state.attached_parts[i].text);
                    }
                }
                current_turn_len += strlen(p) + 1;

                if (current_turn_len <= 1 && state.num_attached_parts == 0) {
                    free(line);
                    continue;
                }

                size_t history_len = 0;
                for (int i = 0; i < state.history.num_contents; i++) {
                    if (state.history.contents[i].num_parts > 0 && state.history.contents[i].parts[0].text) {
                        history_len += strlen(state.history.contents[i].parts[0].text);
                    }
                }

                if (history_len + current_turn_len > MAX_FREE_MODE_CONTEXT_SIZE) {
                    fprintf(stderr, "\nError: Context is too large for free mode (approx. %zu KB). Please use '/clear' or restart the session.\n", (history_len + current_turn_len) / 1024);
                    free_pending_attachments(&state);
                    free(line);
                    continue;
                }

                char* current_turn_prompt = malloc(current_turn_len);
                if (!current_turn_prompt) {
                    fprintf(stderr, "Error: Failed to allocate memory for prompt.\n");
                    free_pending_attachments(&state);
                    free(line);
                    continue;
                }
                current_turn_prompt[0] = '\0';
                for (int i = 0; i < state.num_attached_parts; i++) {
                    if (state.attached_parts[i].text) strcat(current_turn_prompt, state.attached_parts[i].text);
                }
                free_pending_attachments(&state);
                strcat(current_turn_prompt, p);

                printf("\n");
                if(state.last_free_response_part) {
                    free(state.last_free_response_part);
                    state.last_free_response_part = NULL;
                }
                bool success = send_free_api_request(&state, current_turn_prompt);
                printf("\n");
                if (success) {
                    Part user_part = { .type = PART_TYPE_TEXT, .text = current_turn_prompt };
                    add_content_to_history(&state.history, "user", &user_part, 1);
                    if (state.last_free_response_part) {
                         Part model_part = { .type = PART_TYPE_TEXT, .text = state.last_free_response_part };
                         add_content_to_history(&state.history, "model", &model_part, 1);
                    }
                }
                free(current_turn_prompt);

            } else { // Logic for handling prompts with the official API.
                int total_parts = state.num_attached_parts + (strlen(p) > 0 ? 1 : 0);
                if (total_parts == 0) { free(line); continue; }

                Part* current_turn_parts = malloc(sizeof(Part) * total_parts);
                if (!current_turn_parts) {
                    fprintf(stderr, "Error: Failed to allocate memory for current turn parts.\n");
                    free(line);
                    continue;
                }

                int current_part_index = 0;
                for(int i=0; i < state.num_attached_parts; i++) {
                    current_turn_parts[current_part_index++] = state.attached_parts[i];
                }

                if (strlen(p) > 0) {
                    current_turn_parts[current_part_index] = (Part){ .type = PART_TYPE_TEXT, .text = p };
                }

                add_content_to_history(&state.history, "user", current_turn_parts, total_parts);

                free_pending_attachments(&state);
                free(current_turn_parts);

                printf("\n");

                char* model_response_text = NULL;
                if (send_api_request(&state, &model_response_text)) {
                    printf("\n");
                    if (state.last_model_response) free(state.last_model_response);
                    state.last_model_response = model_response_text;

                    Part model_part = { .type = PART_TYPE_TEXT, .text = strdup(state.last_model_response) };
                    add_content_to_history(&state.history, "model", &model_part, 1);
                    free(model_part.text);
                } else {
                    if (state.history.num_contents > 0) {
                        state.history.num_contents--;
                        free_content(&state.history.contents[state.history.num_contents]);
                    }
                }
            }

            free(line);
        }
    }

    if (state.save_session_path) {
        if (!is_path_safe(state.save_session_path)) {
            fprintf(stderr, "Error: Unsafe file path specified for saving session: %s\n", state.save_session_path);
        } else {
            save_history_to_file(&state, state.save_session_path);
        }
    }
    
    // --- 8. Cleanup ---
    // Free all dynamically allocated memory before exiting.
    if (state.api_keys) {
        for (int i = 0; i < state.num_api_keys; i++) {
            free(state.api_keys[i]);
        }
        free(state.api_keys);
    }
    if (state.origins) {
        for (int i = 0; i < state.num_api_keys; i++) {
            free(state.origins[i]);
        }
        free(state.origins);
    }

    if(state.host) free(state.host);
    if(state.last_model_response) free(state.last_model_response);
    if(state.last_free_response_part) free(state.last_free_response_part);
    if(state.system_prompt) free(state.system_prompt);
    if(state.final_code) free(state.final_code);
    free_history(&state.history);
    free_pending_attachments(&state);

    if (interactive) fprintf(stderr,"\nExiting session.\n");
}

// --- Helper and Utility Functions ---

/**
 * @brief Heuristically determines if a buffer contains text data.
 * @details It checks a sample of the data for a high percentage of printable
 *          ASCII characters and the absence of null bytes, which are common
 *          indicators of a text file.
 * @param data A pointer to the raw file data.
 * @param size The size of the data buffer.
 * @return True if the data is likely text, false otherwise.
 */
bool is_data_text(const unsigned char* data, size_t size) {
    if (data == NULL || size == 0) return false;

    size_t sample_size = (size > 1024) ? 1024 : size;
    int printable_count = 0;

    for (size_t i = 0; i < sample_size; i++) {
        if (data[i] == 0) return false; // Null bytes strongly indicate binary
        if (isprint(data[i]) || data[i] == '\n' || data[i] == '\r' || data[i] == '\t') {
            printable_count++;
        }
    }

    // Heuristic: If over 85% of the characters are printable, it's probably text.
    return ((double)printable_count / sample_size) > 0.85;
}

/**
 * @brief Determines the image MIME type by inspecting the file's magic bytes.
 * @param data A pointer to the raw file data.
 * @param size The size of the data buffer.
 * @return A constant string literal for the MIME type (e.g., "image/jpeg") if
 *         a known image signature is found, otherwise NULL.
 */
const char* get_image_mime_type_from_data(const unsigned char* data, size_t size) {
    if (data == NULL) return NULL;

    // Check for JPEG (JPG) - FF D8 FF
    if (size >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        return "image/jpeg";
    }
    // Check for PNG - 89 50 4E 47 0D 0A 1A 0A
    if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
        return "image/png";
    }
    // Check for GIF - "GIF87a" or "GIF89a"
    if (size >= 6 && memcmp(data, "GIF8", 4) == 0 && (data[4] == '7' || data[4] == '9') && data[5] == 'a') {
        return "image/gif";
    }
    // Check for WebP - "RIFF" at start, "WEBP" at offset 8
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WEBP", 4) == 0) {
        return "image/webp";
    }

    return NULL; // Not a recognized image format
}

/**
 * @brief Determines the MIME type of a file by inspecting its content and extension.
 * @details This function first attempts to open the file and read its header to
 *          check for known "magic bytes" (for images) or to heuristically
 *          determine if it's a text file. If the file cannot be opened or if the
 *          content is inconclusive, it falls back to checking the file extension
 *          using efficient array lookups.
 * @param filename The name of the file.
 * @return A constant string literal representing the guessed MIME type.
 */
const char* get_mime_type(const char* filename) {
    // --- Content-Aware Analysis ---
    FILE *file = fopen(filename, "rb");
    if (file) {
        unsigned char buffer[1024];
        size_t bytes_read = fread(buffer, 1, sizeof(buffer), file);
        fclose(file);

        if (bytes_read > 0) {
            // 1. Check for image formats by magic bytes first.
            const char* image_mime = get_image_mime_type_from_data(buffer, bytes_read);
            if (image_mime != NULL) {
                return image_mime;
            }
            // 2. Heuristically check if the file is plain text.
            if (is_data_text(buffer, bytes_read)) {
                return "text/plain";
            }
        }
    }

    // --- Fallback to Extension-Based Analysis ---
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) {
        return "text/plain"; // Default if no extension.
    }

    // Array for extensions that should be treated as plain text.
    static const char* text_extensions[] = {
        ".txt", ".c", ".h", ".cpp", ".hpp", ".py", ".ts", ".java",
        ".cs", ".go", ".php", ".rs", ".md", ".yml", ".yaml", ".sh",
        ".bat", ".pl", ".rb", ".jsx", ".tsx", ".vue", ".svelte",
        ".dart", ".kt", ".swift", ".r", ".sql", ".toml", ".ini", ".cfg"
    };

    // More specific text-based MIME types.
    if (STRCASECMP(dot, ".html") == 0) return "text/html";
    if (STRCASECMP(dot, ".css") == 0) return "text/css";
    if (STRCASECMP(dot, ".js") == 0) return "text/javascript";
    if (STRCASECMP(dot, ".xml") == 0) return "application/xml";
    if (STRCASECMP(dot, ".json") == 0) return "application/json";

    // Loop through the generic text extensions array.
    for (size_t i = 0; i < sizeof(text_extensions) / sizeof(text_extensions[0]); i++) {
        if (STRCASECMP(dot, text_extensions[i]) == 0) {
            return "text/plain";
        }
    }

    // Fallback for non-text types by extension.
    if (STRCASECMP(dot, ".pdf") == 0) return "application/pdf";
    if (STRCASECMP(dot, ".jpg") == 0 || STRCASECMP(dot, ".jpeg") == 0) return "image/jpeg";
    if (STRCASECMP(dot, ".png") == 0) return "image/png";
    if (STRCASECMP(dot, ".gif") == 0) return "image/gif";
    if (STRCASECMP(dot, ".webp") == 0) return "image/webp";

    // Default for any unrecognized extension.
    return "application/octet-stream"; // or text/plain ?
}

// Helper function to remove a substring from a string.
// Returns a new dynamically allocated string.
static char* str_replace(const char* orig, const char* rep, const char* with) {
    char *result; // the return string
    char *ins;    // the next insert point
    char *tmp;    // varies
    int len_rep;  // length of rep (the string to remove)
    int len_with; // length of with (the string to replace with)
    int len_front; // distance from beginning to rep
    int count;    // number of replacements

    // Sanity checks and initialization
    if (!orig || !rep)
        return NULL;
    len_rep = strlen(rep);
    if (len_rep == 0)
        return NULL; // Empty rep causes infinite loop
    if (!with)
        with = "";
    len_with = strlen(with);

    // Count the number of replacements needed
    ins = (char*) orig;
    for (count = 0; (tmp = strstr(ins, rep)); ++count) {
        ins = tmp + len_rep;
    }

    tmp = result = malloc(strlen(orig) + (len_with - len_rep) * count + 1);

    if (!result)
        return NULL;

    // First time through the loop, all the variable are set correctly
    // from here on,
    //    tmp points to the end of the result string
    //    ins points to the next occurrence of rep in orig
    //    orig points to the remainder of orig after "end of rep"
    while (count--) {
        ins = strstr(orig, rep);
        len_front = ins - orig;
        tmp = strncpy(tmp, orig, len_front) + len_front;
        tmp = strcpy(tmp, with) + len_with;
        orig += len_front + len_rep; // move to next "end of rep"
    }
    strcpy(tmp, orig);
    return result;
}

/**
 * @brief Parses a line from the unofficial "free" API's streaming response.
 * @details The free API returns data in a complex format: a JSON array containing
 *          a string, which itself is another JSON array. This function navigates
 *          this nested structure to extract the progressive response text. It then
 *          calculates the difference between the new text and the previous fragment
 *          to print only the new characters, creating a smooth streaming effect.
 * @param line The null-terminated string containing the data line to process.
 * @param state A pointer to the AppState, used to access and update the
 *              `last_free_response_part` buffer which tracks the streaming state.
 */

static void process_free_line(char* line, AppState* state) {

    char* processed_line = str_replace(line, "\\\\nhttp://googleusercontent.com/immersive_entry_chip/0\\\\n", "");

    cJSON* root = cJSON_Parse(processed_line);

    if (!root) {
        fprintf(stderr,"\n\n***ERROR IN RECEIVED DATA***\n\n");
        return;
    }

    cJSON* wrb_fr_array = cJSON_GetArrayItem(root, 0);

    if (!cJSON_IsArray(wrb_fr_array)) {
        cJSON_Delete(root);
        return;
    }

    // The actual payload is a stringified JSON blob at the third position.
    cJSON* stringified_json = cJSON_GetArrayItem(wrb_fr_array, 2);
    if (!stringified_json) {
        // This chunk signals the end of a code block. Print and free the stored code.
        if (state->final_code) {
            printf("\n\n%s\n", state->final_code);
            free(state->final_code);
            state->final_code = NULL;
        }
        cJSON_Delete(root);
        return;
    }

    // Parse the stringified inner JSON.
    cJSON* inner_root = cJSON_Parse(stringified_json->valuestring);
    if (!inner_root) {
        cJSON_Delete(root);
        return;
    }

    if (state->loc_tile > 0) {
        cJSON* item5 = cJSON_GetArrayItem(inner_root, 5);
        if (item5) {
            // Check for bit 0. Use 'else if' to check for bit 1 only if bit 0 is not set.
            if ((state->loc_tile & 1)) {
                cJSON* item5_0 = cJSON_GetArrayItem(item5, 0);
                if (item5_0 && cJSON_IsString(item5_0)) {
                    printf("%s\n", item5_0->valuestring);
                    state->loc_tile &= ~1; // ✅ Correctly clear bit 0
                    state->loc_tile |= 4;
                }
            } else if ((state->loc_tile & 2)) {
                cJSON* item5_4 = cJSON_GetArrayItem(item5, 4);
                if (item5_4 && cJSON_IsString(item5_4)) {
                    printf("https:%s\n", item5_4->valuestring);
                    state->loc_tile &= ~2; // ✅ Correctly clear bit 1
                    state->loc_tile |= 4;
                }
            }
            state->loc_gathered = true;

        }
            // ✨ Cleanup and exit once, regardless of which branch was taken.
            cJSON_Delete(root);
            return;

    }

    // Navigate through the deeply nested structure to find the response text.
    // The path is typically inner_root -> [4] -> [0] -> [1] -> [0].
    cJSON* item4 = cJSON_GetArrayItem(inner_root, 4);
    if (item4) {
        cJSON* item4_0 = cJSON_GetArrayItem(item4, 0);
        if (item4_0) {
            cJSON* item4_0_1 = cJSON_GetArrayItem(item4_0, 1);
            if (item4_0_1) {
                cJSON* text_item = cJSON_GetArrayItem(item4_0_1, 0);
                if (cJSON_IsString(text_item)) {
                    const char* current_text = text_item->valuestring;
                    size_t last_len = state->last_free_response_part ? strlen(state->last_free_response_part) : 0;
                    size_t current_len = strlen(current_text);

                    // If the new text is an extension of the old one, print the difference.
                    if (current_len > last_len && strncmp(current_text, state->last_free_response_part ? state->last_free_response_part : "", last_len) == 0) {
                        const char* diff = current_text + last_len;
                        printf("%s", diff);
                        fflush(stdout);
                    }
                    // Handle cases where the stream resets or provides a shorter, corrected version.
                    else if (last_len > 0 && current_len < last_len) {
                        // Use carriage return to overwrite the previous line with the new, shorter text.
                        printf("\r%*s\r%s", (int)last_len, "", current_text);
                        fflush(stdout);
                    }

                    // Update the buffer with the latest full response text.
                    free(state->last_free_response_part);
                    state->last_free_response_part = strdup(current_text);
                }
            }

            cJSON* item30 = cJSON_GetArrayItem(item4_0, 30);
            if (item30) {
                cJSON* item30_0 = cJSON_GetArrayItem(item30, 0);
                if (item30_0) {
                    cJSON* code_item = cJSON_GetArrayItem(item30_0, 4);
                    if (cJSON_IsString(code_item)) {
                        const char* current_code = code_item->valuestring;
                        // Free any code from a previous chunk in this same stream
                        if (state->final_code) {
                            free(state->final_code);
                        }
                        // Store the new code block in our safe state variable
                        state->final_code = strdup(current_code);
                    }
                }
            }
            
        }
    }

    // Clean up all parsed JSON objects.
    cJSON_Delete(inner_root);
    cJSON_Delete(root);
}

/**
 * @brief A libcurl write callback for the unofficial "free" API stream.
 * @details This function is called by libcurl when new data arrives from the
 *          free API. It handles the specific quirks of this stream, such as
 *          stripping the initial ")]}'" prefix. It buffers the incoming data,
 *          identifies complete lines (separated by newlines), and passes them
 *          to `process_free_line` for parsing and printing.
 * @param contents A pointer to the received data chunk.
 * @param size The size of each data member (always 1 for text streams).
 * @param nmemb The number of members received.
 * @param userp A user-defined pointer, which points to a FreeCallbackData
 *              struct containing both the memory buffer and the application state.
 * @return The total number of bytes (realsize) successfully handled. Returning a
 *         different value will signal an error to libcurl.
 */
static size_t write_free_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    FreeCallbackData* data = (FreeCallbackData*)userp;
    MemoryStruct* mem = data->mem;

    char* current_contents = (char*)contents;
    
    
    //fprintf(stderr,"\nReceived:\n%s\n",current_contents);

    // The very first chunk of data from this API stream starts with ")]}'".
    // We must skip this prefix to get to the actual data.
    if (mem->size == 0 && realsize > 4 && strncmp(current_contents, ")]}'", 4) == 0) {
        current_contents += 4;
        realsize -= 4;
    }

    // Append the new, potentially stripped, data to the buffer.
    char* ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Error: realloc failed in stream callback.\n");
        return 0; // Signal error to libcurl.
    }
    mem->buffer = ptr;
    memcpy(mem->buffer + mem->size, current_contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = '\0';

    // Process the buffer line by line.
    char* line_start = mem->buffer;
    char* line_end;
    while ((line_end = strchr(line_start, '\n')) != NULL) {
        *line_end = '\0';
        // The actual content lines start with a '[', so we process only those.
        if (*line_start == '[') {
            process_free_line(line_start, data->state);
        }
        line_start = line_end + 1;
    }

    // If there's a partial line left at the end of the buffer, move it
    // to the beginning so it can be completed by the next data chunk.
    if (line_start < mem->buffer + mem->size) {
        size_t remaining_len = mem->size - (line_start - mem->buffer);
        memmove(mem->buffer, line_start, remaining_len);
        mem->size = remaining_len;
        mem->buffer[mem->size] = '\0';
    } else {
        // If everything was processed, clear the buffer.
        mem->size = 0;
        if(mem->buffer) mem->buffer[0] = '\0';
    }

    return size * nmemb;
}

/**
 * @brief Gets the system language and normalizes it to the "ll-CC" format.
 * @details This function provides a convenient interface to get the system locale.
 *          It determines the locale, normalizes it, and stores it in a static
 *          internal buffer.
 *
 * @warning This function is NOT thread-safe due to its use of a static buffer.
 *          The returned pointer should not be freed, and its contents may be
 *          overwritten by subsequent calls to this function.
 *
 * @return A const pointer to a static buffer containing the language string
 *         (e.g., "en-US"). Returns a safe default ("en-US") on failure.
 */
const char* get_system_language(void) {
    // Define a static buffer that persists between function calls.
    // Its value will be returned but will be overwritten on the next call.
    static char language_buffer[16]; // Sufficient for "ll-CC" and some slack.
    bool success = false;

#ifdef _WIN32
    // --- Windows Implementation ---
    // Use the Win32 API to get language and country codes separately.
    LCID lcid = GetThreadLocale();
    char lang[9] = {0};
    char country[9] = {0};

    // Get the ISO 639 language name (e.g., "en") and ISO 3166 country name (e.g., "US").
    if (GetLocaleInfoA(lcid, LOCALE_SISO639LANGNAME, lang, sizeof(lang)) > 0 &&
        GetLocaleInfoA(lcid, LOCALE_SISO3166CTRYNAME, country, sizeof(country)) > 0)
    {
        // Safely combine them into the final "ll-CC" format.
        snprintf(language_buffer, sizeof(language_buffer), "%s-%s", lang, country);
        success = true;
    }

#else
    // --- POSIX Implementation ---
    // Use setlocale to query the locale from environment variables (e.g., LANG).
    // An empty string "" for the second argument tells it to use the environment.
    const char* locale = setlocale(LC_ALL, "");

    // Check if a valid, non-default locale was found. "C" and "C.*" are default
    // locales indicating that no specific user locale is configured.
    if (locale != NULL && strcmp(locale, "C") != 0 && strncmp(locale, "C.", 2) != 0) {
        // Copy the locale string into our buffer for modification.
        strncpy(language_buffer, locale, sizeof(language_buffer) - 1);
        language_buffer[sizeof(language_buffer) - 1] = '\0'; // Ensure null-termination.

        // Truncate the string at the encoding part (e.g., ".UTF-8") or
        // modifier part (e.g., "@euro"), which we don't need.
        char* ptr = strpbrk(language_buffer, ".@");
        if (ptr != NULL) {
            *ptr = '\0';
        }

        // Normalize the separator from the common POSIX underscore to a hyphen.
        ptr = strchr(language_buffer, '_');
        if (ptr != NULL) {
            *ptr = '-';
        }
        success = true;
    }
#endif

    // --- Fallback on Failure ---
    // If neither of the platform-specific methods succeeded, set a default value.
    if (!success) {
        // Use strncpy for safety, although "en-US" fits easily.
        strncpy(language_buffer, "en-US", sizeof(language_buffer) - 1);
        language_buffer[sizeof(language_buffer) - 1] = '\0'; // Ensure null-termination.
    }

    return language_buffer;
}

/**
 * @brief Constructs the JSON request payload for the unofficial "free" API.
 * @details This function programmatically builds the required complex and brittle
 *          JSON object using the cJSON library. This avoids unreadable string
 *          templates and provides a clear, maintainable, and self-documenting
 *          way to create the payload. It selects between two structures based on
 *          whether the model is "pro" or "flash".
 * @param state A pointer to the AppState, used for conversation history.
 * @param current_prompt The user's latest prompt for this turn.
 * @param is_pro_model A boolean flag to select the correct structure.
 * @return A dynamically allocated, null-terminated string containing the final
 *         JSON payload. The caller is responsible for freeing this memory.
 */
char* build_free_request_payload(AppState* state, const char* current_prompt, bool is_pro_model) {
    // --- 1. Build the full conversation transcript string ---
    size_t transcript_len = 0;
    for (int i = 0; i < state->history.num_contents; i++) {
        Content* c = &state->history.contents[i];
        if (c->num_parts > 0 && c->parts[0].text) {
            transcript_len += strlen(c->role) + strlen(c->parts[0].text) + 5;
        }
    }
    transcript_len += strlen("User: ") + strlen(current_prompt) + 1;

    char* full_transcript = malloc(transcript_len);
    if (!full_transcript) return NULL;
    full_transcript[0] = '\0';

    for (int i = 0; i < state->history.num_contents; i++) {
        Content* c = &state->history.contents[i];
        if (c->num_parts > 0 && c->parts[0].text) {
            char role_cap[32];
            strncpy(role_cap, c->role, sizeof(role_cap) - 1);
            role_cap[0] = toupper((unsigned char)role_cap[0]);
            sprintf(full_transcript + strlen(full_transcript), "%s: %s\n\n", role_cap, c->parts[0].text);
        }
    }
    sprintf(full_transcript + strlen(full_transcript), "User: %s", current_prompt);

    // --- 2. Programmatically build the inner JSON array ---
    cJSON* inner_array = cJSON_CreateArray();

    // Element [0]: The main prompt part
    cJSON* prompt_part = cJSON_CreateArray();
    cJSON_AddItemToArray(prompt_part, cJSON_CreateString(full_transcript));
    cJSON_AddItemToArray(prompt_part, cJSON_CreateNumber(0));
    for(int i=0; i<5; ++i) cJSON_AddItemToArray(prompt_part, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, prompt_part);
    free(full_transcript); // Transcript is now copied into cJSON object.

    const char* system_lang = get_system_language();
    
    cJSON_AddItemToArray(inner_array, cJSON_CreateStringArray((const char*[]){system_lang},1));

    // Element [2]: Placeholder array
    cJSON* element2 = cJSON_CreateArray();
    for(int i=0; i<3; ++i) cJSON_AddItemToArray(element2, cJSON_CreateString(""));
    for(int i=0; i<6; ++i) cJSON_AddItemToArray(element2, cJSON_CreateNull());
    cJSON_AddItemToArray(element2, cJSON_CreateString(""));
    cJSON_AddItemToArray(inner_array, element2);

    // --- Add all remaining placeholder elements ---
    cJSON_AddItemToArray(inner_array, cJSON_CreateString(""));
    cJSON_AddItemToArray(inner_array, cJSON_CreateString(""));
    cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateIntArray((const int[]){is_pro_model ? 1 : 0}, 1)); // Key difference
    cJSON_AddItemToArray(inner_array, cJSON_CreateNumber(1));
    for(int i=0; i<2; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateNumber(1));
    cJSON_AddItemToArray(inner_array, cJSON_CreateNumber(0+1));
    for(int i=0; i<5; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());

    cJSON* element17 = cJSON_CreateArray();
    cJSON_AddItemToArray(element17, cJSON_CreateIntArray((const int[]){0}, 1));
    cJSON_AddItemToArray(inner_array, element17);

    cJSON_AddItemToArray(inner_array, cJSON_CreateNumber(0+1));
    for(int i=0; i<8; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateNumber(1));
    for(int i=0; i<2; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateIntArray((const int[]){4}, 1));

    for(int i=0; i<10; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateIntArray((const int[]){is_pro_model ? 1 : 2}, 1)); // Key difference
    for(int i=0; i<61; ++i) cJSON_AddItemToArray(inner_array, cJSON_CreateNull());
    cJSON_AddItemToArray(inner_array, cJSON_CreateArray());

    // --- 3. Stringify the inner array and wrap it in the final outer array ---
    char* inner_json_str = cJSON_PrintUnformatted(inner_array);
    cJSON_Delete(inner_array);
    if (!inner_json_str) return NULL;

    cJSON* outer_array = cJSON_CreateArray();
    cJSON_AddItemToArray(outer_array, cJSON_CreateNull());
    cJSON_AddItemToArray(outer_array, cJSON_CreateString(inner_json_str));
    free(inner_json_str);

    char* final_json_str = cJSON_PrintUnformatted(outer_array);
    cJSON_Delete(outer_array);

    return final_json_str;
}

/**
 * @brief Sends a request to the unofficial, key-free Gemini API with retry logic.
 * @details This function orchestrates the entire process of making a request
 *          to the free API. It builds the specialized payload, URL-encodes it,
 *          and performs the cURL request inside a retry loop to handle transient
 *          server errors (HTTP 503). It also correctly handles the case where
 *          the transfer is purposefully aborted by the write_callback.
 * @param state A pointer to the application's current state.
 * @param prompt The user's prompt for the current turn.
 * @return Returns true if the API call was successful, and false otherwise.
 */
bool send_free_api_request(AppState* state, const char* prompt) {
    // Determine which payload format to use.
    bool is_pro_model = (strstr(state->model_name, "pro") != NULL);

    // Build the payload once, as it's the same for all retries.
    char* freq_payload = build_free_request_payload(state, prompt, is_pro_model);
    if (!freq_payload) {
        fprintf(stderr, "Error: Failed to build free request payload.\n");
        return false;
    }

    long http_code = 0;
    CURLcode res = CURLE_OK;
    int max_retries = 3;

    for (int i = 0; i < max_retries; i++) {
        // All cURL resources must be initialized inside the loop for a clean retry.
        CURL* curl = curl_easy_init();
        if (!curl) {
            res = CURLE_FAILED_INIT;
            break; // Fatal error, no point retrying.
        }

        char* escaped_payload = curl_easy_escape(curl, freq_payload, 0);
        if (!escaped_payload) {
            fprintf(stderr, "Error: Failed to URL-encode payload.\n");
            curl_easy_cleanup(curl);
            res = CURLE_OUT_OF_MEMORY;
            continue; // Try again.
        }

        size_t post_fields_len = strlen("f.req=") + strlen(escaped_payload) + 1;
        char* post_fields = malloc(post_fields_len);
        snprintf(post_fields, post_fields_len, "f.req=%s", escaped_payload);
        curl_free(escaped_payload);

        MemoryStruct chunk = { .buffer = malloc(1), .size = 0 };
        chunk.buffer[0] = '\0';
        FreeCallbackData callback_data = { .mem = &chunk, .state = state };

        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded;charset=UTF-8");
        headers = curl_slist_append(headers, "Origin: https://gemini.google.com");
        headers = curl_slist_append(headers, "Referer: https://gemini.google.com/");
        
        //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

        curl_easy_setopt(curl, CURLOPT_URL, FREE_API_URL);
        if (state->proxy[0] != '\0') {
            curl_easy_setopt(curl, CURLOPT_PROXY, state->proxy);
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_fields);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_free_memory_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &callback_data);

        http_code = 0;
        res = curl_easy_perform(curl);
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        // Clean up all resources allocated for THIS specific attempt.
        free(post_fields);
        free(chunk.buffer);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        // --- Decision Logic for the current attempt ---

        // Case 1: Success (normal completion OR purposeful abort for --loc/--map)
        if ((res == CURLE_OK && http_code == 200) || (res == CURLE_WRITE_ERROR && state->loc_gathered)) {
            break; // Success, exit the retry loop.
        }

        // Case 2: Retryable server error
        if (http_code == 503) {
            fprintf(stderr, "\nAPI returned 503 (Service Unavailable), retrying... (%d/%d)\n", i + 1, max_retries);
            if (i < max_retries - 1) { // Don't sleep after the final attempt.
                #ifdef _WIN32
                    Sleep(2000); // 2 seconds
                #else
                    sleep(2);
                #endif
            }
        } else {
            // Case 3: Any other error is final, so don't retry.
            break;
        }
    }

    // This payload was allocated outside the loop, so it's freed once, here.
    free(freq_payload);

    // --- Final Return Logic ---
    // Check the final status from the last attempt.
    if ((res == CURLE_OK && http_code == 200) || (res == CURLE_WRITE_ERROR && state->loc_gathered)) {
        return true;
    }

    // If we're here, all retries failed.
    fprintf(stderr, "\nFree API call failed after retries (Last HTTP code: %ld, Curl error: %s)\n", http_code, curl_easy_strerror(res));
    return false;
}

/**
 * @brief Saves the current application settings to the config.json file.
 * @details This function serializes the configurable fields from the AppState
 *          struct (like model, temperature, API key, etc.) into a cJSON object.
 *          It then converts this object into a formatted string and writes it to
 *          the default configuration file path, overwriting any existing file.
 * @param state A pointer to the current application state containing the
 *              settings to be saved.
 */
void save_configuration(AppState* state) {
    char config_path[PATH_MAX];
    get_config_path(config_path, sizeof(config_path));
    if (config_path[0] == '\0') {
        fprintf(stderr, "Error: Could not determine configuration file path.\n");
        return;
    }

    // Create the root JSON object for the configuration.
    cJSON* root = cJSON_CreateObject();
    if (!root) {
        fprintf(stderr, "Error: Failed to create JSON object for configuration.\n");
        return;
    }

    // Add all configurable values from the state to the JSON object.
    cJSON_AddStringToObject(root, "host", state->host);
    cJSON_AddStringToObject(root, "model", state->model_name);
    cJSON_AddNumberToObject(root, "temperature", state->temperature);
    cJSON_AddNumberToObject(root, "seed", state->seed);
    if (state->system_prompt) {
        cJSON_AddStringToObject(root, "system_prompt", state->system_prompt);
    }
    if (state->proxy[0] != '\0') {
        cJSON_AddStringToObject(root, "proxy", state->proxy);
    }

    if (state->num_api_keys > 0) {
        cJSON* keys_array = cJSON_CreateStringArray((const char * const *)state->api_keys, state->num_api_keys);
        cJSON_AddItemToObject(root, "api_keys", keys_array);
        cJSON_AddNumberToObject(root, "next_key_index", state->next_key_index);
    }

    if (state->num_api_keys > 0) {
        cJSON* origins_array = cJSON_CreateStringArray((const char * const *)state->origins, state->num_api_keys);
        cJSON_AddItemToObject(root, "origins", origins_array);
    }
    
    cJSON_AddNumberToObject(root, "max_output_tokens", state->max_output_tokens);
    cJSON_AddNumberToObject(root, "thinking_budget", state->thinking_budget);
    cJSON_AddBoolToObject(root, "google_grounding", state->google_grounding);
    cJSON_AddBoolToObject(root, "url_context", state->url_context);
    // Only save topK and topP if they have been explicitly set.
    if (state->topK > 0) {
        cJSON_AddNumberToObject(root, "top_k", state->topK);
    }
    if (state->topP > 0.0f) {
        cJSON_AddNumberToObject(root, "top_p", state->topP);
    }

    // Convert the cJSON object to a formatted, human-readable string.
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root); // The JSON object is no longer needed.

    if (!json_string) {
        fprintf(stderr, "Error: Failed to format configuration to JSON string.\n");
        return;
    }

    // Open the configuration file for writing.
    FILE* file = fopen(config_path, "w");
    if (!file) {
        perror("Failed to open configuration file for writing");
        free(json_string);
        return;
    }

    // Write the JSON string to the file and clean up.
    fputs(json_string, file);
    fclose(file);
    free(json_string);

    fprintf(stderr, "Configuration saved to %s\n", config_path);
}

/**
 * @brief Performs a generic cURL GET request to a specified URL.
 * @details This is a helper function that configures and executes a standard
 *          HTTP GET request using libcurl. It is used for API calls that do not
 *          require a POST body, such as listing available models. It includes
 *          the necessary API key and origin headers.
 * @param url The full URL to request.
 * @param state The current application state, used for the API key and origin.
 * @param callback The libcurl write callback function to handle the response data.
 * @param callback_data A pointer to the data structure for the callback (e.g., MemoryStruct).
 * @return The HTTP status code of the response. On a transport-level error (e.g.,
 *         DNS failure), it returns a negative CURLcode.
 */
long perform_api_get_request(const char* url, AppState* state, size_t (*callback)(void*, size_t, size_t, void*), void* callback_data) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -CURLE_FAILED_INIT;
    }

    if (state->num_api_keys == 0) {
        fprintf(stderr, "Error: No API key provided.\n");
        return -1;
    }
    const char* current_key = state->api_keys[state->next_key_index];
    const char* current_origin = state->origins[state->next_key_index];
    state->next_key_index = (state->next_key_index + 1) % state->num_api_keys;
    
    // Prepare the required HTTP headers for authentication.
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", current_key);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, auth_header);

    // The 'Origin' header is optional and only added if not using the default.
    if (strcmp(current_origin, "default") != 0) {
        char origin_header[256];
        snprintf(origin_header, sizeof(origin_header), "Origin: %s", current_origin);
        headers = curl_slist_append(headers, origin_header);
    }

    // Configure the cURL handle for a GET request.
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (state->proxy[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_PROXY, state->proxy);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, callback_data);

    // Execute the request and retrieve the HTTP response code.
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // If the request failed at the transport layer (e.g., could not connect),
    // http_code will be 0. In this case, we return the negative cURL error code.
    if (res != CURLE_OK && http_code == 0) {
        http_code = -res;
    }

    // Clean up allocated resources.
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    
    return http_code;
}


/**
 * @brief Fetches and lists all available models from the Gemini API.
 * @details This function retrieves a list of all models accessible with the
 *          configured API key. It handles API pagination by making repeated
 *          GET requests if a `nextPageToken` is present in the response,
 *          ensuring that all available models are fetched and displayed.
 * @param state The current application state, used to provide the API key
 *              for the request.
 */
void list_available_models(AppState* state) {
    char next_page_token[256] = {0};
    bool first_page = true;
    int model_count = 0;

    // Allocate a memory buffer to store the API's JSON response.
    MemoryStruct chunk = { .buffer = malloc(1), .size = 0 };
    if (!chunk.buffer) {
        fprintf(stderr, "Error: Failed to allocate memory for API response.\n");
        return;
    }
    chunk.buffer[0] = '\0';

    fprintf(stderr, "Fetching available models...\n");

    // Loop to handle paginated results.
    do {
        char full_url[1024];

        // Construct the appropriate URL for the request.
        if (first_page) {
            snprintf(full_url, sizeof(full_url), "https://%s/v1beta/models?pageSize=50", state->host);
            first_page = false;
        } else {
            snprintf(full_url, sizeof(full_url), "https://%s/v1beta/models?pageSize=50&pageToken=%s", state->host, next_page_token);
        }

        // --- START OF MODIFICATION ---

        long http_code = 0;
        int max_retries = 3;

        for (int i = 0; i < max_retries; i++) {
            // Reset the response buffer for each new attempt.
            chunk.size = 0;
            chunk.buffer[0] = '\0';

            // Perform the GET request.
            http_code = perform_api_get_request(full_url, state, write_to_memory_struct_callback, &chunk);

            if (http_code == 200) {
                break; // Success, exit the retry loop.
            }

            if (http_code == 503) {
                fprintf(stderr, "\nAPI returned 503 (Service Unavailable), retrying... (%d/%d)\n", i + 1, max_retries);
                if (i < max_retries - 1) { // Don't sleep after the final attempt.
                    #ifdef _WIN32
                        Sleep(2000); // 2 seconds
                    #else
                        sleep(2);
                    #endif
                }
            } else {
                // Any other error is final, don't retry.
                break;
            }
        }

        // --- END OF MODIFICATION ---

        // This block now correctly handles the final result after all retries.
        if (http_code != 200) {
            fprintf(stderr, "\nAPI call to list models failed (Last HTTP code: %ld)\n", http_code);
            if(http_code < 0) fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(-http_code));
            parse_and_print_error_json(chunk.buffer);
            break; // Exit the pagination loop on failure.
        }

        // Parse the JSON response from the buffer.
        cJSON* root = cJSON_Parse(chunk.buffer);
        if (!root) {
            fprintf(stderr, "Error: Failed to parse JSON response for models list.\n");
            break;
        }

        // Extract and iterate over the array of models.
        cJSON* models_array = cJSON_GetObjectItem(root, "models");
        if (cJSON_IsArray(models_array)) {
            cJSON* model_item;
            cJSON_ArrayForEach(model_item, models_array) {
                cJSON* name = cJSON_GetObjectItem(model_item, "name");
                cJSON* display_name = cJSON_GetObjectItem(model_item, "displayName");

                if (cJSON_IsString(name)) {
                    const char* full_model_name = name->valuestring;
                    const char* prefix = "models/";
                    const char* name_to_print = (strncmp(full_model_name, prefix, strlen(prefix)) == 0)
                                                ? full_model_name + strlen(prefix)
                                                : full_model_name;
                    fprintf(stdout, "- %s (%s)\n", name_to_print, cJSON_IsString(display_name) ? display_name->valuestring : "N/A");
                    model_count++;
                }
            }
        }

        // Check for a token for the next page.
        const cJSON* token_item = cJSON_GetObjectItem(root, "nextPageToken");
        if (cJSON_IsString(token_item) && (token_item->valuestring != NULL)) {
            strncpy(next_page_token, token_item->valuestring, sizeof(next_page_token) - 1);
        } else {
            next_page_token[0] = '\0'; // No more pages.
        }

        cJSON_Delete(root);

    } while (next_page_token[0] != '\0');

    if (model_count > 0) {
        fprintf(stderr, "\nFound %d models.\n", model_count);
    } else {
        fprintf(stderr, "No models were found or an error occurred.\n");
    }

    free(chunk.buffer);
}

/**
 * @brief Exports the current conversation history to a human-readable Markdown file.
 * @details This function iterates through the entire conversation history stored in
 *          the AppState. It formats each user and model turn into a simple Markdown
 *          structure, including the system prompt, text content, and placeholders
 *          for file attachments. The output is saved to the specified file path.
 * @param state The current application state, containing the history to be exported.
 * @param filepath The path to the Markdown file that will be created or overwritten.
 */
void export_history_to_markdown(AppState* state, const char* filepath) {
    // Ensure the file path is safe and does not attempt directory traversal.
    if (!is_path_safe(filepath)) {
        fprintf(stderr, "Error: Unsafe or absolute file path specified: %s\n", filepath);
        return;
    }

    FILE* file = fopen(filepath, "w");
    if (!file) {
        perror("Failed to open file for export");
        return;
    }

    fprintf(stderr, "Exporting conversation to %s...\n", filepath);

    // First, write the system prompt to the file if one is set.
    if (state->system_prompt) {
        fprintf(file, "## System Prompt\n\n```\n%s\n```\n\n---\n\n", state->system_prompt);
    }

    // Iterate through each content block (turn) in the history.
    for (int i = 0; i < state->history.num_contents; i++) {
        Content* content = &state->history.contents[i];

        // Write the role ("User" or "Model") as a level-3 Markdown header.
        fprintf(file, "### %c%s\n\n", toupper((unsigned char)content->role[0]), content->role + 1);

        bool has_text = false;
        // Iterate through each part within the content block.
        for (int j = 0; j < content->num_parts; j++) {
            Part* part = &content->parts[j];
            if (part->type == PART_TYPE_TEXT && part->text) {
                // Write the text content directly.
                fprintf(file, "%s\n", part->text);
                has_text = true;
            } else if (part->type == PART_TYPE_FILE) {
                // For file attachments, write a placeholder indicating the file's name and type.
                const char* filename = part->filename ? part->filename : "Pasted Data";
                const char* mime_type = part->mime_type ? part->mime_type : "unknown";
                fprintf(file, "\n`[Attached File: %s (%s)]`\n", filename, mime_type);
            }
        }

        // Add extra spacing for readability if there was text content.
        if (has_text) {
            fprintf(file, "\n");
        }

        // Add a horizontal rule to separate turns, except after the very last one.
        if (i < state->history.num_contents - 1) {
            fprintf(file, "---\n\n");
        }
    }

    fclose(file);
    fprintf(stderr, "Successfully exported history to %s\n", filepath);
}

/**
 * @brief Gets the base path for the application's data directory.
 * @details This function provides a portable way to determine the correct location
 *          for storing configuration and session files. It creates the directory
 *          if it does not already exist.
 *          - On Windows, it resolves to `%APPDATA%\gemini-cli`.
 *          - On POSIX systems (Linux, macOS), it resolves to `~/.config/gemini-cli`.
 * @param buffer A character buffer to store the resulting path.
 * @param buffer_size The size of the buffer. The buffer will be empty on failure.
 */
void get_base_app_path(char* buffer, size_t buffer_size) {
    const char* config_dir_name = "gemini-cli";

#ifdef _WIN32
    // On Windows, the standard location is the APPDATA directory.
    char* base_path = getenv("APPDATA");
    if (!base_path) {
        buffer[0] = '\0'; // Unable to find APPDATA directory.
        return;
    }
    // Construct the full path: C:\Users\user\AppData\Roaming\gemini-cli
    snprintf(buffer, buffer_size, "%s\\%s", base_path, config_dir_name);
    MKDIR(buffer); // Create the directory if it doesn't exist.
#else
    // On POSIX systems, the standard is the .config directory in the user's home.
    char* base_path = getenv("HOME");
    if (!base_path) {
        buffer[0] = '\0'; // Unable to find HOME directory.
        return;
    }
    // First, ensure the ~/.config directory exists.
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/.config", base_path);
    MKDIR(dir_path);

    // Then, construct the full path: /home/user/.config/gemini-cli
    snprintf(buffer, buffer_size, "%s/.config/%s", base_path, config_dir_name);
    MKDIR(buffer); // Create the final directory if it doesn't exist.
#endif
}

/**
 * @brief Safely constructs the full file path for a named session.
 * @details This function combines the base sessions directory path with a
 *          user-provided session name to create a full, absolute path to a
 *          session's .json file. It performs critical safety checks to ensure
 *          the session name is valid and to prevent path traversal attacks.
 * @param session_name The name of the session, provided by the user.
 * @param path_buffer A buffer to store the resulting full file path.
 * @param buffer_size The size of the `path_buffer`.
 * @return Returns true if the path was constructed successfully and safely.
 *         Returns false if the session name is unsafe or if the resulting
 *         path would be too long for the buffer.
 */
bool build_session_path(const char* session_name, char* path_buffer, size_t buffer_size) {
    // First, validate the session name to prevent directory traversal.
    if (!is_session_name_safe(session_name)) {
        // The is_session_name_safe function prints its own specific error message.
        return false;
    }

    // Get the base path for the "sessions" subdirectory (e.g., ~/.config/gemini-cli/sessions).
    char sessions_path[PATH_MAX];
    get_sessions_path(sessions_path, sizeof(sessions_path));
    if (sessions_path[0] == '\0') {
        // This can happen if the base app path could not be determined.
        return false;
    }

    // Define the correct path separator for the operating system.
#ifdef _WIN32
    const char* separator = "\\";
#else
    const char* separator = "/";
#endif

    // Pre-calculate the required buffer size to prevent overflow with snprintf.
    // Required size = base_path + separator + session_name + ".json" + null_terminator
    size_t required_size = strlen(sessions_path) + strlen(separator) + strlen(session_name) + 5 + 1;
    if (required_size > buffer_size) {
        fprintf(stderr, "Error: Session name '%s' results in a path that is too long.\n", session_name);
        return false;
    }

    // Safely construct the final, full path.
    snprintf(path_buffer, buffer_size, "%s%s%s.json", sessions_path, separator, session_name);
    return true;
}

/**
 * @brief Sends a request to the official Gemini API and handles the response.
 * @details This is the primary function for interacting with the official Gemini
 *          API. It builds the JSON request payload, Gzip-compresses it for
 *          efficiency, sends it via a POST request, and processes the streaming
 *          SSE response. The full, concatenated response from the model is
 *          returned upon success.
 * @param state The current application state, containing the history, configuration,
 *              and API key needed for the request.
 * @param[out] full_response_out A pointer to a character pointer. On success, this
 *             will be updated to point to a newly allocated string containing the
 *             complete model response. The caller is responsible for freeing this
 *             memory.
 * @return Returns true if the API call was successful (HTTP 200), and false otherwise.
 */
bool send_api_request(AppState* state, char** full_response_out) {
    *full_response_out = NULL;

    // 1. Build and compress the payload once. It's the same for all retries.
    cJSON* root = build_request_json(state);
    if (!root) {
        fprintf(stderr, "Error: Failed to build JSON request.\n");
        return false;
    }
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_string) {
        fprintf(stderr, "Error: Failed to print JSON to string.\n");
        return false;
    }
    GzipResult compressed_result = gzip_compress((unsigned char*)json_string, strlen(json_string));
    free(json_string);
    if (!compressed_result.data) {
        fprintf(stderr, "Error: Failed to compress request payload.\n");
        return false;
    }

    // 2. Prepare the memory structure. We allocate it once and reuse/reset it.
    MemoryStruct chunk = { .buffer = malloc(1), .size = 0, .full_response = malloc(1), .full_response_size = 0 };
    if (!chunk.buffer || !chunk.full_response) {
        fprintf(stderr, "Error: Failed to allocate memory for curl response chunk.\n");
        free(compressed_result.data);
        if(chunk.buffer) free(chunk.buffer);
        if(chunk.full_response) free(chunk.full_response);
        return false;
    }

    long http_code = 0;
    bool success = false;
    int max_retries = 3;

    for (int i = 0; i < max_retries; i++) {
        // 3. Reset buffers for this attempt to clear data from any previous failed attempt.
        chunk.buffer[0] = '\0';
        chunk.size = 0;
        chunk.full_response[0] = '\0';
        chunk.full_response_size = 0;

        // 4. Perform the API request.
        http_code = perform_api_curl_request(
            state,
            "streamGenerateContent?alt=sse",
            (const char*)compressed_result.data,
            compressed_result.size,
            write_memory_callback,
            &chunk
        );

        // 5. Decide if this attempt was successful, retryable, or a final failure.
        if (http_code == 200) {
            success = true;
            break; // Success, exit the loop.
        }

        if (http_code == 503) {
            fprintf(stderr, "\nAPI returned 503 (Service Unavailable), retrying... (%d/%d)\n", i + 1, max_retries);
            if (i < max_retries - 1) { // Don't sleep after the final attempt.
                #ifdef _WIN32
                    Sleep(2000); // 2 seconds
                #else
                    sleep(2);
                #endif
            }
        } else {
            // Any other error is final, don't retry.
            break;
        }
    }

    // 6. Handle the final result after the loop is finished.
    if (success) {
        *full_response_out = chunk.full_response;
    } else if (http_code == 403) {
        size_t last_index = (state->next_key_index + state->num_api_keys - 1) % state->num_api_keys;
    	  char *current_key = state->api_keys[last_index];
    	  char *current_origin = state->origins[last_index];
    	  size_t key_len = strlen(current_key);
        fprintf(stderr, "\nAPI returned 403 (Unauthorized) for key: %.4s...%.4s (%s)\n", current_key + 6, current_key + key_len - 4, current_origin);
        return false;
    } else {
        fprintf(stderr, "\nAPI call failed after retries (Last HTTP code: %ld)\n", http_code);
        if(http_code < 0) fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(-http_code));
        parse_and_print_error_json(chunk.buffer);
        free(chunk.full_response); // Free the unused response buffer on failure.
    }

    // 7. Clean up all remaining resources.
    free(chunk.buffer);
    free(compressed_result.data);
    return success;

}

/**
 * @brief Safely reads a string value from a cJSON object into a fixed-size buffer.
 * @param obj The cJSON object to read from.
 * @param key The key of the string value to read.
 * @param buffer The character buffer to store the string.
 * @param buffer_size The size of the buffer.
 */
static void json_read_string(const cJSON* obj, const char* key, char* buffer, size_t buffer_size) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        strncpy(buffer, item->valuestring, buffer_size - 1);
        buffer[buffer_size - 1] = '\0'; // Ensure null-termination.
    }
}

/**
 * @brief Safely reads a float value from a cJSON object.
 * @param obj The cJSON object to read from.
 * @param key The key of the float value to read.
 * @param target A pointer to the float variable to update.
 */
static void json_read_float(const cJSON* obj, const char* key, float* target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        *target = (float)item->valuedouble;
    }
}

/**
 * @brief Safely reads an integer value from a cJSON object.
 * @param obj The cJSON object to read from.
 * @param key The key of the integer value to read.
 * @param target A pointer to the integer variable to update.
 */
static void json_read_int(const cJSON* obj, const char* key, int* target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        *target = item->valueint;
    }
}

/**
 * @brief Safely reads a boolean value from a cJSON object.
 * @details It handles both true/false boolean types and numeric 0/1 values
 *          for better compatibility.
 * @param obj The cJSON object to read from.
 * @param key The key of the boolean value to read.
 * @param target A pointer to the bool variable to update.
 */
static void json_read_bool(const cJSON* obj, const char* key, bool* target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(item)) {
        *target = cJSON_IsTrue(item);
    } else if (cJSON_IsNumber(item)) { // For compatibility with config files using 0/1
        *target = (item->valueint != 0);
    }
}

/**
 * @brief Parses command-line options and updates the application state.
 * @details This function iterates through the command-line arguments, looking for
 *          recognized flags (e.g., -m, --temp, --free). When a flag is found,
 *          it updates the corresponding field in the AppState struct. The parsing
 *          stops when the first non-option argument (like a prompt word or
 *          filename) is encountered.
 * @param argc The argument count from main().
 * @param argv The argument vector from main().
 * @param state A pointer to the AppState struct to be updated.
 * @return The index of the first argument that was not a recognized option. This
 *         allows the main session logic to process the remaining arguments as
 *         part of the initial prompt or as file attachments.
 */

/*
  Enum of all recognized options
*/
typedef enum {
    OPT_UNKNOWN,
    OPT_MODEL, OPT_HOST, OPT_SYSTEM, OPT_CONFIG,
    OPT_TEMP, OPT_PROXY, OPT_SEED, OPT_MAX_TOKENS,
    OPT_TOPK, OPT_TOPP, OPT_BUDGET,
    OPT_EXECUTE, OPT_QUIET, OPT_NO_GROUNDING, OPT_FREE, OPT_NO_URL_CONTEXT,
    OPT_LOC, OPT_MAP,
    OPT_LIST_KEYS, OPT_ADD_KEY, OPT_REMOVE_KEY, OPT_CHECK_KEYS,
    OPT_LIST_MODELS, OPT_LIST_SESSIONS, OPT_SAVE_SESSION, OPT_LOAD_SESSION,
    OPT_HELP
} OptionType;

/*
  Map a flag string to its OptionType
*/
static OptionType classify_option(const char *arg) {
    if (!STRCASECMP(arg, "-m")          || !STRCASECMP(arg, "--model"))          return OPT_MODEL;
    if (!STRCASECMP(arg, "-H")          || !STRCASECMP(arg, "--host"))           return OPT_HOST;
    if (!STRCASECMP(arg, "-S")          || !STRCASECMP(arg, "--system"))         return OPT_SYSTEM;
    if (!STRCASECMP(arg, "-c")          || !STRCASECMP(arg, "--config"))         return OPT_CONFIG;
    if (!STRCASECMP(arg, "-t")          || !STRCASECMP(arg, "--temp"))           return OPT_TEMP;
    if (!STRCASECMP(arg, "-p")          || !STRCASECMP(arg, "--proxy"))          return OPT_PROXY;
    if (!STRCASECMP(arg, "-s")          || !STRCASECMP(arg, "--seed"))           return OPT_SEED;
    if (!STRCASECMP(arg, "-o")          || !STRCASECMP(arg, "--max-tokens"))     return OPT_MAX_TOKENS;
    if (!STRCASECMP(arg, "--topk"))                                              return OPT_TOPK;
    if (!STRCASECMP(arg, "--topp"))                                              return OPT_TOPP;
    if (!STRCASECMP(arg, "-b")          || !STRCASECMP(arg, "--budget"))         return OPT_BUDGET;
    if (!STRCASECMP(arg, "-e")          || !STRCASECMP(arg, "--execute"))        return OPT_EXECUTE;
    if (!STRCASECMP(arg, "-q")          || !STRCASECMP(arg, "--quiet"))          return OPT_QUIET;
    if (!STRCASECMP(arg, "-ng")         || !STRCASECMP(arg, "--no-grounding"))   return OPT_NO_GROUNDING;
    if (!STRCASECMP(arg, "-f")          || !STRCASECMP(arg, "--free"))           return OPT_FREE;
    if (!STRCASECMP(arg, "-nu")         || !STRCASECMP(arg, "--no-url-context")) return OPT_NO_URL_CONTEXT;
    if (!STRCASECMP(arg, "--loc"))                                               return OPT_LOC;
    if (!STRCASECMP(arg, "--map"))                                               return OPT_MAP;
    if (!STRCASECMP(arg, "--list-keys"))                                         return OPT_LIST_KEYS;
    if (!STRCASECMP(arg, "--add-key"))                                           return OPT_ADD_KEY;
    if (!STRCASECMP(arg, "--remove-key"))                                        return OPT_REMOVE_KEY;
    if (!STRCASECMP(arg, "--check-keys"))                                        return OPT_CHECK_KEYS;
    if (!STRCASECMP(arg, "-l")          || !STRCASECMP(arg, "--list"))           return OPT_LIST_MODELS;
    if (!STRCASECMP(arg, "--list-sessions"))                                     return OPT_LIST_SESSIONS;
    if (!STRCASECMP(arg, "--save-session"))                                      return OPT_SAVE_SESSION;
    if (!STRCASECMP(arg, "--load-session"))                                      return OPT_LOAD_SESSION;
    if (!STRCASECMP(arg, "-h")          || !STRCASECMP(arg, "--help"))           return OPT_HELP;
    return OPT_UNKNOWN;
}

int parse_common_options(int argc, char *argv[], AppState *state) {
    for (int i = 1; i < argc; i++) {
        const char   *arg      = argv[i];
        const char   *next_arg = (i + 1 < argc ? argv[i + 1] : NULL);
        OptionType    opt      = classify_option(arg);

        switch (opt) {

            // --- Model and Configuration ---
            case OPT_MODEL:
                if (next_arg) {
                    strncpy(state->model_name,
                            next_arg,
                            sizeof(state->model_name) - 1);
                    i++;
                }
                break;

            case OPT_HOST:
                if (next_arg) {
                    if (state->host) {
                        free(state->host);
                    }
                    state->host = strdup(next_arg);
                    i++;
                }
                break;

            case OPT_SYSTEM:
                if (next_arg) {
                    free(state->system_prompt);
                    state->system_prompt = strdup(next_arg);
                    i++;
                }
                break;

            case OPT_CONFIG:
                if (next_arg) {
                    // config already loaded; just skip its value
                    i++;
                }
                break;


            // --- Generation Parameters ---
            case OPT_TEMP:
                if (next_arg) {
                    state->temperature = atof(next_arg);
                    i++;
                }
                break;

            case OPT_PROXY:
                if (next_arg) {
                    snprintf(state->proxy,
                             sizeof(state->proxy),
                             "%s",
                             next_arg);
                    i++;
                }
                break;

            case OPT_SEED:
                if (next_arg) {
                    state->seed = atoi(next_arg);
                    i++;
                }
                break;

            case OPT_MAX_TOKENS:
                if (next_arg) {
                    state->max_output_tokens = atoi(next_arg);
                    i++;
                }
                break;

            case OPT_TOPK:
                if (next_arg) {
                    state->topK = atoi(next_arg);
                    i++;
                }
                break;

            case OPT_TOPP:
                if (next_arg) {
                    state->topP = atof(next_arg);
                    i++;
                }
                break;

            case OPT_BUDGET:
                if (next_arg) {
                    state->thinking_budget = atoi(next_arg);
                    i++;
                }
                break;


            // --- Boolean Flags ---
            case OPT_EXECUTE:
            case OPT_QUIET:
                // handled elsewhere, just consume
                break;

            case OPT_NO_GROUNDING:
                state->google_grounding = false;
                break;

            case OPT_FREE:
                state->free_mode = true;
                break;

            case OPT_NO_URL_CONTEXT:
                state->url_context = false;
                break;

            case OPT_LOC:
                state->loc_tile |= 1;
                break;

            case OPT_MAP:
                state->loc_tile |= 2;
                break;


            // --- Immediate Actions (exit) ---
            case OPT_LIST_KEYS: {
                fprintf(stdout, "--- API Keys in Config ---\n");
                if (state->num_api_keys == 0) {
                    fprintf(stdout, "  (No API keys found in configuration)\n");
                } else {
                    for (int j = 0; j < state->num_api_keys; j++) {
                        const char *key    = state->api_keys[j];
                        const char *origin = state->origins[j];
                        size_t key_len     = strlen(key);
                        fprintf(stdout, "  [%d] ", j);
                        if (key_len > 11)
                            fprintf(stdout,
                                    "%.4s...%.4s (%s)",
                                    key + 6,
                                    key + key_len - 4,
                                    origin);
                        else
                            fprintf(stdout,
                                    "%.*s... (%s)",
                                    (int)key_len,
                                    key,
                                    origin);
                        fprintf(stdout, "\n");
                    }
                }
                exit(0);
            }

            case OPT_ADD_KEY:
                get_api_key_securely(state);
                save_configuration(state);
                exit(0);

            case OPT_REMOVE_KEY:
                if (next_arg) {
                    long idx = strtol(next_arg, NULL, 10);
                    if (idx < 0 || idx >= state->num_api_keys) {
                        fprintf(stderr, "Error: Invalid key index.\n");
                    } else {
                        free(state->api_keys[idx]);
                        if (idx < state->num_api_keys - 1) {
                            memmove(&state->api_keys[idx],
                                    &state->api_keys[idx + 1],
                                    sizeof(char*) *
                                      (state->num_api_keys - idx - 1));
                        }
                        state->num_api_keys--;
                        if (state->next_key_index >= state->num_api_keys &&
                            state->num_api_keys > 0) {
                            state->next_key_index = 0;
                        }
                        save_configuration(state);
                        fprintf(stdout,
                                "Removed key at index %ld from configuration.\n",
                                idx);
                    }
                    i++;
                }
                exit(0);

            case OPT_CHECK_KEYS: {
                fprintf(stdout, "Checking API keys from configuration...\n");
                if (state->num_api_keys == 0) {
                    fprintf(stdout, "  (No keys to check)\n");
                } else {
                    int original_next = state->next_key_index;
                    for (int j = 0; j < state->num_api_keys; j++) {
                        state->next_key_index = j;
                        int token_count       = get_token_count(state);
                        const char *key       = state->api_keys[j];
                        size_t key_len        = strlen(key);
                        fprintf(stdout, "  [%d] ", j);
                        if (key_len > 11)
                            fprintf(stdout,
                                    "%.4s...%.4s",
                                    key + 6,
                                    key + key_len - 4);
                        else
                            fprintf(stdout,
                                    "%.*s...",
                                    (int)key_len,
                                    key);
                        if (token_count >= 0)
                            fprintf(stdout, " : OK\n");
                        else
                            fprintf(stdout, " : FAILED\n");
                    }
                    state->next_key_index = original_next;
                }
                exit(0);
            }

            case OPT_LIST_MODELS:
                list_available_models(state);
                exit(0);

            case OPT_LIST_SESSIONS:
                list_sessions();
                exit(0);

            case OPT_SAVE_SESSION:
                if (next_arg) {
                    free(state->save_session_path);
                    state->save_session_path = strdup(next_arg);
                    i++;
                }
                break;

            case OPT_LOAD_SESSION:
                if (next_arg) {
                    char file_path[PATH_MAX];
                    if (build_session_path(next_arg,
                                            file_path,
                                            sizeof(file_path))) {
                        load_history_from_file(state, file_path);
                        strncpy(state->current_session_name,
                                next_arg,
                                sizeof(state->current_session_name) - 1);
                    }
                    i++;
                }
                break;

            case OPT_HELP:
                print_usage(argv[0]);
                exit(0);

            case OPT_UNKNOWN:
            default:
                // first non-option → return its index
                return i;
        }
    }

    // all args parsed as options
    return argc;
}

/**
 * @brief Prints the command-line usage instructions and exits.
 * @details This function displays a comprehensive help message that outlines the
 *          program's modes of operation and lists all available command-line
 *          options with brief descriptions. It is typically called when the
 *          user provides the -h or --help flag.
 * @param prog_name The name of the executable (from argv[0]).
 */
void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options] [prompt or files...]\n\n", prog_name);
    fprintf(stderr, "A portable, feature-rich command-line client for the Google Gemini API.\n\n");
    fprintf(stderr, "The client operates in two modes:\n");
    fprintf(stderr, "  - Interactive Mode: (Default) A full chat session with history and commands.\n");
    fprintf(stderr, "  - Non-Interactive Mode: Engaged if stdin or stdout is piped.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -c, --config <path>       Load configuration from a specific file path.\n");
    fprintf(stderr, "  -m, --model <name>        Specify the model name (e.g., gemini-1.5-pro-latest).\n");
    fprintf(stderr, "  -t, --temp <float>        Set the generation temperature (e.g., 1.0).\n");
    fprintf(stderr, "  -s, --seed <int>          Set the random seed for reproducible outputs.\n");
    fprintf(stderr, "  -S, --system <prompt>     Set a system prompt for the entire session.\n");
    fprintf(stderr, "  -o, --max-tokens <int>    Set the maximum number of tokens in the response.\n");
    fprintf(stderr, "  -b, --budget <int>        Set the model's max 'thinking' token budget.\n");
    fprintf(stderr, "  -p, --proxy <url>         Specify a proxy to use (e.g., 'http://localhost:8080').\n");
    fprintf(stderr, "      --topk <int>          Set the Top-K sampling parameter.\n");
    fprintf(stderr, "      --topp <float>        Set the Top-P (nucleus) sampling parameter.\n");
    fprintf(stderr, "  -e, --execute             Execute a single prompt non-interactively and exit.\n");
    fprintf(stderr, "  -q, --quiet               Enable quiet mode; print only the final response to stdout.\n");
    fprintf(stderr, "  -f, --free                Use the unofficial, key-free API endpoint.\n");
    fprintf(stderr, "      --loc                 Get location information (requires --free mode).\n");
    fprintf(stderr, "      --map                 Get map URL for location (requires --free mode).\n");
    fprintf(stderr, "  -ng, --no-grounding       Disable Google Search grounding for the response.\n");
    fprintf(stderr, "  -nu, --no-url-context     Disable automatic fetching of URL context.\n");
    fprintf(stderr, "  -l, --list                List all available models and exit.\n");
    fprintf(stderr, "      --list-sessions       List all saved sessions and exit.\n");
    fprintf(stderr, "      --load-session <name> Load a saved session by name and start chatting.\n");
    fprintf(stderr, "      --save-session <file> Save the conversation to a file after a non-interactive run.\n");
    fprintf(stderr, "\nKey Management:\n");
    fprintf(stderr, "      --list-keys           List API keys from the configuration file and exit.\n");
    fprintf(stderr, "      --add-key <key>       Add a new API key to the configuration file and exit.\n");
    fprintf(stderr, "      --remove-key <index>  Remove an API key from the configuration file and exit.\n");
    fprintf(stderr, "      --check-keys          Validate all configured API keys and exit.\n\n");
    fprintf(stderr, "  -h, --help                Show this help message and exit.\n\n");
    fprintf(stderr, "For a list of in-session commands (like /save, /attach), start interactive mode and type /help.\n");
}

/**
 * @brief Sets the application state to its default values.
 * @details This function initializes a new AppState struct by first clearing it
 *          with `memset` and then populating it with the standard default
 *          settings for the model, generation parameters, and features. This
 *          ensures the application starts in a consistent state before any
 *          user configuration is loaded.
 * @param state A pointer to the AppState struct to be initialized.
 */
void initialize_default_state(AppState* state) {
    // Start with a clean, zeroed-out state struct to prevent undefined behavior.
    memset(state, 0, sizeof(AppState));

    // --- Set default values ---
    strncpy(state->current_session_name, "[unsaved]", sizeof(state->current_session_name) - 1);
    strncpy(state->origin, "default", sizeof(state->origin) - 1);

    // Default model and generation parameters.
    strncpy(state->model_name, DEFAULT_MODEL_NAME, sizeof(state->model_name) - 1);
    state->temperature = 0.75f;
    state->seed = 42;
    state->max_output_tokens = 65536; // A high default limit.

    // Default feature toggles.
    state->google_grounding = true;
    state->url_context = true;

    // Default values indicating that these parameters are not set by default.
    // The API will use its own defaults for these.
    state->thinking_budget = -1;
    state->topK = -1;
    state->topP = -1.0f;

    // The application starts in official API mode by default.
    state->free_mode = false;

    // Ensure all string buffers are properly null-terminated.
    state->model_name[sizeof(state->model_name) - 1] = '\0';
    state->current_session_name[sizeof(state->current_session_name) - 1] = '\0';
    state->origin[sizeof(state->origin) - 1] = '\0';

    state->api_keys = NULL;
    state->origins = NULL;
    
    state->num_api_keys = 0;
    state->next_key_index = 0;
    
    // Ensure pointers are initialized to NULL.
    state->last_free_response_part = NULL;
    state->last_model_response = NULL;
    state->system_prompt = NULL;
    state->final_code = NULL;
    
    state->loc_tile = 0;
    state->loc_gathered = false;
    
    state->save_session_path = NULL;
    
    state->host = strdup("generativelanguage.googleapis.com");
}

/**
 * @brief Resets the current session to a clean state.
 * @details This function is invoked by the `/clear` or `/session new` command.
 *          It clears the entire conversation history, frees the last model
 *          response, removes any system prompt, clears all pending file
 *          attachments, and resets the session name to "[unsaved]". This allows
 *          the user to start a completely new conversation without restarting
 *          the program.
 * @param state A pointer to the AppState struct to be cleared.
 */
void clear_session_state(AppState* state) {
    // Deallocate all memory associated with the conversation history.
    free_history(&state->history);

    // Free the buffers holding the last responses from both API modes.
    if (state->last_model_response) {
        free(state->last_model_response);
        state->last_model_response = NULL;
    }
    if (state->last_free_response_part) {
        free(state->last_free_response_part);
        state->last_free_response_part = NULL;
    }

    // Free the system prompt if it was set.
    if (state->system_prompt) {
        free(state->system_prompt);
        state->system_prompt = NULL;
    }

    if (state->final_code) {
        free(state->final_code);
        state->final_code = NULL;
    }
    
    // Clear any files that were attached but not yet sent with a prompt.
    free_pending_attachments(state);

    // Reset the session name to its default.
    strncpy(state->current_session_name, "[unsaved]", sizeof(state->current_session_name) - 1);
    state->current_session_name[sizeof(state->current_session_name) - 1] = '\0';

    fprintf(stderr, "New session started.\n");
}

/**
 * @brief Gets the path for the 'sessions' subdirectory, creating it if needed.
 * @details This function constructs the path to the directory where all named
 *          session files are stored. It builds upon the base application path
 *          (e.g., `~/.config/gemini-cli`) and appends the 'sessions' subdirectory
 *          name, creating it on the filesystem if it does not already exist.
 * @param buffer A character buffer to store the resulting path.
 * @param buffer_size The size of the buffer. The buffer will be empty on failure.
 */
void get_sessions_path(char* buffer, size_t buffer_size) {
    const char* sessions_dir_name = "sessions";
    char base_app_path[PATH_MAX];

    // First, get the main application data directory path.
    get_base_app_path(base_app_path, sizeof(base_app_path));
    if (base_app_path[0] == '\0') {
        buffer[0] = '\0'; // Abort if the base path could not be determined.
        return;
    }

    // Construct the full path to the sessions subdirectory using the
    // correct path separator for the current operating system.
#ifdef _WIN32
    snprintf(buffer, buffer_size, "%s\\%s", base_app_path, sessions_dir_name);
#else
    snprintf(buffer, buffer_size, "%s/%s", base_app_path, sessions_dir_name);
#endif

    // Create the sessions directory if it doesn't already exist.
    MKDIR(buffer);
}

/**
 * @brief Validates a session name to ensure it is safe for use as a filename.
 * @details This is a security function designed to prevent path traversal
 *          attacks. It checks that the user-provided session name does not
 *          contain any characters that could be used to navigate the filesystem
 *          (e.g., '/', '\', '.').
 * @param name The session name string to validate.
 * @return Returns true if the name is safe, and false otherwise. A specific
 *         error message is printed to stderr if the name is found to be unsafe.
 */
bool is_session_name_safe(const char* name) {
    if (name == NULL || name[0] == '\0') {
        return false;
    }

    // Disallow directory separators and dots to prevent path traversal.
    // For example, a name like "../my_secrets" would be rejected.
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '.')) {
        fprintf(stderr, "Error: Session name cannot contain '/', '\\', or '.' characters.\n");
        return false;
    }

    return true;
}

/**
 * @brief Lists all saved session files from the sessions directory.
 * @details This function reads the contents of the sessions directory, filters
 *          for files ending in ".json", and prints their names (without the
 *          extension) to the console. It uses platform-specific APIs for
 *          directory traversal to ensure portability between Windows and
 *          POSIX systems.
 */
void list_sessions() {
    char sessions_path[PATH_MAX];
    get_sessions_path(sessions_path, sizeof(sessions_path));
    if (sessions_path[0] == '\0') {
        fprintf(stderr, "Error: Could not determine sessions directory.\n");
        return;
    }

    fprintf(stderr, "Saved Sessions:\n");
    int count = 0;

#ifdef _WIN32
    // --- Windows Implementation ---
    char search_path[PATH_MAX];
    snprintf(search_path, sizeof(search_path), "%s\\*.json", sessions_path);

    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);

    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "  (No sessions found)\n");
        return;
    }

    // Iterate through all found .json files.
    do {
        // Temporarily remove the .json extension for clean printing.
        char* dot = strrchr(fd.cFileName, '.');
        if (dot && strcmp(dot, ".json") == 0) {
            *dot = '\0';
            fprintf(stderr, "  - %s\n", fd.cFileName);
            count++;
        }
    } while (FindNextFile(hFind, &fd) != 0);

    FindClose(hFind);

    if (count == 0) {
        fprintf(stderr, "  (No sessions found)\n");
    }

#else
    // --- POSIX (Linux, macOS) Implementation ---
    DIR *d = opendir(sessions_path);
    if (d) {
        struct dirent *dir;
        // Iterate through all entries in the directory.
        while ((dir = readdir(d)) != NULL) {
            // Check if the entry is a file ending in .json.
            char* dot = strrchr(dir->d_name, '.');
            if (dot && strcmp(dot, ".json") == 0) {
                *dot = '\0'; // Temporarily remove extension.
                fprintf(stderr, "  - %s\n", dir->d_name);
                *dot = '.'; // Restore it for the next check (good practice).
                count++;
            }
        }
        closedir(d);
    }

    if (count == 0) {
        fprintf(stderr, "  (No sessions found)\n");
    }
#endif
}

/**
 * @brief Gets the full path for the application's configuration file.
 * @details This function constructs the absolute path to the `config.json` file.
 *          It builds upon the base application path and appends the standard
 *          configuration filename.
 *          - On Windows, this resolves to `%APPDATA%\gemini-cli\config.json`.
 *          - On POSIX systems, this resolves to `~/.config/gemini-cli/config.json`.
 * @param buffer A character buffer to store the resulting path.
 * @param buffer_size The size of the buffer. The buffer will be empty on failure.
 */
void get_config_path(char* buffer, size_t buffer_size) {
    const char* config_file_name = "config.json";
    char base_app_path[PATH_MAX];

    // First, get the main application data directory path.
    get_base_app_path(base_app_path, sizeof(base_app_path));
    if (base_app_path[0] == '\0') {
        buffer[0] = '\0'; // Abort if the base path could not be determined.
        return;
    }

    // Define the correct path separator for the current operating system.
#ifdef _WIN32
    const char* separator = "\\";
#else
    const char* separator = "/";
#endif

    // Pre-calculate the required buffer size to prevent an overflow with snprintf.
    // Required size = base_path + separator + config_file_name + null_terminator
    size_t required_size = strlen(base_app_path) + strlen(separator) + strlen(config_file_name) + 1;
    if (required_size > buffer_size) {
        fprintf(stderr, "Error: Resolved configuration path is too long.\n");
        buffer[0] = '\0';
        return;
    }

    // Safely construct the final, full path.
    snprintf(buffer, buffer_size, "%s%s%s", base_app_path, separator, config_file_name);
}

/**
 * @brief Safely reads a string from a cJSON object and allocates new memory for it.
 * @details This is used for fields that can be of variable length, like the
 *          system prompt. The caller is responsible for freeing the memory
 *          allocated to `*target`.
 * @param obj The cJSON object to read from.
 * @param key The key of the string value to read.
 * @param target A pointer to the char pointer that will hold the new string.
 *               If `*target` already points to a string, it will be freed first.
 */
static void json_read_strdup(const cJSON* obj, const char* key, char** target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        // Free the existing string, if any, to prevent memory leaks.
        if (*target) {
            free(*target);
        }
        *target = strdup(item->valuestring);
    }
}

/**
 * @brief Loads application settings from a specified configuration file path.
 * @details This function reads a JSON configuration file from the given path,
 *          parses it, and updates the application's state with the values found
 *          in the file. It uses a series of `json_read_*` helper functions to
 *          safely extract the values and handle missing keys gracefully.
 * @param state The application state struct to update with the loaded settings.
 * @param filepath The absolute path to the `config.json` file to load.
 */
void load_configuration_from_path(AppState* state, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        // This is not an error if the default config file doesn't exist on first run,
        // so we don't print an error message here. We simply return.
        return;
    }

    // Read the entire file content into a buffer.
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);

    char* buffer = malloc(length + 1);
    if (!buffer) {
        fclose(file);
        fprintf(stderr, "Warning: Failed to allocate memory to read config file.\n");
        return;
    }

    if (fread(buffer, 1, length, file) != (size_t)length) {
        fclose(file);
        free(buffer);
        fprintf(stderr, "Warning: Failed to read content from config file.\n");
        return;
    }
    buffer[length] = '\0';
    fclose(file);

    // Parse the buffer content as a JSON object.
    cJSON* root = cJSON_Parse(buffer);
    free(buffer); // The file content is now in the cJSON object.

    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        fprintf(stderr, "Warning: Could not parse configuration file '%s' or it is not a valid JSON object.\n", filepath);
        return;
    }

    // Use the helper functions to safely read each value from the JSON object
    // into the AppState struct. This gracefully handles missing keys.
    json_read_string(root, "model", state->model_name, sizeof(state->model_name));
    json_read_strdup(root, "host", &state->host);
    json_read_float(root, "temperature", &state->temperature);
    json_read_int(root, "seed", &state->seed);
    json_read_strdup(root, "system_prompt", &state->system_prompt);
    json_read_string(root, "proxy", state->proxy, sizeof(state->proxy));

    json_read_int(root, "next_key_index", &state->next_key_index);

    const cJSON* keys_array = cJSON_GetObjectItem(root, "api_keys");
    const cJSON* origins_array = cJSON_GetObjectItem(root, "origins");
    if (cJSON_IsArray(keys_array)) {
        // New format: "api_keys": ["key1", "key2"]
        state->num_api_keys = cJSON_GetArraySize(keys_array);
        state->api_keys = malloc(sizeof(char*) * state->num_api_keys);
        state->origins = malloc(sizeof(char*) * state->num_api_keys);
        cJSON* key_item;
        cJSON* origin_item;
        int i = 0;
        cJSON_ArrayForEach(key_item, keys_array) {
            if (cJSON_IsString(key_item)) {
                state->api_keys[i++] = strdup(key_item->valuestring);
            }
        }
        i = 0;
        cJSON_ArrayForEach(origin_item, origins_array) {
            if (cJSON_IsString(origin_item)) {
                state->origins[i++] = strdup(origin_item->valuestring);
            }
        }
    } else {
        // Backward compatibility: "api_key": "key1"
        const cJSON* single_key = cJSON_GetObjectItem(root, "api_key");
        const cJSON* single_origin = cJSON_GetObjectItem(root, "origin");
        if (cJSON_IsString(single_key)) {
            state->num_api_keys = 1;
            state->api_keys = malloc(sizeof(char*));
            state->api_keys[0] = strdup(single_key->valuestring);
            state->origins = malloc(sizeof(char*));
            state->origins[0] = strdup(single_origin->valuestring);
        }
    }
    
    // Ensure index is not out of bounds
    if (state->num_api_keys > 0 && state->next_key_index >= state->num_api_keys) {
        state->next_key_index = 0;
    }
    
    //json_read_string(root, "origin", state->origin, sizeof(state->origin));
    json_read_int(root, "max_output_tokens", &state->max_output_tokens);
    json_read_int(root, "thinking_budget", &state->thinking_budget);
    json_read_bool(root, "google_grounding", &state->google_grounding);
    json_read_bool(root, "url_context", &state->url_context);
    json_read_int(root, "top_k", &state->topK);
    json_read_float(root, "top_p", &state->topP);

    // Clean up the parsed JSON object.
    cJSON_Delete(root);
}

/**
 * @brief Loads application settings from the default configuration file.
 * @details This is a convenience wrapper around `load_configuration_from_path`.
 *          It determines the default path for the `config.json` file and then
 *          calls the main loading function to parse it and update the state.
 * @param state The application state struct to update with the loaded settings.
 */
void load_configuration(AppState* state) {
    char config_path[PATH_MAX];

    // Get the default configuration file path (e.g., ~/.config/gemini-cli/config.json).
    get_config_path(config_path, sizeof(config_path));

    // If the path could not be determined, abort.
    if (config_path[0] == '\0') {
        return;
    }

    // Load the configuration from that specific path.
    load_configuration_from_path(state, config_path);
}

/**
 * @brief Prompts the user and reads their input without echoing it to the screen.
 * @details This function provides a secure way to get sensitive information, like
 *          an API key, from the user. It displays a prompt and then reads user
 *          input character-by-character, printing an asterisk '*' for each one
 *          instead of the character itself. It correctly handles backspace for
 *          editing and is implemented with platform-specific terminal controls
 *          to work on both Windows and POSIX-compliant systems.
 * @param prompt The message to display to the user before reading input.
 * @param buffer The character buffer where the user's input will be stored.
 * @param buffer_size The total size of the buffer.
 */
void get_masked_input(const char* prompt, char* buffer, size_t buffer_size) {
    fprintf(stderr, "%s", prompt);
    fflush(stderr); // Ensure the prompt is displayed immediately.

    memset(buffer, 0, buffer_size);
    size_t i = 0;

#ifdef _WIN32
    // --- Windows Implementation using conio.h ---
    char ch;
    while (i < buffer_size - 1) {
        ch = _getch(); // Reads a character directly without waiting for Enter.

        if (ch == '\r' || ch == '\n') { // User pressed Enter.
            break;
        } else if (ch == '\b') { // User pressed Backspace.
            if (i > 0) {
                i--;
                fprintf(stderr, "\b \b"); // Erase the last asterisk from the screen.
            }
        } else if (isprint((unsigned char)ch)) { // A printable character was entered.
            buffer[i++] = ch;
            fprintf(stderr, "*");
        }
    }
#else
    // --- POSIX (Linux, macOS) Implementation using termios.h ---
    struct termios old_term, new_term;

    // Get the current terminal settings.
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;

    // Disable canonical mode (line buffering) and character echoing.
    new_term.c_lflag &= ~(ECHO | ICANON);

    // Apply the new, temporary settings.
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);

    int c;
    // Read character by character.
    while (i < buffer_size - 1 && (c = getchar()) != '\n' && c != '\r' && c != EOF) {
        if (c == 127 || c == 8) { // ASCII for Backspace/Delete.
            if (i > 0) {
                i--;
                fprintf(stderr, "\b \b"); // Erase the last asterisk from the screen.
            }
        } else if (isprint(c)) { // A printable character was entered.
            buffer[i++] = (char)c;
            fprintf(stderr, "*");
            fflush(stderr);
        }
    }

    // IMPORTANT: Restore the original terminal settings.
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
#endif

    buffer[i] = '\0'; // Always null-terminate the final string.
    fprintf(stderr, "\n"); // Move to the next line after input is complete.
}

/**
 * @brief Securely gets the API key and (optionally) the origin from the user.
 * @details This function orchestrates the process of obtaining user credentials.
 *          It uses the `get_masked_input` helper function to prompt the user for
 *          their API key. If the origin has not already been set (i.e., it is
 *          still "default"), it will also prompt for the origin, allowing the
 *          user to skip it by pressing Enter.
 * @param state A pointer to the AppState struct where the key and origin will be
 *              stored.
 */
void get_api_key_securely(AppState* state) {
    char key_buffer[128];
    get_masked_input("Enter your API Key: ", key_buffer, sizeof(key_buffer));

    // If no API key entered, do nothing
    if (key_buffer[0] == '\0') {
        return;
    }

    // 1) Append new API key
    state->num_api_keys += 1;
    state->api_keys = realloc(
        state->api_keys,
        state->num_api_keys * sizeof *state->api_keys
    );
    state->api_keys[state->num_api_keys - 1] = strdup(key_buffer);

    // 2) Prompt for origin (Enter → "default")
    char origin_buffer[128];
    get_masked_input(
      "Enter your Origin (press Enter for 'default'): ",
      origin_buffer,
      sizeof(origin_buffer)
    );

    // 3) Append origin in lock-step with api_keys count
    state->origins = realloc(
        state->origins,
        state->num_api_keys * sizeof *state->origins
    );

    if (origin_buffer[0] == '\0') {
        state->origins[state->num_api_keys - 1] = strdup("default");
    } else {
        state->origins[state->num_api_keys - 1] = strdup(origin_buffer);
    }
    
}


/**
 * @brief Constructs the main JSON request object from the application state.
 * @details This function builds the complete cJSON object that serves as the
 *          payload for a `generateContent` API call. It serializes the different
 *          parts of the AppState into the format required by the Gemini API,
 *          including the system prompt, the conversation history, tool
 *          configurations (like grounding), and generation parameters.
 * @param state A pointer to the application's current state.
 * @return A pointer to the root cJSON object of the request. The caller is
 *         responsible for freeing this object with `cJSON_Delete`. Returns
 *         NULL on failure.
 */
cJSON* build_request_json(AppState* state) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;

    // --- 1. Add System Instruction (if provided) ---
    if (state->system_prompt) {
        cJSON* sys_instruction = cJSON_CreateObject();
        cJSON* sys_parts_array = cJSON_CreateArray();
        cJSON* sys_part_item = cJSON_CreateObject();

        cJSON_AddStringToObject(sys_part_item, "text", state->system_prompt);
        cJSON_AddItemToArray(sys_parts_array, sys_part_item);
        cJSON_AddItemToObject(sys_instruction, "parts", sys_parts_array);
        cJSON_AddItemToObject(root, "systemInstruction", sys_instruction);
    }

    // --- 2. Add Contents (the conversation history) ---
    cJSON* contents = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "contents", contents);
    for (int i = 0; i < state->history.num_contents; i++) {
        cJSON* content_item = cJSON_CreateObject();
        cJSON_AddStringToObject(content_item, "role", state->history.contents[i].role);

        cJSON* parts_array = cJSON_CreateArray();
        cJSON_AddItemToObject(content_item, "parts", parts_array);

        for (int j = 0; j < state->history.contents[i].num_parts; j++) {
            Part* current_part = &state->history.contents[i].parts[j];
            cJSON* part_item = cJSON_CreateObject();

            if (current_part->type == PART_TYPE_TEXT) {
                if (current_part->text) {
                    cJSON_AddStringToObject(part_item, "text", current_part->text);
                }
            } else { // PART_TYPE_FILE
                cJSON* inline_data = cJSON_CreateObject();
                cJSON_AddStringToObject(inline_data, "mimeType", current_part->mime_type);
                cJSON_AddStringToObject(inline_data, "data", current_part->base64_data);
                cJSON_AddItemToObject(part_item, "inlineData", inline_data);
            }
            cJSON_AddItemToArray(parts_array, part_item);
        }
        cJSON_AddItemToArray(contents, content_item);
    }

    // --- 3. Add Tools Configuration ---
    // Only add the "tools" object if at least one tool is enabled.
    if (state->url_context || state->google_grounding) {
        cJSON* tools_array = cJSON_CreateArray();
        if (state->url_context) {
            cJSON* tool1 = cJSON_CreateObject();
            cJSON_AddItemToObject(tool1, "urlContext", cJSON_CreateObject());
            cJSON_AddItemToArray(tools_array, tool1);
        }
        if (state->google_grounding) {
            cJSON* tool2 = cJSON_CreateObject();
            cJSON_AddItemToObject(tool2, "googleSearch", cJSON_CreateObject());
            cJSON_AddItemToArray(tools_array, tool2);
        }
        cJSON_AddItemToObject(root, "tools", tools_array);
    }

    // --- 4. Add Generation Configuration ---
    cJSON* gen_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(gen_config, "temperature", state->temperature);
    cJSON_AddNumberToObject(gen_config, "maxOutputTokens", state->max_output_tokens);
    cJSON_AddNumberToObject(gen_config, "seed", state->seed);
    if (state->topK > 0) {
        cJSON_AddNumberToObject(gen_config, "topK", state->topK);
    }
    if (state->topP > 0.0f) {
        cJSON_AddNumberToObject(gen_config, "topP", state->topP);
    }

    // Add thinking budget as a sub-object within generationConfig.
    cJSON* thinking_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(thinking_config, "thinkingBudget", state->thinking_budget);
    cJSON_AddItemToObject(gen_config, "thinkingConfig", thinking_config);

    cJSON_AddItemToObject(root, "generationConfig", gen_config);

    return root;
}

/**
 * @brief Parses a JSON error response from the API and prints a clean message.
 * @details When an API call fails, the body of the HTTP response often contains
 *          a JSON object with error details. This function attempts to parse that
 *          JSON, find the "message" field within the "error" object, and print
 *          it to stderr for the user. If parsing fails, it prints the raw
 *          buffer as a fallback.
 * @param error_buffer The raw response body received from a failed API call.
 */
void parse_and_print_error_json(const char* error_buffer) {
    if (!error_buffer) return;

    // The actual JSON object may be preceded by other text in the error buffer.
    // Find the opening brace to ensure we parse only the JSON part.
    const char* json_start = strchr(error_buffer, '{');
    if (!json_start) {
        // If no JSON object is found, print the raw error buffer as-is.
        fprintf(stderr, "API Error: %s\n", error_buffer);
        return;
    }

    cJSON* root = cJSON_Parse(json_start);
    if (!root) {
        // If parsing fails, we can't extract a specific message.
        return;
    }

    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON* message = cJSON_GetObjectItem(error, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            // Print the clean, user-friendly error message from the API.
            fprintf(stderr, "API Error Message: %s\n", message->valuestring);
        }
    }

    cJSON_Delete(root);
}

/**
 * @brief A libcurl write callback for non-streaming API responses.
 * @details This function is used for API calls that return a complete JSON
 *          object in a single response (e.g., `countTokens`). It simply
 *          appends all received data chunks into the MemoryStruct's buffer
 *          until the request is finished.
 * @param contents A pointer to the data received.
 * @param size The size of each data member (always 1 for text).
 * @param nmemb The number of data members received.
 * @param userp A user-defined pointer to a MemoryStruct to store the data.
 * @return The total number of bytes successfully handled.
 */
static size_t write_to_memory_struct_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;

    // Expand the buffer to accommodate the new data chunk.
    char* ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) {
        fprintf(stderr, "Error: realloc failed in token count callback.\n");
        return 0; // Signal an error to libcurl.
    }
    mem->buffer = ptr;

    // Append the new data and update the size.
    memcpy(&(mem->buffer[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = '\0'; // Ensure the buffer is always null-terminated.

    return realsize;
}

/**
 * @brief Calculates the token count for the current conversation history.
 * @details This function sends the current conversation contents to the Gemini
 *          API's `countTokens` endpoint. It builds a request similar to a
 *          standard generation request but omits unnecessary fields like
 *          `generationConfig`. It then parses the response to extract the integer
 *          value for the total token count.
 * @param state The current application state, containing the history to be counted.
 * @return The integer token count on success, or -1 on failure.
 */
int get_token_count(AppState* state) {
    // Build the request JSON, which includes history and system prompt.
    cJSON* root = build_request_json(state);
    if (!root) return -1;

    // The countTokens endpoint does not use these fields, so remove them.
    cJSON_DeleteItemFromObject(root, "generationConfig");
    cJSON_DeleteItemFromObject(root, "tools");

    // Serialize and compress the payload.
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_string) return -1;

    GzipResult compressed_result = gzip_compress((unsigned char*)json_string, strlen(json_string));
    free(json_string);
    if (!compressed_result.data) {
        fprintf(stderr, "Failed to compress payload for token count.\n");
        return -1;
    }

    // Prepare a memory buffer for the API response.
    MemoryStruct chunk = { .buffer = malloc(1), .size = 0 };
    if (!chunk.buffer) {
        free(compressed_result.data);
        return -1;
    }
    chunk.buffer[0] = '\0';

    // Perform the API call to the "countTokens" endpoint.
    long http_code = perform_api_curl_request(
        state,
        "countTokens",
        (const char*)compressed_result.data,
        compressed_result.size,
        write_to_memory_struct_callback, // Use the simple, non-streaming callback.
        &chunk
    );

    int token_count = -1;
    if (http_code == 200) {
        // If successful, parse the JSON response.
        cJSON* json_resp = cJSON_Parse(chunk.buffer);
        if (json_resp) {
            // Extract the "totalTokens" value.
            cJSON* tokens = cJSON_GetObjectItem(json_resp, "totalTokens");
            if (cJSON_IsNumber(tokens)) {
                token_count = tokens->valueint;
            }
            cJSON_Delete(json_resp);
        }
    } else {
        // On failure, print the error.
        fprintf(stderr, "Token count API call failed (HTTP code: %ld)\n", http_code);
        if (http_code < 0) fprintf(stderr, "Curl error: %s\n", curl_easy_strerror(-http_code));
        parse_and_print_error_json(chunk.buffer);
    }

    // Clean up resources.
    free(compressed_result.data);
    free(chunk.buffer);
    return token_count;
}

/**
 * @brief Saves the current conversation state to a JSON file.
 * @details This function serializes the entire application state, including the
 *          conversation history, system prompt, and configuration, into a JSON
 *          object using `build_request_json`. It then saves this JSON to the
 *          specified file path, allowing a session to be resumed later.
 * @param state A pointer to the current application state to be saved.
 * @param filepath The path of the file where the history will be saved.
 */
void save_history_to_file(AppState* state, const char* filepath) {
    FILE* file = fopen(filepath, "w");
    if (!file) {
        perror("Failed to open file for writing");
        return;
    }

    // Use the existing function to build a complete JSON representation of the state.
    cJSON* root = build_request_json(state);
    if (!root) {
        fclose(file);
        return;
    }

    // Convert the cJSON object to a formatted, human-readable string.
    char* json_string = cJSON_Print(root);
    cJSON_Delete(root);

    if (json_string) {
        fputs(json_string, file);
        free(json_string);
    }

    fclose(file);
    fprintf(stderr, "Conversation history saved to %s\n", filepath);
}

/**
 * @brief Loads a conversation state from a JSON file.
 * @details This function reads a JSON file from the given path, parses it, and
 *          repopulates the application's state. It clears any existing history
 *          before loading the new data. It carefully iterates through the JSON
 *          structure to reconstruct the `contents` (history), `systemInstruction`,
 *          and other settings.
 * @param state A pointer to the AppState struct that will be overwritten with the
 *              loaded data.
 * @param filepath The path of the file from which to load the history.
 */
void load_history_from_file(AppState* state, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) {
        perror("Failed to open file for reading");
        return;
    }

    // Read the entire file content into a memory buffer.
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) {
        fclose(file);
        return;
    }
    if (fread(buffer, 1, length, file) != (size_t)length) {
        fclose(file);
        free(buffer);
        fprintf(stderr, "Error reading file content.\n");
        return;
    }
    buffer[length] = '\0';
    fclose(file);

    // Parse the buffer into a cJSON object.
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!cJSON_IsObject(root)) {
        fprintf(stderr, "Error: JSON file is not a valid history object.\n");
        if(root) cJSON_Delete(root);
        return;
    }

    // --- Repopulate the AppState ---

    // 1. Clear existing history before loading the new session.
    free_history(&state->history);

    // 2. Load the conversation history ("contents").
    cJSON* contents = cJSON_GetObjectItem(root, "contents");
    if (cJSON_IsArray(contents)) {
        cJSON* content_item;
        cJSON_ArrayForEach(content_item, contents) {
            cJSON* role_json = cJSON_GetObjectItem(content_item, "role");
            cJSON* parts_array = cJSON_GetObjectItem(content_item, "parts");
            if (!cJSON_IsString(role_json) || !cJSON_IsArray(parts_array)) continue;

            int num_parts = cJSON_GetArraySize(parts_array);
            Part* loaded_parts = calloc(num_parts, sizeof(Part)); // Use calloc for zero-initialization
            if (!loaded_parts) continue;

            cJSON* part_item;
            int part_idx = 0;
            cJSON_ArrayForEach(part_item, parts_array) {
                if (part_idx >= num_parts) break; // Should not happen, but safe
                cJSON* text_json = cJSON_GetObjectItem(part_item, "text");
                cJSON* inline_data_json = cJSON_GetObjectItem(part_item, "inlineData");

                if (cJSON_IsString(text_json)) {
                    loaded_parts[part_idx].type = PART_TYPE_TEXT;
                    loaded_parts[part_idx].text = strdup(text_json->valuestring);
                } else if (inline_data_json) {
                    cJSON* mime_json = cJSON_GetObjectItem(inline_data_json, "mimeType");
                    cJSON* data_json = cJSON_GetObjectItem(inline_data_json, "data");
                    if (cJSON_IsString(mime_json) && cJSON_IsString(data_json)) {
                        loaded_parts[part_idx].type = PART_TYPE_FILE;
                        loaded_parts[part_idx].mime_type = strdup(mime_json->valuestring);
                        loaded_parts[part_idx].base64_data = strdup(data_json->valuestring);
                    }
                }
                part_idx++;
            }
            add_content_to_history(&state->history, role_json->valuestring, loaded_parts, num_parts);

            // Free the temporary parts structure.
            for (int i = 0; i < num_parts; i++) {
                if (loaded_parts[i].text) free(loaded_parts[i].text);
                if (loaded_parts[i].mime_type) free(loaded_parts[i].mime_type);
                if (loaded_parts[i].base64_data) free(loaded_parts[i].base64_data);
            }
            free(loaded_parts);
        }
    }

    // 3. Load the system prompt.
    cJSON* sys_instruction = cJSON_GetObjectItem(root, "systemInstruction");
    if (sys_instruction) {
        cJSON* parts_array = cJSON_GetObjectItem(sys_instruction, "parts");
        if (cJSON_IsArray(parts_array)) {
            cJSON* part_item = cJSON_GetArrayItem(parts_array, 0);
            if (part_item) {
                cJSON* text_item = cJSON_GetObjectItem(part_item, "text");
                if (cJSON_IsString(text_item)) {
                    if (state->system_prompt) free(state->system_prompt);
                    state->system_prompt = strdup(text_item->valuestring);
                }
            }
        }
    }

    cJSON_Delete(root);
    fprintf(stderr, "Conversation history loaded from %s\n", filepath);
}

/**
 * @brief Adds a new content block (a user or model turn) to the conversation history.
 * @details This function appends a new `Content` struct to the history array.
 *          It performs a deep copy of the role string and all the `Part` structs
 *          provided, ensuring that the history owns its own memory.
 * @param history A pointer to the History struct to be modified.
 * @param role The role for this turn, either "user" or "model".
 * @param parts An array of Part structs that make up this turn's content.
 * @param num_parts The number of parts in the `parts` array.
 */
void add_content_to_history(History* history, const char* role, Part* parts, int num_parts) {
    // Expand the contents array to make room for the new entry.
    Content* new_contents = realloc(history->contents, sizeof(Content) * (history->num_contents + 1));
    if (!new_contents) {
        fprintf(stderr, "Error: realloc failed when adding to history.\n");
        return;
    }
    history->contents = new_contents;

    // Get a pointer to the new content block at the end of the array.
    Content* new_content = &history->contents[history->num_contents];
    new_content->role = strdup(role);
    new_content->num_parts = num_parts;
    new_content->parts = malloc(sizeof(Part) * num_parts);

    if (!new_content->parts || !new_content->role) {
        fprintf(stderr, "Error: malloc failed for new history content.\n");
        // Attempt to roll back the realloc if allocation fails here.
        if (new_content->role) free(new_content->role);
        if (new_content->parts) free(new_content->parts);
        history->contents = realloc(history->contents, sizeof(Content) * history->num_contents);
        return;
    }

    // Perform a deep copy of each part from the input array into the history.
    for (int i = 0; i < num_parts; i++) {
        new_content->parts[i].type = parts[i].type;
        if (parts[i].type == PART_TYPE_TEXT) {
            new_content->parts[i].text = parts[i].text ? strdup(parts[i].text) : NULL;
            new_content->parts[i].mime_type = NULL;
            new_content->parts[i].base64_data = NULL;
            new_content->parts[i].filename = NULL;
        } else { // PART_TYPE_FILE
            new_content->parts[i].text = NULL;
            new_content->parts[i].mime_type = parts[i].mime_type ? strdup(parts[i].mime_type) : NULL;
            new_content->parts[i].base64_data = parts[i].base64_data ? strdup(parts[i].base64_data) : NULL;
            new_content->parts[i].filename = parts[i].filename ? strdup(parts[i].filename) : NULL;
        }
    }
    history->num_contents++;
}

/**
 * @brief Frees all memory associated with a single Content struct.
 * @details This is a helper function for cleaning up history. It deallocates the
 *          memory for the role string and for each individual part within the
 *          content block, including text, MIME types, Base64 data, and filenames.
 * @param content A pointer to the Content struct to be freed.
 */
void free_content(Content* content) {
    if (!content) return;

    // Free the role string (e.g., "user", "model").
    if (content->role) free(content->role);

    // Free the data within each part of the content.
    if (content->parts) {
        for (int i = 0; i < content->num_parts; i++) {
            if (content->parts[i].text) free(content->parts[i].text);
            if (content->parts[i].mime_type) free(content->parts[i].mime_type);
            if (content->parts[i].base64_data) free(content->parts[i].base64_data);
            if (content->parts[i].filename) free(content->parts[i].filename);
        }
        // Free the array of parts itself.
        free(content->parts);
    }
}

/**
 * @brief Frees all memory used by pending attachments.
 * @details This function is called to clear the list of attachments that have
 *          been prepared but not yet sent with a prompt. It iterates through the
 *          `attached_parts` array and frees all dynamically allocated fields.
 * @param state A pointer to the AppState containing the attachments to clear.
 */
void free_pending_attachments(AppState* state) {
    for (int i = 0; i < state->num_attached_parts; i++) {
        // Free all possible dynamically allocated fields for each pending part.
        if (state->attached_parts[i].text) free(state->attached_parts[i].text);
        if (state->attached_parts[i].filename) free(state->attached_parts[i].filename);
        if (state->attached_parts[i].mime_type) free(state->attached_parts[i].mime_type);
        if (state->attached_parts[i].base64_data) free(state->attached_parts[i].base64_data);
    }
    // Reset the counter to zero, effectively clearing the list.
    state->num_attached_parts = 0;
}

/**
 * @brief Frees all memory associated with the entire conversation history.
 * @details This function iterates through every content block in the history,
 *          calling `free_content` on each one to perform a deep clean. Finally,
 *          it frees the top-level `contents` array itself and resets the history.
 * @param history A pointer to the History struct to be completely freed.
 */
void free_history(History* history) {
    if (!history) return;

    // Free each content block in the history array.
    for (int i = 0; i < history->num_contents; i++) {
        free_content(&history->contents[i]);
    }

    // Free the array of content blocks itself.
    if (history->contents) {
        free(history->contents);
    }

    // Reset the history to a clean, empty state.
    history->contents = NULL;
    history->num_contents = 0;
}

/**
 * @brief Compresses data using the Gzip algorithm.
 * @details This function takes a buffer of input data and compresses it using
 *          zlib's deflate functionality with Gzip headers. This is used to
 *          reduce the size of the JSON payload sent to the Gemini API, which can
 *          improve network performance for large requests.
 * @param input_data A pointer to the raw data to be compressed.
 * @param input_size The size of the input data in bytes.
 * @return A GzipResult struct containing a pointer to the compressed data and
 *         its size. The `data` field will be NULL on failure. The caller is
 *         responsible for freeing the `data` buffer.
 */
GzipResult gzip_compress(const unsigned char* input_data, size_t input_size) {
    GzipResult result = { .data = NULL, .size = 0 };
    z_stream strm = {0};

    // Initialize the zlib stream for Gzip compression.
    // 15 + 16 enables Gzip headers.
    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return result; // Return empty result on failure.
    }

    strm.avail_in = input_size;
    strm.next_in = (Bytef*)input_data;

    unsigned char out_chunk[GZIP_CHUNK_SIZE];

    // Compress the data in chunks until all input is processed.
    do {
        strm.avail_out = GZIP_CHUNK_SIZE;
        strm.next_out = out_chunk;

        // Perform the compression. Z_FINISH tells zlib that this is the last chunk.
        int ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END && ret != Z_OK) {
            deflateEnd(&strm);
            if (result.data) free(result.data);
            return (GzipResult){NULL, 0}; // Return empty result on failure.
        }

        // Calculate how much compressed data was produced in this chunk.
        size_t have = GZIP_CHUNK_SIZE - strm.avail_out;
        if (have > 0) {
            // Expand the result buffer and append the new compressed data.
            unsigned char* new_data = realloc(result.data, result.size + have);
            if (!new_data) {
                deflateEnd(&strm);
                if (result.data) free(result.data);
                return (GzipResult){NULL, 0}; // Return empty result on failure.
            }
            result.data = new_data;
            memcpy(result.data + result.size, out_chunk, have);
            result.size += have;
        }
    } while (strm.avail_out == 0); // Continue if the output chunk was filled completely.

    // Clean up the zlib stream.
    deflateEnd(&strm);
    return result;
}

/**
 * @brief Validates a file path to ensure it is safe.
 * @details This is a security function that checks for common path traversal
 *          patterns. It rejects any path that contains ".." and also rejects
 *          absolute paths, ensuring that file operations are restricted to the
 *          current working directory and its subdirectories.
 * @param path The file path string to validate.
 * @return Returns true if the path is deemed safe, and false otherwise.
 */
bool is_path_safe(const char* path) {
    if (path == NULL || path[0] == '\0') {
        return false;
    }

    // Reject paths containing ".." to prevent directory traversal attacks.
    if (strstr(path, "..")) {
        return false;
    }

    // Reject absolute paths.
#ifdef _WIN32
    // Rejects "C:\path" or "\path"
    if (path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':')) {
        return false;
    }
#else
    // Rejects "/path"
    if (path[0] == '/') {
        return false;
    }
#endif

    return true;
}

/**
 * @brief Reads data from a stream and creates a pending file attachment.
 * @details This function is a robust, production-ready handler for all file and
 *          stream-based attachments (`/attach`, `/paste`, piped input). It safely
 *          acquires a file handle, reads the entire contents into a buffer, and
 *          then creates a `Part` struct. The attachment is handled differently
 *          based on the API mode: in official mode, it's Base64-encoded; in
 *          free mode, it's formatted as plain text. It uses a goto-based cleanup
 *          to ensure all resources are deallocated correctly on all execution paths.
 * @param stream An optional, pre-opened file stream. If NULL, the function will
 *               open the file specified by `filepath`.
 * @param filepath A descriptive name for the source (e.g., filename or "stdin").
 *                 If `stream` is NULL, this is used as the path to open.
 * @param mime_type The MIME type of the data, used in official API mode.
 * @param state The application state where the new attachment part will be added.
 */
void handle_attachment_from_stream(FILE* stream, const char* filepath, const char* mime_type, AppState* state) {
    // --- Variable Declarations ---
    FILE* input_stream = stream;
    unsigned char* buffer = NULL;
    char* formatted_text = NULL;
    bool opened_here = false;
    size_t total_read = 0;

    // --- 1. Pre-flight Checks ---
    if (state->num_attached_parts >= ATTACHMENT_LIMIT) {
        fprintf(stderr, "Error: Attachment limit of %d reached.\n", ATTACHMENT_LIMIT);
        return;
    }

    // --- 2. Resource Acquisition ---
    if (input_stream == NULL) {
        if (!is_path_safe(filepath)) {
            fprintf(stderr, "Error: Unsafe or absolute file path specified: %s\n", filepath);
            return;
        }
        input_stream = fopen(filepath, "rb");
        if (!input_stream) {
            perror("Error opening file");
            return;
        }
        opened_here = true;
    }

    // --- 3. Read Stream into Buffer ---
    int fd = fileno(input_stream);
    struct stat st;
    if (fstat(fd, &st) != 0) {
        perror("Error getting file status");
        goto cleanup;
    }

    // Check if the stream is a regular file
    if (S_ISREG(st.st_mode)) {
        fseek(input_stream, 0, SEEK_END);
        long file_size = ftell(input_stream);
        fseek(input_stream, 0, SEEK_SET);

        if (file_size <= 0) {
            fprintf(stderr, "Warning: File '%s' is empty or invalid. Attachment skipped.\n", filepath);
            goto cleanup;
        }
        buffer = malloc(file_size + 1);
        if (!buffer) {
            fprintf(stderr, "Error: Failed to allocate memory for file buffer.\n");
            goto cleanup;
        }
        total_read = fread(buffer, 1, file_size, input_stream);
        if (total_read != (size_t)file_size) {
            fprintf(stderr, "Error reading from file '%s'.\n", filepath);
            goto cleanup;
        }
    } else { // Stream is not a regular file (it's a pipe or the console)
        size_t capacity = 4096;
        buffer = malloc(capacity);
        if (!buffer) {
            fprintf(stderr, "Error: Failed to allocate memory for stream buffer.\n");
            goto cleanup;
        }

        #ifdef _WIN32
            // On Windows, check if stdin is an interactive console (a TTY)
            if (_isatty(fd) && input_stream == stdin) {
                fprintf(stderr, "Reading from stdin. Press Ctrl+D or Ctrl+Z, Enter when done.\n");
                int c;
                // Read character by character to manually check for Ctrl+D
                while ((c = fgetc(input_stream)) != EOF) {
                    if (c == 4) { // ASCII for Ctrl+D is 4
                        break; // Exit loop on Ctrl+D
                    }
                    if (total_read >= capacity - 1) {
                        capacity *= 2;
                        unsigned char* new_buffer = realloc(buffer, capacity);
                        if (!new_buffer) {
                            fprintf(stderr, "Error: Failed to reallocate stream buffer.\n");
                            goto cleanup;
                        }
                        buffer = new_buffer;
                    }
                    buffer[total_read++] = (unsigned char)c;
                }
            } else {
        #endif
                // On POSIX, or for a non-interactive pipe on Windows,
                // the existing block-read logic is safe and efficient.
                ssize_t bytes_read;
                while ((bytes_read = read(fd, buffer + total_read, 1024)) > 0) {
                    total_read += (size_t)bytes_read;
                    if (capacity - total_read < 1024) {
                        capacity *= 2;
                        unsigned char* new_buffer = realloc(buffer, capacity);
                        if (!new_buffer) {
                            fprintf(stderr, "Error: Failed to reallocate pipe buffer.\n");
                            goto cleanup;
                        }
                        buffer = new_buffer;
                    }
                }
                if (bytes_read < 0) {
                    perror("Error reading from input stream");
                    goto cleanup;
                }
        #ifdef _WIN32
            } // end of the windows-specific 'if'
        #endif
    }

    if (total_read == 0) {
        fprintf(stderr, "Warning: No data received from input stream. Attachment skipped.\n");
        goto cleanup;
    }
    buffer[total_read] = '\0';

    // --- 4. Create Attachment Part (This logic remains the same) ---
    Part* part = &state->attached_parts[state->num_attached_parts];
    memset(part, 0, sizeof(Part));

    if (state->free_mode) {
        if (strcmp(filepath, "stdin") == 0) {
            const char* format = "\n--- Pasted Text ---\n%s\n--- End of Pasted Text ---\n";
            size_t len = snprintf(NULL, 0, format, buffer);
            formatted_text = malloc(len + 1);
            if (formatted_text) sprintf(formatted_text, format, buffer);
        } else {
            const char* format = "\n--- Attached File: %s ---\n%s\n--- End of File ---\n";
            size_t len = snprintf(NULL, 0, format, filepath, buffer);
            formatted_text = malloc(len + 1);
            if (formatted_text) sprintf(formatted_text, format, filepath, buffer);
        }

        if (!formatted_text) {
            fprintf(stderr, "Error: Failed to allocate memory for formatted text.\n");
            goto cleanup;
        }
        part->type = PART_TYPE_TEXT;
        part->text = formatted_text;
    } else { // Official API mode
        part->type = PART_TYPE_FILE;
        part->filename = strdup(filepath);
        part->mime_type = strdup(mime_type);
        part->base64_data = base64_encode(buffer, total_read);

        if (!part->filename || !part->mime_type || !part->base64_data) {
             fprintf(stderr, "Error: Failed to allocate memory for attachment metadata.\n");
             if (part->filename) free(part->filename);
             if (part->mime_type) free(part->mime_type);
             if (part->base64_data) free(part->base64_data);
             goto cleanup;
        }
    }

    fprintf(stderr, "Attached %s (MIME: %s, Size: %zu bytes)\n",
            state->free_mode ? "stdin/file" : part->filename,
            state->free_mode ? "text/plain" : part->mime_type,
            total_read);
    (state->num_attached_parts)++;

cleanup:
    if (buffer) {
        free(buffer);
    }
    if (opened_here && input_stream) {
        fclose(input_stream);
    }
}

/**
 * @brief Encodes binary data into a Base64 string.
 * @details This function implements the standard Base64 encoding algorithm. It
 *          takes a buffer of binary data and converts it into a null-terminated
 *          ASCII string suitable for embedding in JSON payloads.
 * @param data A pointer to the raw binary data to be encoded.
 * @param input_length The size of the input data in bytes.
 * @return A dynamically allocated, null-terminated Base64 string. The caller
 *         is responsible for freeing this memory. Returns NULL on failure.
 */
char* base64_encode(const unsigned char* data, size_t input_length) {
    static const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // Calculate the length of the output string.
    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = malloc(output_length + 1);
    if (!encoded_data) return NULL;

    // Process the input data in 3-byte chunks, converting them to 4 Base64 characters.
    for (size_t i = 0, j = 0; i < input_length;) {
        uint32_t octet_a = i < input_length ? data[i++] : 0;
        uint32_t octet_b = i < input_length ? data[i++] : 0;
        uint32_t octet_c = i < input_length ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) + (octet_b << 8) + octet_c;

        encoded_data[j++] = b64_chars[(triple >> 18) & 0x3F];
        encoded_data[j++] = b64_chars[(triple >> 12) & 0x3F];
        encoded_data[j++] = b64_chars[(triple >> 6) & 0x3F];
        encoded_data[j++] = b64_chars[triple & 0x3F];
    }

    // Add padding characters ('=') if the input length was not a multiple of 3.
    static const int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++) {
        encoded_data[output_length - 1 - i] = '=';
    }

    encoded_data[output_length] = '\0';
    return encoded_data;
}

/**
 * @brief Performs the low-level cURL request for the official Gemini API.
 * @details This is the core transport function for all POST requests to the
 *          official API. It constructs the full API URL, sets the required
 *          HTTP headers (including content-type, encoding, and API key), and
 *          executes the cURL request with the provided payload and callback.
 * @param state The current application state, used for model name, API key, and origin.
 * @param endpoint The specific API endpoint to call (e.g., "streamGenerateContent").
 * @param compressed_payload The Gzipped request body.
 * @param payload_size The size of the compressed payload.
 * @param callback The libcurl write callback function to handle the response data.
 * @param callback_data A pointer to the data structure for the callback (e.g., MemoryStruct).
 * @return The HTTP status code of the response. On a transport-level error,
 *         it returns a negative CURLcode.
 */
long perform_api_curl_request(AppState* state, const char* endpoint, const char* compressed_payload, size_t payload_size, size_t (*callback)(void*, size_t, size_t, void*), void* callback_data) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return -CURLE_FAILED_INIT;
    }

    if (state->num_api_keys == 0) {
        fprintf(stderr, "Error: No API key provided.\n");
        return -1;
    }

    const char* current_key = state->api_keys[state->next_key_index];
    const char* current_origin = state->origins[state->next_key_index];
    state->next_key_index = (state->next_key_index + 1) % state->num_api_keys;
    
    // Construct the full API URL from the model name and endpoint.
    char full_api_url[256];
    snprintf(full_api_url, sizeof(full_api_url), API_URL_FORMAT, state->host, state->model_name, endpoint);

    // Prepare the authentication and origin headers.
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", current_key);

    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Content-Encoding: gzip");
    headers = curl_slist_append(headers, auth_header);

    // The 'Origin' header is optional.
    if (strcmp(state->origins[state->next_key_index], "default") != 0) {
        char origin_header[256];
        snprintf(origin_header, sizeof(origin_header), "Origin: %s", current_origin);
        headers = curl_slist_append(headers, origin_header);
    }

    //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
    
    // Configure the cURL handle for the POST request.
    curl_easy_setopt(curl, CURLOPT_URL, full_api_url);
    if (state->proxy[0] != '\0') {
        curl_easy_setopt(curl, CURLOPT_PROXY, state->proxy);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, compressed_payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)payload_size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, callback_data);

    // Execute the request and retrieve the HTTP response code.
    long http_code = 0;
    CURLcode res = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    // If the request failed at the transport layer, return the negative cURL error code.
    if (res != CURLE_OK && http_code == 0) {
        http_code = -res;
    }

    // Clean up all allocated resources.
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    return http_code;
}

/**
 * @brief The main entry point of the application.
 * @details This function initializes the cURL library, determines whether the
 *          program is being run in interactive or non-interactive mode by
 *          checking if stdin and stdout are connected to a terminal, and then
 *          calls `generate_session` to start the main application logic. It
 *          ensures that cURL is properly cleaned up upon exit.
 * @param argc The number of command-line arguments.
 * @param argv An array of command-line argument strings.
 * @return Returns 0 on successful execution.
 */
int main(int argc, char* argv[]) {
    // Initialize the cURL library globally.
    curl_global_init(CURL_GLOBAL_ALL);

    // --- Pre-scan arguments for mode flags ---
    bool execute_flag_found = false;
    bool quiet_flag_found = false;
    for (int i = 1; i < argc; i++) {
        if (STRCASECMP(argv[i], "-e") == 0 || STRCASECMP(argv[i], "--execute") == 0) {
            execute_flag_found = true;
        }
        if (STRCASECMP(argv[i], "-q") == 0 || STRCASECMP(argv[i], "--quiet") == 0) {
            quiet_flag_found = true;
        }
    }

    // --- If in quiet mode, redirect stderr to the null device ---
    if (quiet_flag_found) {
        #ifdef _WIN32
            // On Windows, freopen_s is the safe way to redirect a standard stream
            FILE* dummy;
            if (freopen_s(&dummy, "NUL", "w", stderr) != 0) {
                // If redirection fails, we can't guarantee quietness, but proceed.
            }
        #else
            // On POSIX, freopen is the standard way
            if (freopen("/dev/null", "w", stderr) == NULL) {
                // If redirection fails, we can't guarantee quietness, but proceed.
            }
        #endif
    }


    // Differentiate between interactive and non-interactive (piped) modes.
#ifdef _WIN32
    int is_stdin_a_terminal = _isatty(_fileno(stdin));
    int is_stdout_a_terminal = _isatty(_fileno(stdout));
#else
    int is_stdin_a_terminal = isatty(fileno(stdin));
    int is_stdout_a_terminal = isatty(fileno(stdout));
#endif

    // A session is interactive only if both are terminals AND the execute flag is NOT present.
    bool is_interactive = (is_stdin_a_terminal && is_stdout_a_terminal) && !execute_flag_found;

    // Start the main session logic, passing the determined mode.
    generate_session(argc, argv, is_interactive, is_stdin_a_terminal);

    // Clean up the cURL library.
    curl_global_cleanup();
    return 0;
}