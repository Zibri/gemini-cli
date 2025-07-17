/**
 * @file prompt.c
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
 * gcc prompt.c cJSON.c -o prompt -lcurl -lz -lreadline
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
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
  #define MKDIR(path) _mkdir(path)
  #define STRCASECMP _stricmp
  #define PATH_MAX MAX_PATH
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
#define API_URL_FORMAT "https://generativelanguage.googleapis.com/v1beta/models/%s:%s"
#define GZIP_CHUNK_SIZE 16384
#define ATTACHMENT_LIMIT 1024

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
    char model_name[128];
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
    char current_session_name[128];
} AppState;

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
void get_api_key_securely(char* api_key_buffer, size_t buffer_size);
void parse_and_print_error_json(const char* error_buffer);
void load_configuration(AppState* state);
void get_config_path(char* buffer, size_t buffer_size);
void handle_attachment_from_stream(FILE* stream, const char* stream_name, const char* mime_type, AppState* state);
void get_sessions_path(char* buffer, size_t buffer_size);
bool is_session_name_safe(const char* name);
void list_sessions();
void clear_session_state(AppState* state);
void generate_non_interactive_response(int argc, char* argv[]);
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

// --- Core API and Stream Processing ---
static void process_line(char* line, MemoryStruct* mem) {
    if (strncmp(line, "data: ", 6) == 0) {
        cJSON* json_root = cJSON_Parse(line + 6);
        if (!json_root) return;
        cJSON* candidates = cJSON_GetObjectItem(json_root, "candidates");
        if (cJSON_IsArray(candidates)) {
            cJSON* candidate = cJSON_GetArrayItem(candidates, 0);
            if (candidate) {
                cJSON* content = cJSON_GetObjectItem(candidate, "content");
                if (content) {
                    cJSON* parts = cJSON_GetObjectItem(content, "parts");
                    if (cJSON_IsArray(parts)) {
                        cJSON* part = cJSON_GetArrayItem(parts, 0);
                        if (part) {
                            cJSON* text = cJSON_GetObjectItem(part, "text");
                            if (cJSON_IsString(text)) {
                                printf("%s", text->valuestring);
                                fflush(stdout);
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
                        }
                    }
                }
            }
        }
        cJSON_Delete(json_root);
    }
}

static size_t write_memory_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;
    char* ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) { fprintf(stderr, "Error: realloc failed in stream callback.\n"); return 0; }
    mem->buffer = ptr;
    memcpy(mem->buffer + mem->size, contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = '\0';
    char* line_end;
    while ((line_end = strchr(mem->buffer, '\n')) != NULL) {
        *line_end = '\0';
        process_line(mem->buffer, mem);
        size_t line_len = (line_end - mem->buffer) + 1;
        memmove(mem->buffer, line_end + 1, mem->size - line_len);
        mem->size -= line_len;
        mem->buffer[mem->size] = '\0';
    }
    return realsize;
}


// --- Main Application Logic ---
void generate_interactive_session(int argc, char* argv[]) {
    AppState state = {0};

    initialize_default_state(&state);

    load_configuration(&state);

    int first_arg_index = parse_common_options(argc, argv, &state);

    // Process remaining arguments as files or history loads
    for (int i = first_arg_index; i < argc; i++) {
        if (strlen(argv[i]) > 5 && strcmp(argv[i] + strlen(argv[i]) - 5, ".json") == 0) {
            load_history_from_file(&state, argv[i]);
        } else {
            // Treat the rest as files to attach
            handle_attachment_from_stream(NULL, argv[i], get_mime_type(argv[i]), &state);
        }
    }

    if ((strstr(state.model_name, "flash") != NULL) && (state.thinking_budget > 16384)) {
        state.thinking_budget = 16384;
    }
    
    fprintf(stderr,"Using model: %s, Temperature: %.2f, Seed: %d\n", state.model_name, state.temperature, state.seed);
    if (state.max_output_tokens > 0) { fprintf(stderr,"Max Output Tokens: %d\n", state.max_output_tokens); }
    if (state.thinking_budget > 0) {
    	 fprintf(stderr,"Thinking Budget: %d tokens\n", state.thinking_budget);
    } else {
    	 fprintf(stderr,"Thinking Budget: automatic\n");
    }
    
    fprintf(stderr,"Google grounding: %s\n", state.google_grounding?"ON":"OFF");
    fprintf(stderr,"URL Context: %s\n", state.url_context?"ON":"OFF");
	
    char* key_from_env = getenv("GEMINI_API_KEY");
    if (key_from_env) {
        strncpy(state.api_key, key_from_env, sizeof(state.api_key) - 1);
        fprintf(stderr,"API Key loaded from environment variable.\n");
    } else if (state.api_key[0] == '\0') {
        get_api_key_securely(state.api_key, sizeof(state.api_key));
    } else {
        fprintf(stderr,"API Key loaded from configuration file.\n");
    }

    char* origin_from_env = getenv("GEMINI_API_KEY_ORIGIN");
    if (origin_from_env) {
        strncpy(state.origin, origin_from_env, sizeof(state.origin) - 1);
        fprintf(stderr,"Origin loaded from environment variable: %s\n", state.origin);
    }

    if (state.num_attached_parts > 0) {
        fprintf(stderr,"Initial attachments loaded. Enter your prompt to send them.\n");
    }

    fprintf(stderr, "Interactive session started. Type '/help' for commands, '/exit' or '/quit' to end.\n");

#ifdef _WIN32
    char history_path[PATH_MAX];
    snprintf(history_path, sizeof(history_path), "%s\\gemini-cli\\history.txt", getenv("APPDATA"));
    linenoiseHistoryLoad(history_path);
#endif

    char* line;
    char prompt_buffer[16384];

    // Use an infinite loop; we will explicitly break on EOF (Ctrl+D/Ctrl+Z) or /exit.
    while (1) {
        // Generate the prompt at the start of every loop. This ensures it's
        // correct for the first run and updates after any state change.
        snprintf(prompt_buffer, sizeof(prompt_buffer), "\n(%s)>: ", state.current_session_name);

#ifdef _WIN32
        line = linenoise(prompt_buffer);
        if (line == NULL) { // EOF on Windows (Ctrl+Z, Enter)
            break;
        }
#else
        line = readline(prompt_buffer);
        if (line == NULL) { // EOF on POSIX (Ctrl+D)
            printf("\n"); // Add a newline for a clean exit, as readline does not.
            break;
        }
#endif
        // The rest of the original loop's logic follows...
        char* p = line;
        while(isspace((unsigned char)*p)) p++;
        if (*p) {
#ifndef _WIN32
            add_history(line);
#else
            linenoiseHistoryAdd(line);
            linenoiseHistorySave(history_path);
#endif
        }

        if (strcmp(p, "") == 0 && state.num_attached_parts == 0) { free(line); continue; }
        if (strcmp(p, "/exit") == 0 || strcmp(p, "/quit") == 0) { free(line); break; }

        bool is_command = false;
        if (p[0] == '/') {
            is_command = true;
            char command_buffer[64];
            sscanf(p, "%63s", command_buffer);

            char* arg_start = p + strlen(command_buffer);
            while(isspace((unsigned char)*arg_start)) arg_start++;

            if (strcmp(command_buffer, "/help") == 0) {
                fprintf(stderr,"Commands:\n"
                       "  /help                      - Show this help message.\n"
                       "  /exit, /quit               - Exit the program.\n"
                       "  /clear                     - Clear history and attachments for a new chat.\n"
                       "  /stats                     - Show session statistics (tokens, model, etc.).\n"
                       "  /system <prompt>           - Set a system prompt for the conversation.\n"
                       "  /clear_system              - Remove the system prompt.\n"
                       "  /budget <tokens>           - Set the max thinking budget for the model.\n"
                       "  /maxtokens <tokens>        - Set the max output tokens for the response.\n"
                       "  /attach <file> [prompt]    - Attach a file. Optionally add prompt on same line.\n"
                       "  /paste                     - Paste text from stdin as an attachment.\n"
                       "  /savelast <file.txt>       - Save the last model response to a text file.\n"
                       "  /save <file.json>          - (Export) Save history to a specific file path.\n"
                       "  /load <file.json>          - (Import) Load history from a specific file path.\n"
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
                        fprintf(stderr,"Usage: /session save <name>\n");
                } else if (is_session_name_safe(session_name)) {
                    char sessions_path[PATH_MAX];
                    char file_path[PATH_MAX];

                    // Build the base sessions_path
                    get_sessions_path(sessions_path, sizeof(sessions_path));

                    // Measure current lengths
                    size_t base_len = strnlen(sessions_path, sizeof(sessions_path));
                    size_t name_len = strnlen(session_name, sizeof(session_name));

                    // Compute required size: 
                    // base_len + 1 ('/') + name_len + 5 (".json") + 1 ('\0')
                    if (base_len + 1 + name_len + 5 + 1 > sizeof(file_path)) {
                        // handle error: combined path would overflow
                        fprintf(stderr, "Error: session name too long for path buffer\n");
                    } else {
                        // Safe to concatenate
                        snprintf(file_path, sizeof(file_path),
                                 "%s/%s.json", sessions_path, session_name);

                        save_history_to_file(&state, file_path);

                        // Update current_session_name (ensure null-termination)
                        strncpy(state.current_session_name,
                                session_name,
                                sizeof(state.current_session_name) - 1);
                        state.current_session_name
                            [sizeof(state.current_session_name) - 1] = '\0';
                    }
                }

                } else if (strcmp(sub_command, "load") == 0) {
                    if (session_name[0] == '\0') {
                        fprintf(stderr,"Usage: /session load <name>\n");
                    } else if (is_session_name_safe(session_name)) {
                        char sessions_path[PATH_MAX];
                        char file_path[PATH_MAX];

                        get_sessions_path(sessions_path, sizeof(sessions_path));

                        size_t base_len = strnlen(sessions_path, sizeof(sessions_path));
                        size_t name_len = strnlen(session_name, sizeof(session_name));
                        // +1 for '/', +5 for ".json", +1 for '\0'
                        if (base_len + 1 + name_len + 5 + 1 > sizeof(file_path)) {
                            fprintf(stderr, "Error: session name '%s' results in path too long\n",
                                    session_name);
                        } else {
                            snprintf(file_path, sizeof(file_path),
                                     "%s/%s.json", sessions_path, session_name);
                            load_history_from_file(&state, file_path);

                            strncpy(state.current_session_name,
                                    session_name,
                                    sizeof(state.current_session_name) - 1);
                            state.current_session_name
                                [sizeof(state.current_session_name) - 1] = '\0';
                        }
                    }

                } else if (strcmp(sub_command, "delete") == 0) {
                    if (session_name[0] == '\0') {
                        fprintf(stderr,"Usage: /session delete <name>\n");
                    } else if (is_session_name_safe(session_name)) {
                        char sessions_path[PATH_MAX];
                        char file_path[PATH_MAX];

                        get_sessions_path(sessions_path, sizeof(sessions_path));

                        size_t base_len = strnlen(sessions_path, sizeof(sessions_path));
                        size_t name_len = strnlen(session_name, sizeof(session_name));
                        // +1 for '/', +5 for ".json", +1 for '\0'
                        if (base_len + 1 + name_len + 5 + 1 > sizeof(file_path)) {
                            fprintf(stderr, "Error: session name '%s' results in path too long\n",
                                    session_name);
                        } else {
                            snprintf(file_path, sizeof(file_path),
                                     "%s/%s.json", sessions_path, session_name);
                            if (remove(file_path) == 0) {
                                fprintf(stderr,"Session '%s' deleted.\n", session_name);
                            } else {
                                perror("Error deleting session");
                            }
                        }
                    }

                } else {
                    fprintf(stderr,"Unknown session command: '%s'. Use '/help' to see options.\n", sub_command);
                }

            } else if (strcmp(command_buffer, "/stats") == 0) {
                fprintf(stderr,"--- Session Stats ---\n");
                fprintf(stderr,"Model: %s\n", state.model_name);
                fprintf(stderr,"Temperature: %.2f\n", state.temperature);
                fprintf(stderr,"Seed: %d\n", state.seed);
                fprintf(stderr,"System Prompt: %s\n", state.system_prompt ? state.system_prompt : "Not set");
                fprintf(stderr,"Messages in history: %d\n", state.history.num_contents);
                fprintf(stderr,"Pending attachments: %d\n", state.num_attached_parts);
                if (state.history.num_contents==0) continue;
                int tokens = get_token_count(&state);
                if (tokens >= 0) fprintf(stderr,"Total tokens in context: %d\n", tokens);
                else fprintf(stderr,"Could not retrieve token count.\n");
                fprintf(stderr,"---------------------\n");
            } else if (strcmp(command_buffer, "/system") == 0) {
                if (state.system_prompt) free(state.system_prompt);
                state.system_prompt = strdup(arg_start);
                if (!state.system_prompt) { fprintf(stderr, "Error: Failed to allocate memory for system prompt.\n"); }
                else { fprintf(stderr,"System prompt set to: '%s'\n", state.system_prompt); }
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
                    fprintf(stderr, "Usage: /budget <tokens>\n");
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
                    fprintf(stderr, "Usage: /maxtokens <tokens>\n");
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
                char filename[PATH_MAX] = {0};
                char* prompt_text = arg_start;

                if (*prompt_text == '\0') {
                    fprintf(stderr,"Usage: /attach <filename> [prompt...]\n");
                } else {
                    // Find the end of the filename
                    char* filename_end = prompt_text;
                    while (*filename_end != '\0' && !isspace((unsigned char)*filename_end)) {
                        filename_end++;
                    }

                    // Copy the filename
                    size_t filename_len = filename_end - prompt_text;
                    if (filename_len < sizeof(filename)) {
                        strncpy(filename, prompt_text, filename_len);
                        filename[filename_len] = '\0';

                        // The rest of the line is the prompt
                        prompt_text = filename_end;
                        while (isspace((unsigned char)*prompt_text)) {
                            prompt_text++;
                        }

                        handle_attachment_from_stream(NULL, filename, get_mime_type(filename), &state);

                        // Set the pointer 'p' to continue processing the prompt text
                        p = prompt_text;
                        is_command = false; // Ensure we process the rest of the line as a prompt
                    } else {
                        fprintf(stderr, "Error: Filename is too long.\n");
                    }
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
                            // Free the memory of the part being removed
                            free(state.attached_parts[index_to_remove].filename);
                            free(state.attached_parts[index_to_remove].mime_type);
                            free(state.attached_parts[index_to_remove].base64_data);

                            // Shift remaining elements down
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
                fprintf(stderr,"Pasting content. Press Ctrl+D (Unix) or Ctrl+Z then Enter (Windows) when done.\n");
                handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
            } else {
                fprintf(stderr,"Unknown command: %s. Type /help for a list of commands.\n", command_buffer);
            }
        } else {
            is_command = false;
        }

        if (is_command) { free(line); continue; }

        int total_parts_this_turn = state.num_attached_parts + (strlen(p) > 0 ? 1 : 0);
        if (total_parts_this_turn == 0) { free(line); continue; }

        Part* current_turn_parts = malloc(sizeof(Part) * total_parts_this_turn);
        if (!current_turn_parts) { fprintf(stderr, "Error: Failed to allocate memory for current turn parts.\n"); free(line); continue; }

        int current_part_index = 0;
        for(int i=0; i < state.num_attached_parts; i++) {
            current_turn_parts[current_part_index++] = state.attached_parts[i];
        }

        if (strlen(p) > 0) {
            // Just point to the text on the stack. add_content_to_history will copy it.
            current_turn_parts[current_part_index] = (Part){ .type = PART_TYPE_TEXT, .text = p, .mime_type = NULL, .base64_data = NULL };
        }

        add_content_to_history(&state.history, "user", current_turn_parts, total_parts_this_turn);

        // Clear pending attachments after they've been added to history
        free_pending_attachments(&state);
        // Free only the container array, as its contents were either moved or pointed to stack data.
        free(current_turn_parts);

        cJSON* root = build_request_json(&state);
        if (!root) { free(line); continue; }
        char* json_string = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        GzipResult compressed_result = gzip_compress((unsigned char*)json_string, strlen(json_string));
        free(json_string);
        if (!compressed_result.data) { free(line); continue; }

        CURL* curl = curl_easy_init();
        if (curl) {
            char full_api_url[256], auth_header[256], origin_header[256];
            snprintf(full_api_url, sizeof(full_api_url), API_URL_FORMAT, state.model_name, "streamGenerateContent?alt=sse");
            snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", state.api_key);
            snprintf(origin_header, sizeof(origin_header), "Origin: %s", state.origin);
            struct curl_slist* headers = NULL;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers, "Content-Encoding: gzip");
            headers = curl_slist_append(headers, auth_header);
            if (strcmp(state.origin, "default") != 0) { headers = curl_slist_append(headers, origin_header); }
            MemoryStruct chunk = { .buffer = malloc(1), .size = 0, .full_response = malloc(1), .full_response_size = 0 };
            if (!chunk.buffer || !chunk.full_response) {
                 fprintf(stderr, "Error: Failed to allocate memory for curl response chunk.\n");
            } else {
                chunk.buffer[0] = '\0'; chunk.full_response[0] = '\0';
                curl_easy_setopt(curl, CURLOPT_URL, full_api_url);
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, compressed_result.data);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)compressed_result.size);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_memory_callback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
                printf("\n");
                //curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
                CURLcode res = curl_easy_perform(curl);
                long http_code = 0;
                curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
                if (res == CURLE_OK && http_code == 200) {
                     printf("\n");
                     if (state.last_model_response) free(state.last_model_response);
                     state.last_model_response = chunk.full_response;
                     Part model_part = { .type = PART_TYPE_TEXT, .text = strdup(state.last_model_response), .mime_type = NULL, .base64_data = NULL };
                     add_content_to_history(&state.history, "model", &model_part, 1);
                     free(model_part.text);
                } else {
                    fprintf(stderr, "\nAPI call failed (HTTP code: %ld, Curl code: %d)\n", http_code, res);
                    parse_and_print_error_json(chunk.buffer);
                    if (state.history.num_contents > 0) {
                        state.history.num_contents--;
                        free_content(&state.history.contents[state.history.num_contents]);
                    }
                    free(chunk.full_response);
                }
            }
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            if (chunk.buffer) free(chunk.buffer);
        }
        free(compressed_result.data);
        free(line);
    }

    if(state.last_model_response) free(state.last_model_response);
    if(state.system_prompt) free(state.system_prompt);
    free_history(&state.history);
    for(int i = 0; i < state.num_attached_parts; i++) {
        free(state.attached_parts[i].mime_type);
        free(state.attached_parts[i].base64_data);
    }
    fprintf(stderr,"\nExiting session.\n");
}

// --- Helper and Utility Functions ---

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
        buffer[buffer_size - 1] = '\0'; // Ensure null-termination
    }
}

/**
 * @brief Safely reads a float value from a cJSON object.
 * @param obj The cJSON object to read from.
 * @param key The key of the float value to read.
 * @param target Pointer to the float variable to update.
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
 * @param target Pointer to the integer variable to update.
 */
static void json_read_int(const cJSON* obj, const char* key, int* target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        *target = item->valueint;
    }
}

/**
 * @brief Safely reads a boolean value from a cJSON object.
 * @param obj The cJSON object to read from.
 * @param key The key of the boolean value to read.
 * @param target Pointer to the bool variable to update.
 */
static void json_read_bool(const cJSON* obj, const char* key, bool* target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsBool(item)) {
        *target = cJSON_IsTrue(item);
    } else if (cJSON_IsNumber(item)) { // For compatibility with 0/1
        *target = (item->valueint != 0);
    }
}

/**
 * @brief Safely reads a string from a cJSON object and allocates new memory for it.
 * @param obj The cJSON object to read from.
 * @param key The key of the string value to read.
 * @param target Pointer to the char pointer that will hold the new string.
 */
static void json_read_strdup(const cJSON* obj, const char* key, char** target) {
    const cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && (item->valuestring != NULL)) {
        if (*target) {
            free(*target);
        }
        *target = strdup(item->valuestring);
    }
}

/**
 * @brief Parses common command-line options and updates the application state.
 * @param argc The argument count from main().
 * @param argv The argument vector from main().
 * @param state A pointer to the AppState struct to be updated.
 * @return The index of the first argument that was not a recognized option.
 */
int parse_common_options(int argc, char* argv[], AppState* state) {
    int i;
    for (i = 1; i < argc; i++) {
        if ((STRCASECMP(argv[i], "-m") == 0 || STRCASECMP(argv[i], "--model") == 0) && (i + 1 < argc)) {
            strncpy(state->model_name, argv[i + 1], sizeof(state->model_name) - 1);
            state->model_name[sizeof(state->model_name) - 1] = '\0';
            i++;
        } else if ((STRCASECMP(argv[i], "-t") == 0 || STRCASECMP(argv[i], "--temp") == 0) && (i + 1 < argc)) {
            state->temperature = atof(argv[i + 1]);
            i++;
        } else if ((STRCASECMP(argv[i], "-s") == 0 || STRCASECMP(argv[i], "--seed") == 0) && (i + 1 < argc)) {
            state->seed = atoi(argv[i + 1]);
            i++;
        } else if ((STRCASECMP(argv[i], "-o") == 0 || STRCASECMP(argv[i], "--max-tokens") == 0) && (i + 1 < argc)) {
            state->max_output_tokens = atoi(argv[i + 1]);
            i++;
        } else if ((STRCASECMP(argv[i], "-b") == 0 || STRCASECMP(argv[i], "--budget") == 0) && (i + 1 < argc)) {
            state->thinking_budget = atoi(argv[i + 1]);
            i++;
        } else if (STRCASECMP(argv[i], "-ng") == 0 || STRCASECMP(argv[i], "--no-grounding") == 0) {
            state->google_grounding = false;
        } else if (STRCASECMP(argv[i], "-nu") == 0 || STRCASECMP(argv[i], "--no-url-context") == 0) {
            state->url_context = false;
        } else if ((STRCASECMP(argv[i], "-h") == 0 || STRCASECMP(argv[i], "--help") == 0)) {
            print_usage(argv[0]);
            // Returning a special value or setting a flag might be cleaner,
            // but for this purpose, exiting directly is what the original code did.
            exit(0);
        } else {
            // This is not a recognized option, so stop parsing and return its index.
            return i;
        }
    }
    return i; // Return the index after all arguments have been processed.
}
/**
 * @brief Prints the command-line usage instructions and exits.
 */
void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [options] [prompt or files...]\n\n", prog_name);
    fprintf(stderr, "A portable, feature-rich command-line client for the Google Gemini API.\n\n");
    fprintf(stderr, "The client operates in two modes:\n");
    fprintf(stderr, "  - Interactive Mode: (Default) A full chat session with history and commands.\n");
    fprintf(stderr, "  - Non-Interactive Mode: Engaged if input is piped.\n\n");
//    fprintf(stderr, "  - Non-Interactive Mode: Engaged if input is piped or a prompt is given as an argument.\n\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -m, --model <name>        Specify the model name (e.g., gemini-2.5-pro).\n");
    fprintf(stderr, "  -t, --temp <float>        Set the generation temperature (0.0 to 1.0).\n");
    fprintf(stderr, "  -s, --seed <int>          Set the generation seed for reproducible outputs.\n");
    fprintf(stderr, "  -o, --max-tokens <int>    Set the maximum number of tokens in the response.\n");
    fprintf(stderr, "  -b, --budget <int>        Set the model's max 'thinking' token budget.\n");
    fprintf(stderr, "      --help                Show this help message and exit.\n\n");
/*
    fprintf(stderr, "Non-Interactive Examples:\n");
    fprintf(stderr, "  # Quick question\n");
    fprintf(stderr, "  %s \"What is the capital of Nepal?\"\n\n", prog_name);
    fprintf(stderr, "  # Summarize a file by piping its content\n");
    fprintf(stderr, "  cat main.c | %s \"Summarize this C code.\"\n\n", prog_name);
    fprintf(stderr, "Interactive Examples:\n");
    fprintf(stderr, "  # Start a standard interactive session\n");
    fprintf(stderr, "  %s\n\n", prog_name);
    fprintf(stderr, "  # Start a session with a specific model and attach files\n");
    fprintf(stderr, "  %s -m gemini-2.5-flash-latest main.c Makefile\n\n", prog_name);
    fprintf(stderr, "For commands available within the interactive session, type '/help'.\n");
*/
}
/**
 * @brief Sets the application state to its default values.
 */
void initialize_default_state(AppState* state) {
    // Start with a clean, zeroed-out state struct
    memset(state, 0, sizeof(AppState));

    // Set all default values
    strncpy(state->current_session_name, "[unsaved]", sizeof(state->current_session_name) - 1);
    strncpy(state->origin, "default", sizeof(state->origin) - 1);
    strncpy(state->model_name, DEFAULT_MODEL_NAME, sizeof(state->model_name) - 1);
    state->model_name[sizeof(state->model_name) - 1] = '\0';
    state->temperature = 0.75f;
    state->seed = 42;
    state->max_output_tokens = 65536;
    state->google_grounding = true;
    state->url_context = true;
    state->thinking_budget = -1;

/*
    // Conditionally set the thinking_budget based on the model name
    if (strstr(state->model_name, "flash") != NULL) {
        state->thinking_budget = 16384;
    } else {
        state->thinking_budget = 32768;
    }
*/
}

/**
 * @brief Handles a single, non-interactive request from piped stdin and/or argv.
 */
void generate_non_interactive_response(int argc, char* argv[]) {
    AppState state = {0};
    
    initialize_default_state(&state);

    load_configuration(&state);

    // --- 1. Argument and State Setup (reused from interactive mode) ---
    char prompt_buffer[16384] = {0};
    size_t prompt_len = 0;

    int first_arg_index = parse_common_options(argc, argv, &state);

    // Process remaining arguments as the prompt
    for (int i = first_arg_index; i < argc; i++) {
        size_t arg_len = strlen(argv[i]);
        if (prompt_len + arg_len + 2 < sizeof(prompt_buffer)) {
            char* p = prompt_buffer + prompt_len;
            size_t remaining = sizeof(prompt_buffer) - prompt_len;
            int written = snprintf(p, remaining, "%s ", argv[i]);
            if (written > 0 && (size_t)written < remaining) {
                prompt_len += written;
            }
        }
    }

    if ((strstr(state.model_name, "flash") != NULL) && (state.thinking_budget>16384)) {
        state.thinking_budget = 16384;
    }
    
    char* key_from_env = getenv("GEMINI_API_KEY");
    if (key_from_env) {
        strncpy(state.api_key, key_from_env, sizeof(state.api_key) - 1);
    } else if (state.api_key[0] == '\0') {
        fprintf(stderr, "Error: API Key not found. Please set GEMINI_API_KEY environment variable.\n");
        return;
    }

    // --- 2. Read from stdin if it's a pipe ---
    #ifdef _WIN32
        if (!_isatty(_fileno(stdin))) {
            handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
        }
    #else
        if (!isatty(fileno(stdin))) {
            handle_attachment_from_stream(stdin, "stdin", "text/plain", &state);
        }
    #endif


    // --- 3. Build and Send the Request ---
    int total_parts_this_turn = state.num_attached_parts + (prompt_len > 0 ? 1 : 0);
    if (total_parts_this_turn == 0) {
        fprintf(stderr, "Error: No prompt or piped data provided.\n");
        return;
    }

    Part* current_turn_parts = malloc(sizeof(Part) * total_parts_this_turn);
    if (!current_turn_parts) { return; }

    int current_part_index = 0;
    for(int i=0; i < state.num_attached_parts; i++) current_turn_parts[current_part_index++] = state.attached_parts[i];
    if (prompt_len > 0) { current_turn_parts[current_part_index] = (Part){ .type = PART_TYPE_TEXT, .text = prompt_buffer, .mime_type = NULL, .base64_data = NULL }; }
    add_content_to_history(&state.history, "user", current_turn_parts, total_parts_this_turn);
    free(current_turn_parts); // History function makes its own copies

    cJSON* root = build_request_json(&state);
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    GzipResult compressed_result = gzip_compress((unsigned char*)json_string, strlen(json_string));
    free(json_string);

    CURL* curl = curl_easy_init();
    if (curl) {
        char full_api_url[256], auth_header[256], origin_header[256];
        // Use the NON-STREAMING endpoint
        snprintf(full_api_url, sizeof(full_api_url), API_URL_FORMAT, state.model_name, "generateContent");
        snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", state.api_key);
        snprintf(origin_header, sizeof(origin_header), "Origin: %s", state.origin);
        struct curl_slist* headers = NULL;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, "Content-Encoding: gzip");
        headers = curl_slist_append(headers, auth_header);
        if (strcmp(state.origin, "default") != 0) { headers = curl_slist_append(headers, origin_header); }

        MemoryStruct chunk = { .buffer = malloc(1), .size = 0 };
        chunk.buffer[0] = '\0';

        curl_easy_setopt(curl, CURLOPT_URL, full_api_url);
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, compressed_result.data);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)compressed_result.size);
        // Use the simple memory callback, not the streaming one
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory_struct_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

        CURLcode res = curl_easy_perform(curl);
        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

        if (res == CURLE_OK && http_code == 200) {
            cJSON* json_resp = cJSON_Parse(chunk.buffer);
            if (json_resp) {
                cJSON* candidates = cJSON_GetObjectItem(json_resp, "candidates");
                if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
                    cJSON* content = cJSON_GetObjectItem(cJSON_GetArrayItem(candidates, 0), "content");
                    cJSON* parts = cJSON_GetObjectItem(content, "parts");
                    if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
                        cJSON* text = cJSON_GetObjectItem(cJSON_GetArrayItem(parts, 0), "text");
                        if (cJSON_IsString(text)) {
                            printf("%s\n", text->valuestring);
                        }
                    }
                }
                cJSON_Delete(json_resp);
            }
        } else {
            fprintf(stderr, "API call failed (HTTP code: %ld, Curl code: %d)\n", http_code, res);
            parse_and_print_error_json(chunk.buffer);
        }

        curl_easy_cleanup(curl);
        curl_slist_free_all(headers);
        if (chunk.buffer) free(chunk.buffer);
    }
    free(compressed_result.data);

    // --- 4. Final Cleanup ---
    if(state.last_model_response) free(state.last_model_response);
    if(state.system_prompt) free(state.system_prompt);
    free_history(&state.history);
    for(int i = 0; i < state.num_attached_parts; i++) {
        free(state.attached_parts[i].mime_type);
        free(state.attached_parts[i].base64_data);
    }
}

void clear_session_state(AppState* state) {
    free_history(&state->history);
    if (state->last_model_response) { free(state->last_model_response); state->last_model_response = NULL; }
    if (state->system_prompt) { free(state->system_prompt); state->system_prompt = NULL; }
    free_pending_attachments(state);
    strncpy(state->current_session_name, "[unsaved]", sizeof(state->current_session_name) - 1);
    fprintf(stderr,"New session started.\n");
}

/**
 * @brief Gets the path to the sessions directory, creating it if it doesn't exist.
 */
void get_sessions_path(char* buffer, size_t buffer_size) {
    const char* config_dir_name = "gemini-cli";
    const char* sessions_dir_name = "sessions";
#ifdef _WIN32
    char* base_path = getenv("APPDATA");
    if (!base_path) { buffer[0] = '\0'; return; }
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s\\%s", base_path, config_dir_name);
    MKDIR(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s\\%s\\%s", base_path, config_dir_name, sessions_dir_name);
    MKDIR(dir_path);
    snprintf(buffer, buffer_size, "%s", dir_path);
#else
    char* base_path = getenv("HOME");
    if (!base_path) { buffer[0] = '\0'; return; }
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/.config", base_path);
    MKDIR(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/.config/%s", base_path, config_dir_name);
    MKDIR(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/.config/%s/%s", base_path, config_dir_name, sessions_dir_name);
    MKDIR(dir_path);
    snprintf(buffer, buffer_size, "%s", dir_path);
#endif
}

/**
 * @brief Checks if a session name is safe (no path traversal characters).
 */
bool is_session_name_safe(const char* name) {
    if (name == NULL || name[0] == '\0') return false;
    // Disallow directory separators and dots to prevent traversal.
    if (strchr(name, '/') || strchr(name, '\\') || strchr(name, '.')) {
        fprintf(stderr, "Error: Session name cannot contain '/', '\\', or '.' characters.\n");
        return false;
    }
    return true;
}

/**
 * @brief Lists all saved sessions from the sessions directory.
 */
void list_sessions() {
    char sessions_path[PATH_MAX];
    get_sessions_path(sessions_path, sizeof(sessions_path));
    if (sessions_path[0] == '\0') {
        fprintf(stderr, "Error: Could not determine sessions directory.\n");
        return;
    }
    fprintf(stderr,"Saved Sessions:\n");

#ifdef _WIN32
    char search_path[PATH_MAX];
    snprintf(search_path, sizeof(search_path), "%s\\*.json", sessions_path);
    WIN32_FIND_DATA fd;
    HANDLE hFind = FindFirstFile(search_path, &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        fprintf(stderr,"  (No sessions found)\n");
        return;
    }
    do {
        char* dot = strrchr(fd.cFileName, '.');
        if (dot && strcmp(dot, ".json") == 0) {
            *dot = '\0'; // Temporarily remove extension
            fprintf(stderr,"  - %s\n", fd.cFileName);
        }
    } while (FindNextFile(hFind, &fd) != 0);
    FindClose(hFind);
#else
    DIR *d;
    struct dirent *dir;
    d = opendir(sessions_path);
    int count = 0;
    if (d) {
        while ((dir = readdir(d)) != NULL) {
            char* dot = strrchr(dir->d_name, '.');
            if (dot && strcmp(dot, ".json") == 0) {
                *dot = '\0'; // Temporarily remove extension
                fprintf(stderr,"  - %s\n", dir->d_name);
                *dot = '.'; // Restore it
                count++;
            }
        }
        closedir(d);
    }
    if (count == 0) {
        fprintf(stderr,"  (No sessions found)\n");
    }
#endif
}
void get_config_path(char* buffer, size_t buffer_size) {
    const char* config_dir_name = "gemini-cli";
    const char* config_file_name = "config.json";
#ifdef _WIN32
    char* base_path = getenv("APPDATA");
    if (!base_path) { buffer[0] = '\0'; return; }
    snprintf(buffer, buffer_size, "%s\\%s", base_path, config_dir_name);
    MKDIR(buffer);
    snprintf(buffer, buffer_size, "%s\\%s\\%s", base_path, config_dir_name, config_file_name);
#else
    char* base_path = getenv("HOME");
    if (!base_path) { buffer[0] = '\0'; return; }
    char dir_path[PATH_MAX];
    snprintf(dir_path, sizeof(dir_path), "%s/.config", base_path);
    MKDIR(dir_path);
    snprintf(dir_path, sizeof(dir_path), "%s/.config/%s", base_path, config_dir_name);
    MKDIR(dir_path);
    snprintf(buffer, buffer_size, "%s/.config/%s/%s", base_path, config_dir_name, config_file_name);
#endif
}

void load_configuration(AppState* state) {
    char config_path[PATH_MAX];
    get_config_path(config_path, sizeof(config_path));
    if (config_path[0] == '\0') return;

    FILE* file = fopen(config_path, "r");
    if (!file) return;

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
        return;
    }
    buffer[length] = '\0';
    fclose(file);

    cJSON* root = cJSON_Parse(buffer);
    free(buffer);

    if (!cJSON_IsObject(root)) {
        if (root) cJSON_Delete(root);
        fprintf(stderr,"Warning: Could not parse configuration file or it is not a valid JSON object.\n");
        return;
    }

    // Read values from JSON using the helper functions
    json_read_string(root, "model", state->model_name, sizeof(state->model_name));
    json_read_float(root, "temperature", &state->temperature);
    json_read_int(root, "seed", &state->seed);
    json_read_strdup(root, "system_prompt", &state->system_prompt);
    json_read_string(root, "api_key", state->api_key, sizeof(state->api_key));
    json_read_string(root, "origin", state->origin, sizeof(state->origin));
    json_read_int(root, "max_output_tokens", &state->max_output_tokens);
    json_read_int(root, "thinking_budget", &state->thinking_budget);
    json_read_bool(root, "google_grounding", &state->google_grounding);
    json_read_bool(root, "url_context", &state->url_context);

    cJSON_Delete(root);
}

void get_api_key_securely(char* api_key_buffer, size_t buffer_size) {
    fprintf(stderr,"Enter your API Key: ");
    fflush(stderr);
#ifdef _WIN32
    HANDLE hStdin = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    GetConsoleMode(hStdin, &mode);
    SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
    if (!fgets(api_key_buffer, buffer_size, stdin)) { api_key_buffer[0] = '\0'; }
    SetConsoleMode(hStdin, mode);
#else
    struct termios old_term, new_term;
    tcgetattr(STDIN_FILENO, &old_term);
    new_term = old_term;
    new_term.c_lflag &= ~ECHO;
    tcsetattr(STDIN_FILENO, TCSANOW, &new_term);
    if (!fgets(api_key_buffer, buffer_size, stdin)) { api_key_buffer[0] = '\0'; }
    tcsetattr(STDIN_FILENO, TCSANOW, &old_term);
#endif
    fprintf(stderr,"\n");
    api_key_buffer[strcspn(api_key_buffer, "\r\n")] = 0;
}

cJSON* build_request_json(AppState* state) {
    cJSON* root = cJSON_CreateObject();
    if (!root) return NULL;
    if (state->system_prompt) {
        cJSON* sys_instruction = cJSON_CreateObject();
        cJSON* sys_parts_array = cJSON_CreateArray();
        cJSON* sys_part_item = cJSON_CreateObject();
        cJSON_AddStringToObject(sys_part_item, "text", state->system_prompt);
        cJSON_AddItemToArray(sys_parts_array, sys_part_item);
        cJSON_AddItemToObject(sys_instruction, "parts", sys_parts_array);
        cJSON_AddItemToObject(root, "systemInstruction", sys_instruction);
    }
    cJSON* contents = cJSON_CreateArray();
    for (int i = 0; i < state->history.num_contents; i++) {
        cJSON* content_item = cJSON_CreateObject();
        cJSON_AddStringToObject(content_item, "role", state->history.contents[i].role);
        cJSON* parts_array = cJSON_CreateArray();
        cJSON_AddItemToObject(content_item, "parts", parts_array);
        for(int j=0; j < state->history.contents[i].num_parts; j++) {
            Part* current_part = &state->history.contents[i].parts[j];
            cJSON* part_item = cJSON_CreateObject();
            if (current_part->type == PART_TYPE_TEXT) {
                if (current_part->text) cJSON_AddStringToObject(part_item, "text", current_part->text);
            } else {
                cJSON* inline_data = cJSON_CreateObject();
                cJSON_AddStringToObject(inline_data, "mimeType", current_part->mime_type);
                cJSON_AddStringToObject(inline_data, "data", current_part->base64_data);
                cJSON_AddItemToObject(part_item, "inlineData", inline_data);
            }
            cJSON_AddItemToArray(parts_array, part_item);
        }
        cJSON_AddItemToArray(contents, content_item);
    }
    // --- Add tools configuration ---
    if (state->url_context && state->google_grounding) {
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
    cJSON_AddItemToObject(root, "contents", contents);
    cJSON* gen_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(gen_config, "temperature", state->temperature);
    cJSON_AddNumberToObject(gen_config, "maxOutputTokens", state->max_output_tokens);
    cJSON_AddNumberToObject(gen_config, "seed", state->seed);
    cJSON_AddItemToObject(root, "generationConfig", gen_config);

    cJSON* thinking_config = cJSON_CreateObject();
    cJSON_AddNumberToObject(thinking_config, "thinkingBudget", state->thinking_budget);
    //if (state->thinking_budget > 0)
    cJSON_AddItemToObject(gen_config, "thinkingConfig", thinking_config);

    return root;
}

void parse_and_print_error_json(const char* error_buffer) {
    if (!error_buffer) return;
    const char* json_start = strchr(error_buffer, '{');
    if (!json_start) { fprintf(stderr, "API Error: %s\n", error_buffer); return; }
    cJSON* root = cJSON_Parse(json_start);
    if (!root) return;
    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON* message = cJSON_GetObjectItem(error, "message");
        if (message && cJSON_IsString(message)) {
            fprintf(stderr, "API Error Message: %s\n", message->valuestring);
        }
    }
    cJSON_Delete(root);
}

static size_t write_to_memory_struct_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    MemoryStruct* mem = (MemoryStruct*)userp;
    char* ptr = realloc(mem->buffer, mem->size + realsize + 1);
    if (!ptr) { fprintf(stderr, "Error: realloc failed in token count callback.\n"); return 0; }
    mem->buffer = ptr;
    memcpy(&(mem->buffer[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->buffer[mem->size] = 0;
    return realsize;
}

int get_token_count(AppState* state) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;
    cJSON* root = build_request_json(state);
    cJSON_DeleteItemFromObject(root, "generationConfig");
    cJSON_DeleteItemFromObject(root, "tools");
    if (!root) { curl_easy_cleanup(curl); return -1; }
    char* json_string = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    GzipResult compressed_result = gzip_compress((unsigned char*)json_string, strlen(json_string));
    free(json_string);
    if (!compressed_result.data) { fprintf(stderr, "Failed to compress payload for token count.\n"); curl_easy_cleanup(curl); return -1; }
    char full_api_url[256], auth_header[256], origin_header[256];
    snprintf(full_api_url, sizeof(full_api_url), API_URL_FORMAT, state->model_name, "countTokens");
    snprintf(auth_header, sizeof(auth_header), "x-goog-api-key: %s", state->api_key);
    snprintf(origin_header, sizeof(origin_header), "Origin: %s", state->origin);
    struct curl_slist* headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Content-Encoding: gzip");
    headers = curl_slist_append(headers, auth_header);
    if (strcmp(state->origin, "default") != 0) { headers = curl_slist_append(headers, origin_header); }
    MemoryStruct chunk = { .buffer = malloc(1), .size = 0 };
    if (!chunk.buffer) { curl_easy_cleanup(curl); free(compressed_result.data); curl_slist_free_all(headers); return -1; }
    chunk.buffer[0] = '\0';
    curl_easy_setopt(curl, CURLOPT_URL, full_api_url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, compressed_result.data);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)compressed_result.size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_memory_struct_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);
    CURLcode res = curl_easy_perform(curl);
    int token_count = -1;
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (res == CURLE_OK && http_code == 200) {
        cJSON* json_resp = cJSON_Parse(chunk.buffer);
        if (json_resp) {
            cJSON* tokens = cJSON_GetObjectItem(json_resp, "totalTokens");
            if (cJSON_IsNumber(tokens)) token_count = tokens->valueint;
            cJSON_Delete(json_resp);
        }
    } else {
        fprintf(stderr, "Token count API call failed (HTTP code: %ld, Curl code: %d)\n", http_code, res);
        parse_and_print_error_json(chunk.buffer);
    }
    curl_easy_cleanup(curl);
    free(compressed_result.data);
    free(chunk.buffer);
    curl_slist_free_all(headers);
    return token_count;
}

void save_history_to_file(AppState* state, const char* filepath) {
    FILE* file = fopen(filepath, "w");
    if (!file) { perror("Failed to open file for writing"); return; }
    cJSON* root = build_request_json(state);
    if (!root) { fclose(file); return; }
    char* json_string = cJSON_Print(root);
    if (json_string) { fputs(json_string, file); free(json_string); }
    fclose(file);
    cJSON_Delete(root);
    fprintf(stderr,"Conversation history saved to %s\n", filepath);
}

void load_history_from_file(AppState* state, const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (!file) { perror("Failed to open file for reading"); return; }
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    char* buffer = malloc(length + 1);
    if (!buffer) { fclose(file); return; }
    if (fread(buffer, 1, length, file) != (size_t)length) { fclose(file); free(buffer); fprintf(stderr, "Error reading file content.\n"); return; }
    buffer[length] = '\0';
    fclose(file);
    cJSON* root = cJSON_Parse(buffer);
    free(buffer);
    if (!cJSON_IsObject(root)) { fprintf(stderr, "Error: JSON file is not a valid history object.\n"); cJSON_Delete(root); return; }

    // Clear existing history before loading
    free_history(&state->history);

    cJSON* contents = cJSON_GetObjectItem(root, "contents");
    if (!cJSON_IsArray(contents)) { cJSON_Delete(root); return; }
    cJSON* content_item;
    cJSON_ArrayForEach(content_item, contents) {
        cJSON* role_json = cJSON_GetObjectItem(content_item, "role");
        cJSON* parts_array = cJSON_GetObjectItem(content_item, "parts");
        if (!cJSON_IsString(role_json) || !cJSON_IsArray(parts_array)) continue;
        int num_parts = cJSON_GetArraySize(parts_array);
        Part* loaded_parts = malloc(sizeof(Part) * num_parts);
        if (!loaded_parts) { cJSON_Delete(root); return; }
        int part_idx = 0;
        cJSON* part_item;
        cJSON_ArrayForEach(part_item, parts_array) {
            cJSON* text_json = cJSON_GetObjectItem(part_item, "text");
            cJSON* inline_data_json = cJSON_GetObjectItem(part_item, "inlineData");
            if (cJSON_IsString(text_json)) {
                loaded_parts[part_idx] = (Part){ .type = PART_TYPE_TEXT, .text = strdup(text_json->valuestring), .mime_type = NULL, .base64_data = NULL };
            } else if (inline_data_json) {
                cJSON* mime_json = cJSON_GetObjectItem(inline_data_json, "mimeType");
                cJSON* data_json = cJSON_GetObjectItem(inline_data_json, "data");
                if (cJSON_IsString(mime_json) && cJSON_IsString(data_json)) {
                    loaded_parts[part_idx] = (Part){ .type = PART_TYPE_FILE, .text = NULL, .mime_type = strdup(mime_json->valuestring), .base64_data = strdup(data_json->valuestring) };
                }
            }
            part_idx++;
        }
        add_content_to_history(&state->history, role_json->valuestring, loaded_parts, num_parts);
        for(int i=0; i<num_parts; i++) {
            if (loaded_parts[i].text) free(loaded_parts[i].text);
            if (loaded_parts[i].mime_type) free(loaded_parts[i].mime_type);
            if (loaded_parts[i].base64_data) free(loaded_parts[i].base64_data);
        }
        free(loaded_parts);
    }

    // --- FIX START ---
    // Manually traverse the JSON to find the system prompt, as cJSON_GetObjectItemByPath might not exist.
    cJSON* sys_instruction = cJSON_GetObjectItem(root, "systemInstruction");
    if (sys_instruction && cJSON_IsObject(sys_instruction)) {
        cJSON* parts_array = cJSON_GetObjectItem(sys_instruction, "parts");
        if (parts_array && cJSON_IsArray(parts_array)) {
            cJSON* part_item = cJSON_GetArrayItem(parts_array, 0);
            if (part_item && cJSON_IsObject(part_item)) {
                cJSON* text_item = cJSON_GetObjectItem(part_item, "text");
                if (text_item && cJSON_IsString(text_item)) {
                    if (state->system_prompt) free(state->system_prompt);
                    state->system_prompt = strdup(text_item->valuestring);
                }
            }
        }
    }
    // --- FIX END ---

    cJSON_Delete(root);
    fprintf(stderr,"Conversation history loaded from %s\n", filepath);
}

void add_content_to_history(History* history, const char* role, Part* parts, int num_parts) {
    history->contents = realloc(history->contents, sizeof(Content) * (history->num_contents + 1));
    if (!history->contents) { fprintf(stderr, "Error: realloc failed when adding to history.\n"); return; }
    Content* new_content = &history->contents[history->num_contents];
    new_content->role = strdup(role);
    new_content->num_parts = num_parts;
    new_content->parts = malloc(sizeof(Part) * num_parts);
    if (!new_content->parts || !new_content->role) { fprintf(stderr, "Error: malloc failed for new history content.\n"); return; }
    for(int i=0; i<num_parts; i++) {
        new_content->parts[i].type = parts[i].type;
        if (parts[i].type == PART_TYPE_TEXT) {
            new_content->parts[i].text = strdup(parts[i].text);
            new_content->parts[i].mime_type = NULL;
            new_content->parts[i].base64_data = NULL;
            new_content->parts[i].filename = NULL;
        } else {
            new_content->parts[i].text = NULL;
            new_content->parts[i].mime_type = strdup(parts[i].mime_type);
            new_content->parts[i].base64_data = strdup(parts[i].base64_data);
            new_content->parts[i].filename = parts[i].filename ? strdup(parts[i].filename) : NULL;
        }
    }
    history->num_contents++;
}

void free_content(Content* content) {
    if (!content) return;
    free(content->role);
    if (content->parts) {
        for(int i=0; i < content->num_parts; i++) {
            if (content->parts[i].text) free(content->parts[i].text);
            if (content->parts[i].mime_type) free(content->parts[i].mime_type);
            if (content->parts[i].base64_data) free(content->parts[i].base64_data);
            if (content->parts[i].filename) free(content->parts[i].filename);
        }
        free(content->parts);
    }
}

void free_pending_attachments(AppState* state) {
    for(int i = 0; i < state->num_attached_parts; i++) {
        free(state->attached_parts[i].mime_type);
        free(state->attached_parts[i].base64_data);
        free(state->attached_parts[i].filename);
        // Set to NULL to prevent double-free issues if misused, though not strictly necessary here.
        state->attached_parts[i].mime_type = NULL;
        state->attached_parts[i].base64_data = NULL;
        state->attached_parts[i].filename = NULL;
    }
    state->num_attached_parts = 0;
}

void free_history(History* history) {
    if (!history) return;
    for (int i = 0; i < history->num_contents; i++) free_content(&history->contents[i]);
    if (history->contents) free(history->contents);
    history->contents = NULL;
    history->num_contents = 0;
}

GzipResult gzip_compress(const unsigned char* input_data, size_t input_size) {
    GzipResult result = { .data = NULL, .size = 0 };
    z_stream strm = {0};
    if (deflateInit2(&strm, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) return result;
    strm.avail_in = input_size;
    strm.next_in = (Bytef*)input_data;
    unsigned char out_chunk[GZIP_CHUNK_SIZE];
    do {
        strm.avail_out = GZIP_CHUNK_SIZE;
        strm.next_out = out_chunk;
        int ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END && ret != Z_OK) { deflateEnd(&strm); free(result.data); return (GzipResult){NULL, 0}; }
        size_t have = GZIP_CHUNK_SIZE - strm.avail_out;
        if (have > 0) {
            unsigned char* new_data = realloc(result.data, result.size + have);
            if (!new_data) { deflateEnd(&strm); free(result.data); return (GzipResult){NULL, 0}; }
            result.data = new_data;
            memcpy(result.data + result.size, out_chunk, have);
            result.size += have;
        }
    } while (strm.avail_out == 0);
    deflateEnd(&strm);
    return result;
}

bool is_path_safe(const char* path) {
    if (path == NULL || path[0] == '\0') return false;

    // Check for ".." components which are a classic sign of traversal attempts.
    if (strstr(path, "..")) return false;

    // Reject absolute paths
    #ifdef _WIN32
        if (path[0] == '\\' || (isalpha((unsigned char)path[0]) && path[1] == ':')) {
            return false;
        }
    #else
        if (path[0] == '/') return false;
    #endif

    return true;
}


void handle_attachment_from_stream(FILE* stream, const char* filepath, const char* mime_type, AppState* state) {
    if (state->num_attached_parts >= ATTACHMENT_LIMIT) { fprintf(stderr,"Attachment limit reached.\n"); return; }
    bool is_file = (stream == NULL);
    if (is_file) {
        if (!is_path_safe(filepath)) { fprintf(stderr, "Error: Unsafe or absolute file path specified: %s\n", filepath); return; }
        stream = fopen(filepath, "rb");
        if (!stream) { perror("Error opening file"); return; }
    }
    fseek(stream, 0, SEEK_END);
    long size = ftell(stream);
    fseek(stream, 0, SEEK_SET);
    if (size <= 0 && is_file) { fclose(stream); fprintf(stderr,"Warning: File '%s' is empty.\n", filepath); return; }

    unsigned char* buffer = NULL;
    size_t total_read = 0;

    if (is_file && size > 0) { // Known size from a regular file
        buffer = malloc(size);
        if (!buffer) { fclose(stream); fprintf(stderr, "Error: malloc failed for file buffer.\n"); return; }
        total_read = fread(buffer, 1, size, stream);
    } else { // Unknown size from stdin or an empty file
        size_t capacity = 4096;
        buffer = malloc(capacity);
        if (!buffer) {
            if (is_file) fclose(stream);
            fprintf(stderr, "Error: malloc failed for stdin buffer.\n");
            return;
        }

        // Read until EOF is reached. This loop is robust for interactive streams.
        while (1) {
            // Ensure we have space for at least one more read chunk.
            if (capacity - total_read < 1024) { // Use a smaller, fixed realloc threshold
                 capacity *= 2;
                 unsigned char* new_buffer = realloc(buffer, capacity);
                 if (!new_buffer) {
                     free(buffer);
                     if (is_file) fclose(stream);
                     fprintf(stderr, "Error: realloc failed for stdin buffer.\n");
                     return;
                 }
                 buffer = new_buffer;
            }

            // Read a chunk of data.
            size_t bytes_read = fread(buffer + total_read, 1, 1024, stream);
            total_read += bytes_read;

            // Check stream status to decide if we should break the loop.
            if (feof(stream)) {
                break; // EOF is the primary exit condition.
            }
            if (bytes_read == 0 && ferror(stream)) {
                perror("Error reading from stream");
                break; // Exit on error.
            }
        }
    }
    if (is_file) fclose(stream);

    Part* part = &state->attached_parts[state->num_attached_parts];
    part->type = PART_TYPE_FILE;
    part->text = NULL;
    part->filename = strdup(filepath);

    part->mime_type = strdup(mime_type);
    if (!part->mime_type) {
        fprintf(stderr, "Error: Failed to allocate memory for mime type.\n");
        free(buffer);
        return;
    }
    part->base64_data = base64_encode(buffer, total_read);
    if (!part->base64_data) {
        fprintf(stderr, "Error: Failed to allocate memory for base64 data.\n");
        free(part->mime_type); // Free the successfully allocated mime_type
        free(buffer);
        return;
    }

    if (!part->mime_type || !part->base64_data) {
        fprintf(stderr, "Error: Failed to allocate memory for attachment part.\n");
        if (part->mime_type) free(part->mime_type);
        if (part->base64_data) free(part->base64_data);
        free(buffer);
        return;
    }
    fprintf(stderr,"Attached %s (MIME: %s, Size: %zu bytes)\n", filepath, part->mime_type, total_read);
    (state->num_attached_parts)++;
    free(buffer);
}
const char* get_mime_type(const char* filename) {
    const char *dot = strrchr(filename, '.');
    if (!dot || dot == filename) return "text/plain";
    if (STRCASECMP(dot, ".txt") == 0) return "text/plain";
    if (STRCASECMP(dot, ".c") == 0 || STRCASECMP(dot, ".h") == 0 || STRCASECMP(dot, ".cpp") == 0 ||
        STRCASECMP(dot, ".hpp") == 0 || STRCASECMP(dot, ".py") == 0 || STRCASECMP(dot, ".js") == 0 ||
        STRCASECMP(dot, ".ts") == 0 || STRCASECMP(dot, ".java") == 0 || STRCASECMP(dot, ".cs") == 0 ||
        STRCASECMP(dot, ".go") == 0 || STRCASECMP(dot, ".rs") == 0 || STRCASECMP(dot, ".sh") == 0 ||
        STRCASECMP(dot, ".rb") == 0 || STRCASECMP(dot, ".php") == 0 || STRCASECMP(dot, ".css") == 0 ||
        STRCASECMP(dot, ".md") == 0) {
        return "text/plain";
    }
    if (STRCASECMP(dot, ".html") == 0) return "text/html";
    if (STRCASECMP(dot, ".json") == 0) return "application/json";
    if (STRCASECMP(dot, ".xml") == 0) return "application/xml";
    if (STRCASECMP(dot, ".jpg") == 0 || STRCASECMP(dot, ".jpeg") == 0) return "image/jpeg";
    if (STRCASECMP(dot, ".png") == 0) return "image/png";
    if (STRCASECMP(dot, ".gif") == 0) return "image/gif";
    if (STRCASECMP(dot, ".webp") == 0) return "image/webp";
    if (STRCASECMP(dot, ".pdf") == 0) return "application/pdf";
    return "text/plain";
}

char* base64_encode(const unsigned char* data, size_t input_length) {
    const char b64_chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t output_length = 4 * ((input_length + 2) / 3);
    char* encoded_data = malloc(output_length + 1);
    if (!encoded_data) return NULL;
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
    static const int mod_table[] = {0, 2, 1};
    for (int i = 0; i < mod_table[input_length % 3]; i++) encoded_data[output_length - 1 - i] = '=';
    encoded_data[output_length] = '\0';
    return encoded_data;
}

int main(int argc, char* argv[]) {
    curl_global_init(CURL_GLOBAL_ALL);

    // The single, most reliable check to differentiate modes:
    // Is the program's standard input connected to a user's terminal,
    // or is it a pipe/file redirect?
    #ifdef _WIN32
        int is_stdin_a_terminal = _isatty(_fileno(stdin));
    #else
        int is_stdin_a_terminal = isatty(fileno(stdin));
    #endif

    if (is_stdin_a_terminal) {
        // Standard input is the user's keyboard. Start the full interactive session.
        // The interactive session's own logic will correctly handle any arguments
        // like `-m model` or a `filename.txt` to attach.
        generate_interactive_session(argc, argv);
    } else {
        // Standard input is a pipe or a file. Run in non-interactive/scripting mode.
        generate_non_interactive_response(argc, argv);
    }

    curl_global_cleanup();
    return 0;
}