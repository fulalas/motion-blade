#include "sequence.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "y4m.h"

typedef struct input {
    FILE *file;
    pid_t decoder;
} input;

size_t frame_bytes(int width, int height)
{
    return (size_t)width * (size_t)height * 3 / 2;
}

static int is_y4m(const char *path)
{
    size_t length = strlen(path);
    return (length > 4 && !strcmp(path + length - 4, ".y4m")) || !strncmp(path, "/dev/", 5);
}

static input open_decoder(const char *path, int frames)
{
    char count[16];
    snprintf(count, sizeof(count), "%d", frames);
    char *const argv[] = {"ffmpeg", "-nostdin", "-v", "fatal", "-i", (char *)path, "-frames:v", count, "-pix_fmt",
                          "yuv420p", "-f", "yuv4mpegpipe", "-", NULL};
    int fd[2];
    if (pipe(fd) < 0) {
        return (input){NULL, -1};
    }
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1);
        close(fd[0]);
        close(fd[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    close(fd[1]);
    if (pid < 0) {
        close(fd[0]);
        return (input){NULL, -1};
    }
    return (input){fdopen(fd[0], "r"), pid};
}

static input open_input(const sequence_request *request)
{
    if (is_y4m(request->path)) {
        return (input){fopen(request->path, "rb"), -1};
    }
    return open_decoder(request->path, request->max_frames + request->skip);
}

static void close_input(input in)
{
    fclose(in.file);
    if (in.decoder > 0) {
        waitpid(in.decoder, NULL, 0);
    }
}

static int parse_header(sequence *s, const char *path)
{
    if (!y4m_frame_size(s->header, &s->width, &s->height)) {
        fprintf(stderr, "%s: needs 8-bit 4:2:0 with even width and height\n", path);
        return -1;
    }
    return 0;
}

static int read_frames(sequence *s, FILE *file, const sequence_request *request)
{
    size_t bytes = frame_bytes(s->width, s->height);
    s->frames = calloc((size_t)(request->max_frames > 0 ? request->max_frames : 1), sizeof(*s->frames));
    if (!s->frames) {
        return -1;
    }
    char line[256];
    for (int index = 0; s->count < request->max_frames && fgets(line, sizeof(line), file); index++) {
        uint8_t *frame = malloc(bytes);
        if (!frame || fread(frame, 1, bytes, file) != bytes) {
            free(frame);
            break;
        }
        if (index < request->skip) {
            free(frame);
            continue;
        }
        s->frames[s->count++] = frame;
    }
    return 0;
}

int sequence_read(const sequence_request *request, sequence *s)
{
    *s = (sequence){0};
    input in = open_input(request);
    if (!in.file) {
        return -1;
    }
    int result = -1;
    if (fgets(s->header, sizeof(s->header), in.file) && parse_header(s, request->path) == 0) {
        result = read_frames(s, in.file, request);
    }
    close_input(in);
    return result;
}

void sequence_free(sequence *s)
{
    for (int i = 0; i < s->count; i++) {
        free(s->frames[i]);
    }
    free(s->frames);
    *s = (sequence){0};
}
