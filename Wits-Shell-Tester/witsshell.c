#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_ARGS 64
#define MAX_PATH_DIRS 64
#define MAX_TOKENS 100
#define MAX_COMMANDS 10
#define ERROR_MSG "An error has occurred\n"

char *path_dirs[MAX_PATH_DIRS];
int path_count = 0;

void init_path() {
    path_dirs[0] = strdup("/bin");
    path_count = 1;
}

void free_path() {
    for (int i = 0; i < path_count; i++) {
        free(path_dirs[i]);
    }
    path_count = 0;
}

void set_path(char **dirs, int count) {
    free_path();
    for (int i = 0; i < count; i++) {
        path_dirs[i] = strdup(dirs[i]);
    }
    path_count = count;
}

char* resolve_command(char *cmd) {
    if (strchr(cmd, '/') != NULL) {
        if (access(cmd, X_OK) == 0) {
            return strdup(cmd);
        }
    } else {
        for (int i = 0; i < path_count; i++) {
            char *full_path = malloc(strlen(path_dirs[i]) + strlen(cmd) + 2);
            sprintf(full_path, "%s/%s", path_dirs[i], cmd);
            if (access(full_path, X_OK) == 0) {
                return full_path;
            }
            free(full_path);
        }
    }
    return NULL;
}

void execute_redirection(char *file) {
    int fd = open(file, O_CREAT | O_TRUNC | O_WRONLY, 0666);
    if (fd < 0) {
        write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
        _exit(1);
    }
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    close(fd);
}

int execute_single_command(char **args, int argc, int redirect, char *redirect_file, int parallel) {
    if (argc == 0) return 0;

    if (strcmp(args[0], "exit") == 0) {
        if (argc != 1) {
            write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
            return 0;
        }
        exit(0);
    } else if (strcmp(args[0], "cd") == 0) {
        if (argc != 2) {
            write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
            return 0;
        }
        if (chdir(args[1]) != 0) {
            write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
        }
        return 0;
    } else if (strcmp(args[0], "path") == 0) {
        set_path(args + 1, argc - 1);
        return 0;
    }

    pid_t pid = fork();
    if (pid < 0) {
        write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
        return -1;
    }

    if (pid == 0) {
        if (redirect) {
            execute_redirection(redirect_file);
        }
        char *cmd_path = resolve_command(args[0]);
        if (cmd_path == NULL) {
            write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
            _exit(1);
        }
            // Debug: print arguments passed to execv
            fprintf(stderr, "execv args: ");
            for (int i = 0; args[i] != NULL; i++) {
                fprintf(stderr, "'%s' ", args[i]);
            }
            fprintf(stderr, "\n");
        execv(cmd_path, args);
        write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
        _exit(1);
    } else {
        return pid;
    }
}

int tokenize(char *line, char ***tokens) {
    int token_count = 0;
    *tokens = malloc(MAX_TOKENS * sizeof(char *));
    char *p = line;
    while (*p) {
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p == '\0') break;

        if (*p == '&' || *p == '>') {
            (*tokens)[token_count] = malloc(2);
            (*tokens)[token_count][0] = *p;
            (*tokens)[token_count][1] = '\0';
            token_count++;
            p++;
        } else {
            char *start = p;
            while (*p != ' ' && *p != '\t' && *p != '&' && *p != '>' && *p != '\0') {
                p++;
            }
            int len = p - start;
            (*tokens)[token_count] = malloc(len + 1);
            strncpy((*tokens)[token_count], start, len);
            (*tokens)[token_count][len] = '\0';
            token_count++;
        }
    }
    return token_count;
}

void free_tokens(char **tokens, int token_count) {
    for (int i = 0; i < token_count; i++) {
        free(tokens[i]);
    }
    free(tokens);
}

int split_commands(char **tokens, int token_count, char ***commands[], int *command_count) {
    *command_count = 0;
    *commands = malloc(MAX_COMMANDS * sizeof(char **));
    int start = 0;
    for (int i = 0; i < token_count; i++) {
        if (strcmp(tokens[i], "&") == 0) {
            int length = i - start;
            if (length > 0) {
                (*commands)[*command_count] = malloc((length + 1) * sizeof(char *));
                for (int j = 0; j < length; j++) {
                    (*commands)[*command_count][j] = tokens[start + j];
                }
                (*commands)[*command_count][length] = NULL;
                (*command_count)++;
            }
            start = i + 1;
        }
    }
    int length = token_count - start;
    if (length > 0) {
        (*commands)[*command_count] = malloc((length + 1) * sizeof(char *));
        for (int j = 0; j < length; j++) {
            (*commands)[*command_count][j] = tokens[start + j];
        }
        (*commands)[*command_count][length] = NULL;
        (*command_count)++;
    }
    return 0;
}

void parse_and_execute(char *line) {
    char **tokens;
    int token_count = tokenize(line, &tokens);
    if (token_count == 0) {
        free_tokens(tokens, token_count);
        return;
    }

    char ***commands;
    int command_count;
    split_commands(tokens, token_count, &commands, &command_count);

    int pids[MAX_COMMANDS];
    int pid_count = 0;

    for (int i = 0; i < command_count; i++) {
        char **command = commands[i];
        int argc = 0;
        while (command[argc] != NULL) argc++;

        int redirect_index = -1;
        for (int j = 0; j < argc; j++) {
            if (strcmp(command[j], ">") == 0) {
                redirect_index = j;
                break;
            }
        }

        char *redirect_file = NULL;
        int redirect = 0;
        if (redirect_index != -1) {
            if (redirect_index == argc - 1) {
                write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
                continue;
            }
            if (argc - redirect_index > 2) {
                write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
                continue;
            }
            redirect_file = command[redirect_index + 1];
            redirect = 1;
            command[redirect_index] = NULL;
            argc = redirect_index;
        }
            // If no command before redirection, print error
            if (argc == 0) {
                write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
                continue;
            }

        int pid = execute_single_command(command, argc, redirect, redirect_file, command_count > 1);
        if (pid > 0) {
            pids[pid_count++] = pid;
        }
    }

    for (int i = 0; i < pid_count; i++) {
        waitpid(pids[i], NULL, 0);
    }

    for (int i = 0; i < command_count; i++) {
        free(commands[i]);
    }
    free(commands);
    free_tokens(tokens, token_count);
}

int main(int argc, char *argv[]) {
    init_path();
    FILE *input = stdin;
    int interactive = 1;

    if (argc > 2) {
        write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
        exit(1);
    } else if (argc == 2) {
        input = fopen(argv[1], "r");
        if (input == NULL) {
            write(STDERR_FILENO, ERROR_MSG, strlen(ERROR_MSG));
            exit(1);
        }
        interactive = 0;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t read;

    while (1) {
        if (interactive) {
            printf("witsshell> ");
            fflush(stdout);
        }
        read = getline(&line, &len, input);
        if (read == -1) {
            break;
        }
        size_t length = strlen(line);
        if (length > 0 && line[length - 1] == '\n') {
            line[length - 1] = '\0';
        }
        parse_and_execute(line);
    }

    free(line);
    if (input != stdin) fclose(input);
    free_path();
    return 0;
}