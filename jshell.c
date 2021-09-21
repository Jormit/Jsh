// cowrie.c a simple shell

// Version History:
//
// Version 0.1 - Added shell built-in commands (cd, pwd).
//             - Added execution from path and current directory.
//
// Version 0.2 - Added History stored to ~/.cowrie_history.
//             - Added history n command.
//             - Added ability to execute last n command with !n.
//
// Version 0.3 - Globbing working.
//             - History improvements.
//
// Version 0.4 - Cleanup.
//
// Version 0.5 - Redirection of output to file.
//             - Better history.
//             - More error messages.
//
// Version 0.6 - Pipes kind of working.
//
// Version 0.7 - Pipes working but not with redirection.
//             - Pipe code refactored.
//
// Version 0.8 - Redirection working with pipes.
//             - Error checking for all pipe commands.
//             - Cleanup.
//
// Version 0.9 - Check for invalid pipes.
//             - Cd with no command takes to home directory.
//             - Don't allow builtin commands with pipes.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <spawn.h>
#include <glob.h>

#define MAX_LINE_CHARS 1024
#define INTERACTIVE_PROMPT "cowrie> "
#define DEFAULT_PATH "/bin:/usr/bin"
#define WORD_SEPARATORS " \t\r\n"
#define DEFAULT_HISTORY_SHOWN 10

// Defines for the last_n_commands function.
#define EXECUTE 1
#define PRINT   2

// Define for output redirection flag.
#define NONE   0
#define STORE  1
#define APPEND 2

// These characters are always returned as single words
#define SPECIAL_CHARS "!><|"

// Action functions.
static void execute_command(char **words, char **path, char **environment);
static void do_exit(char **words);
char **glob_words(char **words, int *is_globbed, glob_t *globbed_data);
void execute_external(char **words, char **environment, char **path);

// built-in Functions.
void pwd(char **words);
void cd(char **words);

// Pipe functions.
void setup_redirect_output (char **words, int *redirect, int *pipe_file_descriptors, posix_spawn_file_actions_t *actions);
char **setup_redirect_input (char **words, int *redirect_in, int *pipe_file_descriptors, posix_spawn_file_actions_t *actions, char *in_file);
void redirect_input(char **words, int *pipe_file_descriptors_in, char *in_file);
void redirect_output(char **words, int *pipe_file_descriptors_out, int redirect);
char **next_pipe(char **words);
int num_pipes(char **words);
char **split_words(char **words);
int valid_pipe(char **words);

// Helper functions.
static int is_executable(char *pathname);
int get_full_path(char *program, char **path, char full_path[MAX_LINE_CHARS]);
char *get_file_in_home(char *filename);
int words_length(char **words);
int line_count_file(FILE *fp);
void no_redirect (char *program);

// History Functions.
void last_n_commands(int number, int mode, char **environ, char **path);
void print_history(char **words);
void execute_history(char **words, char **environment, char **path);
void store_command (char **words);

// Token functions.
static char **tokenize(char *s, char *separators, char *special_chars);
static void free_tokens(char **tokens);

int main(void) {
    //ensure stdout is line-buffered during autotesting
    setlinebuf(stdout);
    setlinebuf(stderr);

    // Environment variables are pointed to by `environ', an array of
    // strings terminated by a NULL value -- something like:
    //     { "VAR1=value", "VAR2=value", NULL }
    extern char **environ;

    // grab the `PATH' environment variable;
    // if it isn't set, use the default path defined above
    char *pathp;
    if ((pathp = getenv("PATH")) == NULL) {
        pathp = DEFAULT_PATH;
    }
    char **path = tokenize(pathp, ":", "");

    char *prompt = NULL;
    // if stdout is a terminal, print a prompt before reading a line of input
    if (isatty(1)) {
        prompt = INTERACTIVE_PROMPT;
    }

    // main loop: print prompt, read line, execute command
    while (1) {
        if (prompt) {
            fputs(prompt, stdout);
        }

        char line[MAX_LINE_CHARS];
        if (fgets(line, MAX_LINE_CHARS, stdin) == NULL) {
            break;
        }

        char **command_words = tokenize(line, WORD_SEPARATORS, SPECIAL_CHARS);
        execute_command(command_words, path, environ);
        free_tokens(command_words);
    }

    free_tokens(path);
    return 0;
}


//
// Execute a command, and wait until it finishes.
//
//  * `words': a NULL-terminated array of words from the input command line
//  * `path': a NULL-terminated array of directories to search in;
//  * `environment': a NULL-terminated array of environment variables.
//
static void execute_command(char **words, char **path, char **environment) {
    assert(words != NULL);
    assert(path != NULL);
    assert(environment != NULL);
    glob_t globbed_data;
    int is_globbed = 0;
    int is_redirect = 0;

    if (words [0] == NULL) {
        // nothing to do
        return;
    }

    char *program = NULL;

    // Checking if redirection so not to run builtin command.
    if (strrchr(words[0], '<') == NULL) {
        program = words[0];
    } else if (words_length(words) > 2) {
        program = words[2];
        is_redirect = 1;
    } 
    if (words_length(words) > 2) {
        if (strcmp(words[words_length(words) - 2], ">" ) == 0) {
            is_redirect = 1;
        }
    }
    if (num_pipes(words)) {
        is_redirect = 1;
    }


    // History commands first so they don't include current command in output.
    if (strcmp(program, "history") == 0) {
        if (is_redirect) {no_redirect (program); store_command(words);}
        else {print_history(words); store_command(words);}
        return;
    } else if (strcmp(program, "!") == 0) {
        if (is_redirect) {no_redirect (program);}
        else {execute_history(words, environment, path);}
        return;
    }

    // Now store the current command.
    store_command(words);

    // Expand out anything that needs globbing.
    words = glob_words(words, &is_globbed, &globbed_data);

    // Other built-in commands.
    if (strcmp(program, "exit") == 0) {
        if (is_redirect) {no_redirect (program);}
        else { do_exit(words); }
        return;
    } else if (strcmp(program, "cd") == 0) {
        if (is_redirect) {no_redirect (program);}
        else { cd(words); }
        return;
    } else if (strcmp(program, "pwd") == 0) {
        if (is_redirect) {no_redirect (program);}
        else { pwd(words); }
        return;
    }

    // If not builtin it must be external.
    execute_external(words, environment, path);

    // Need to free globbed strings.
    if (is_globbed) {
        globfree(&globbed_data);
    }
}

//
// Executes external programs with or without pipes.
// This also does checking if programs are invalid.
// Will print error message for invalid pipes.
//
void execute_external(char **words, char **environment, char **path) {
    if (!valid_pipe(words)) {
        fprintf(stderr, "invalid pipe\n");
        return; 
    }
    // Create in and out pipes for file i/o in case we need them.
    int pipe_file_in[2];
    int pipe_file_out[2];

    // Need to store the in file if there is input redirection.
    char in_file[MAX_LINE_CHARS];

    // Initialize an array for all the pipes between processes.
    int *pipe_array = NULL;
    int pipe_num = num_pipes(words);
    if (pipe_num) {
        pipe_array = malloc(sizeof(int) * 2 * pipe_num);
        for (int i = 0; i < pipe_num; i++) {
            pipe(&pipe_array[i * 2]);
        }
    }

    // Split words array by the pipes.
    words = split_words(words);

    // Loop through every program and create necessary pipes.
    char full_path[MAX_LINE_CHARS];
    int pipe_count = 0;
    pid_t child;
    while (pipe_count <= pipe_num) {
        int redirect_in = 0;
        int redirect_out = 0;

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);

        // If first command check if needs input from file.
        if (pipe_count == 0) {
            // Create input pipe to take from file.
            pipe(pipe_file_in);
            words = setup_redirect_input(words, &redirect_in, pipe_file_in, &actions, in_file);

        // If last command check if needs to redirect ouput to file.
        } if (pipe_count == pipe_num) {
            pipe(pipe_file_out);
            setup_redirect_output(words, &redirect_out, pipe_file_out, &actions);
        }

        // Redirect stdout to pipe.
        if (pipe_count != pipe_num) {
            posix_spawn_file_actions_addclose(&actions, pipe_array[pipe_count * 2]);
            posix_spawn_file_actions_adddup2(&actions, pipe_array[pipe_count * 2 + 1], 1);
        }

        // If not first pipe take input from pipe_in.
        if (pipe_count) {
            posix_spawn_file_actions_adddup2(&actions, pipe_array[(pipe_count - 1) * 2], 0);
        }

        // Now look for program location.
        if ((strrchr(words[0], '/') == NULL)) {
            if (!get_full_path(words[0], path, full_path)) {
                return;
            }
        } else {
            strcpy(full_path, words[0]);
        }

        // Now check if the file is executable.
        // If so we can execute it and wait until it is done.
        if (!is_executable(full_path)) {
            fprintf(stderr, "%s: command not found\n", full_path);
            return;
        }

        // Execute program.
        posix_spawn(&child, full_path, &actions, NULL, words, environment);

        // Handle all the file i/0.
        if (redirect_in) {
            redirect_input(words, pipe_file_in, in_file);
        }
        if (redirect_out == STORE || redirect_out == APPEND) {
            redirect_output(words, pipe_file_out, redirect_out);
        }

        // If not first command need to close write end of input pipe.
        if (pipe_count) {
            close(pipe_array[(pipe_count - 1) * 2]);
        }

        // If not last need to close read end of output pipe.
        if (pipe_count != pipe_num) {
            close(pipe_array[pipe_count * 2 + 1]);
        }

        // If there are more commands move to next command.
        if (pipe_count < pipe_num){
            words = next_pipe(words);
        }

        pipe_count++;
    }

    // Wait for last program to finish.
    int exit_status;
    if (waitpid(child, &exit_status, 0) == -1) {
        perror("waitpid");
        free(pipe_array);
        return;
    }

    free(pipe_array);
    printf("%s exit status = %d\n", full_path, WEXITSTATUS(exit_status));
    return;
}

// Handles redirection from input file into stdin of command.
void redirect_input(char **words, int *pipe_file_descriptors_in, char *in_file) {
    close(pipe_file_descriptors_in[0]);

    FILE *pipe_in = fdopen(pipe_file_descriptors_in[1], "w");
    if (pipe_in == NULL) {
      perror("fdopen");
    }
    FILE *f_in = fopen(in_file, "r");
    if (f_in == NULL) {
        perror("fopen");
    }

    char file_path[MAX_LINE_CHARS];
    char line[MAX_LINE_CHARS];

    snprintf(file_path, MAX_LINE_CHARS, "./%s", words[1]);

    while (fgets(line, MAX_LINE_CHARS, f_in)) {
      fputs(line, pipe_in); 
    }

    fclose(f_in);
    fclose(pipe_in);
}

// Handles redirection of stdout of command into file.
void redirect_output(char **words, int *pipe_file_descriptors_out, int redirect) {
    int length = words_length(words);
    if (redirect == APPEND) {
        length++;
    }
    close(pipe_file_descriptors_out[1]);

    // Open read end of pipe.
    FILE *pipe_p = fdopen(pipe_file_descriptors_out[0], "r");
    FILE *fp;

    // Get filepath.
    char file_path[MAX_LINE_CHARS];
    snprintf(file_path, MAX_LINE_CHARS, "./%s", words[length + 1]);

    // Open file with correct mode.
    if (redirect == STORE) {
        fp = fopen(file_path, "w");
    } else {
        fp = fopen(file_path, "a");
    }

    if (fp == NULL) {
        perror("fopen");
        return;
    }

    // Read from file and write into file with whichever mode is selected..
    char line[MAX_LINE_CHARS];
    while (fgets(line, MAX_LINE_CHARS, pipe_p) != NULL) {
        fputs(line, fp);
    }

    // Close up pipe and file.
    fclose(pipe_p);
    fclose(fp);
}

//
// Checks wether there is an ouput redirection.
// If there is it determines what type (stores in redirect variable)
// and sets the string with first ">" = NULL.
// eg. {"ls", "test", ">", "file", NULL} becomes {"ls", "test", NULL, "file", NULL} 
//
void setup_redirect_output (char **words, int *redirect, int *pipe_file_descriptors, posix_spawn_file_actions_t *actions) {
    // Redirect output with no append.
    int length = words_length(words);
    if (length > 2 && strcmp(words[length - 2], ">" ) == 0) {
        posix_spawn_file_actions_addclose(actions, pipe_file_descriptors[0]);
        posix_spawn_file_actions_adddup2(actions, pipe_file_descriptors[1], 1);
        *redirect = STORE; 

        // Must not include redirection in arguments passed to external program.
        words[length - 2] = NULL;

        // Check if it should be appended.
        if (length > 3 && strcmp(words[length - 3], ">" ) == 0) {
            words[length - 3] = NULL;
            *redirect = APPEND; 
        }
    } 
}

//
// Checks wether there is input redirection.
// If there is it sets redirect_in flag and returns words 
// starting after < filename.
// eg. {"<", "test", "ls", NULL} is returned as {"ls", NULL}
//
char **setup_redirect_input (char **words, int *redirect_in, int *pipe_file_descriptors, posix_spawn_file_actions_t *actions, char *in_file) {
    int length = words_length(words);
    if (length > 2 && strrchr(words[0], '<')) {
        *redirect_in = 1;

        // Setup the pipe.
        if (posix_spawn_file_actions_addclose(actions, pipe_file_descriptors[1]) != 0) {
            perror("posix_spawn_file_actions_init");
        }
        if (posix_spawn_file_actions_adddup2(actions, pipe_file_descriptors[0], 0) != 0) {
            perror("posix_spawn_file_actions_adddup2");
        }

        // Store the infile for later opening.
        strcpy(in_file, words[1]);
        return &words[2];
    }
    return words;
}

// Counts how many "|" characters there are in words.
int num_pipes(char **words) {
    int num = 0;
    for (int i = 0; i < words_length(words); i++) {
        if (strcmp(words[i], "|") == 0) {
            num++;
        }
    }
    return num;
}

//
// Splits the words array with NULL where the pipes are.
// eg. {"seq", "2", "20", "|", "grep", "2", NULL} becomes {"seq", "2", "20", NULL, "grep", "2", NULL}
//
char **split_words(char **words) {
    int length = words_length(words);
    for (int i = 0; i < length; i++) {
        if (strcmp(words[i], "|") == 0) {
            words[i] = NULL;
        }
    }
    return words;
}

// Returns a pointer to the start of arguments for next pipe.
char **next_pipe(char **words) {
    char **start = words;
    int i = 0;
    while (1) {
        if (words[i] == NULL) {
            start = &words[i + 1];
            return start;
        }
        i++;
    }
}

// built-in commands implementations.
void pwd(char **words) {
    char pathname[MAX_LINE_CHARS];
    if (getcwd(pathname, MAX_LINE_CHARS) == NULL) {
        perror("getcwd");
        return;
    }
    printf("current directory is '%s'\n", pathname);
    return;
}

// Changes directory to specified argument.
void cd(char **words) {
    if (words[1] == NULL) {
        chdir(getenv("HOME"));
    }
    else if (chdir(words[1]) != 0) {
        fprintf(stderr, "cd: %s: No such file or directory\n", words[1]);
    }
    return;
}

// Error message if try to redirect built in command.
void no_redirect (char *program) {
    fprintf(stderr, "%s: I/O redirection not permitted for builtin commands\n", program);
}

//
// Returns path of file if it is in path.
// If it is not found it will return NULL.
//
int get_full_path(char *program, char **path, char full_path[MAX_LINE_CHARS]) {
    // Next check if the file is in one of the path directories.
    int i = 0;
    while(path[i] != NULL) {
        snprintf(full_path, MAX_LINE_CHARS, "%s/%s", path[i], program);
        if (access(full_path, F_OK) != -1) {
            return 1;
        }
        i++;
    }
    // If not, we tell the user.
    fprintf(stderr, "%s: command not found\n", program);
    return 0;
}

// Stores given command to ~/.cowrie_history file.
void store_command (char **words) {
    // Need to get full path of home directory.
    char *file_path = get_file_in_home(".cowrie_history");

    // Now just open and append command with newline at the end.
    FILE *fp = fopen(file_path, "a");
    while (*words != NULL) {
        fputs(*words++, fp);
        fputc(' ', fp);
    }
    fputc('\n', fp);
    fclose(fp);
    free(file_path);
}

//
// Does operations on last n commands depending on given mode.
// If mode is EXECUTE, the nth command will be printed then executed.
// If mode is PRINT, the last nth commands will be printed.
//
void last_n_commands(int number, int mode, char **environ, char **path) {
    char *file_path = get_file_in_home(".cowrie_history");
    char line[MAX_LINE_CHARS]; 
    int line_number = 0;

    // Open file and move to avoid newline at the end.
    FILE *fp = fopen(file_path, "r");

    // Count number of lines first.
    int total_lines = line_count_file(fp);
    if (number == -1) {
        number = total_lines - 1;
    }

    int start_line = total_lines - number; 
    if (start_line < 0) {
        start_line = 0;
    }
    fseek(fp, 0, SEEK_SET);

    // Loop through lines. 
    while (line_number < total_lines) {
        fgets(line, MAX_LINE_CHARS, fp);
            if (mode == PRINT && line_number >= start_line) { 
                printf("%d: %s", line_number, line);
            } else if (mode == EXECUTE && line_number == number) {
                printf("%s", line);
                char **command_words = tokenize(line, WORD_SEPARATORS, SPECIAL_CHARS);
                execute_command(command_words, path, environ);
                free_tokens(command_words);
                return;
            }
        line_number++;
    }
    fclose(fp);
    free(file_path);
}

// Prints last int(words[1]) commands.
void print_history(char **words){
    int length = words_length(words);
    // Either print specified amount of the default (10).
    if (words[1] != NULL) {
        int number;
        if (sscanf(words[1], "%d", &number) == 0) {
            fprintf(stderr, "history: %s: numeric argument required\n", words[1]);
            store_command(words);
            return;
        } else if (length > 2) {
            fprintf(stderr, "history: too many arguments\n");
            store_command(words);
            return;
        }
        last_n_commands(number, PRINT, NULL, NULL);
    } else {
        last_n_commands(10, PRINT, NULL, NULL);
    }
}

// Executes last int(words[1]) command.
void execute_history(char **words, char **environment, char **path) {
    if (words[1] != NULL) {
        last_n_commands(atoi(words[1]), EXECUTE, environment, path);
    } else {
        last_n_commands(-1, EXECUTE, environment, path);
    }
}

// Given an array of strings this will add globbed words to it.
char **glob_words(char **words, int *is_globbed, glob_t *globbed_data) {
    // Loop through all words and check if they need globbing.
    // If so we must allocate new memory and add in the extra words.
    for (int i = 1; words[i] != NULL; i++) {
        if (strrchr (words[i], '*') != NULL || strrchr (words[i], '?') != NULL ||
                strrchr (words[i], '[') != NULL || strrchr (words[i], '~') != NULL) {

            *is_globbed = 1; // Necessary for knowing what to free at the end.

            // Run glob function and then allocate space for the globs.
            glob(words[i], GLOB_NOCHECK|GLOB_TILDE, NULL, globbed_data);
            int length = words_length(words);
            char **new_words = malloc(sizeof (char**) * (globbed_data->gl_pathc + length + 1));

            // Save all words up to the word that need to be globbed.
            for(int s = 0; s < i; s++) {
                new_words[s] = words[s];
            }

            // Save all globs.
            for(int s = i; s < globbed_data->gl_pathc + i; s++) {
                new_words[s] = globbed_data->gl_pathv[s - i];
            }

            // Save everything after the globs.
            for (int s = globbed_data->gl_pathc + i; s < globbed_data->gl_pathc + length; s++) {
                new_words[s] = words[i + s + 1 - (globbed_data->gl_pathc + i)];
            }

            // Add null terminator and set to words.
            new_words[globbed_data->gl_pathc + length] = NULL;
            words = new_words;
        }
    }
    return words;
}

// Count number of lines in the file.
int line_count_file(FILE *fp) {
    int total_lines = 0;
    int ch = fgetc(fp);
    while (ch != EOF) {
        if (ch == '\n') {
            total_lines++;
        }
        ch = fgetc(fp);
    }
    return total_lines;
}

// Calculate how long words array is.
int words_length(char **words) {
    int i = 0;
    while (words[i] != NULL) {
        i++;
    }
    return i;
}

// Given a file name in home directory, this will return it's full path.
char *get_file_in_home(char *filename) {
    char *full_path = malloc(strlen(getenv("HOME")) + strlen(filename) + 2);
    snprintf(full_path, strlen(getenv("HOME")) + strlen(filename) + 2, "%s/%s", getenv("HOME"), filename);
    return full_path;
}

// Makes sure that pipes are valid (eg. no double pipes or pipes with no command).
int valid_pipe(char **words) {
    if (strcmp(words[0], "|") == 0) {
        return 0;
    } 

    int length = words_length(words);

    int i = 0;
    int prev_pipe = 0;
    while (words[i] != NULL) {
        if (strcmp(words[i], ">") == 0 && i != length - 2 && i != length -3) {
            return 0;
        } if (strcmp(words[i], "<") == 0 && i != 0) {
            return 0;
        } else if (strcmp(words[i], "|") == 0) {
            if (prev_pipe) {
                return 0;
            } else {
                prev_pipe = 1;
            }
        } else {
            prev_pipe = 0;
        } 
        i++;
    }

    if (strcmp(words[i - 1], "|") == 0) {
        return 0;
    }
    return 1;
}

static void do_exit(char **words) {
    int exit_status = 0;

    if (words[1] != NULL) {
        if (words[2] != NULL) {
            fprintf(stderr, "exit: too many arguments\n");
        } else {
            char *endptr;
            exit_status = (int)strtol(words[1], &endptr, 10);
            if (*endptr != '\0') {
                fprintf(stderr, "exit: %s: numeric argument required\n",
                        words[1]);
            }
        }
    }

    exit(exit_status);
}

//
// Check whether this process can execute a file.
// Use this function when searching through the directories
// in the path for an executable file
//
static int is_executable(char *pathname) {
    struct stat s;
    return
        // does the file exist?
        stat(pathname, &s) == 0 &&
        // is the file a regular file?
        S_ISREG(s.st_mode) &&
        // can we execute it?
        faccessat(AT_FDCWD, pathname, X_OK, AT_EACCESS) == 0;
}

//
// Split a string 's' into pieces by any one of a set of separators.
//
// Returns an array of strings, with the last element being `NULL';
// The array itself, and the strings, are allocated with `malloc(3)';
// the provided `free_token' function can deallocate this.
//
static char **tokenize(char *s, char *separators, char *special_chars) {
    size_t n_tokens = 0;
    // malloc array guaranteed to be big enough
    char **tokens = malloc((strlen(s) + 1) * sizeof *tokens);


    while (*s != '\0') {
        // We are pointing at zero or more of any of the separators.
        // Skip leading instances of the separators.
        s += strspn(s, separators);

        // Now, `s' points at one or more characters we want to keep.
        // The number of non-separator characters is the token length.
        //
        // Trailing separators after the last token mean that, at this
        // point, we are looking at the end of the string, so:
        if (*s == '\0') {
            break;
        }

        size_t token_length = strcspn(s, separators);
        size_t token_length_without_special_chars = strcspn(s, special_chars);
        if (token_length_without_special_chars == 0) {
            token_length_without_special_chars = 1;
        }
        if (token_length_without_special_chars < token_length) {
            token_length = token_length_without_special_chars;
        }
        char *token = strndup(s, token_length);
        assert(token != NULL);
        s += token_length;

        // Add this token.
        tokens[n_tokens] = token;
        n_tokens++;
    }

    tokens[n_tokens] = NULL;
    // shrink array to correct size
    tokens = realloc(tokens, (n_tokens + 1) * sizeof *tokens);

    return tokens;
}

//
// Free an array of strings as returned by `tokenize'.
//
static void free_tokens(char **tokens) {
    for (int i = 0; tokens[i] != NULL; i++) {
        free(tokens[i]);
    }
    free(tokens);
}
