#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#define _GNU_SOURCE
#define __USE_GNU
#include <dlfcn.h>

static int fd = 0;
static FILE * f;

/*int open(const char *pathname, int flags)
{
        printf("called %s\n", pathname);
}*/
FILE* fopen(const char* path, const char* mode) {
    printf("called %s\n", path);
FILE* (*real_fopen)(const char*, const char*) =
dlsym(RTLD_NEXT, "fopen");
    if (strstr(path, "input.jpg")== NULL)
        return real_fopen(path, mode);
if (strstr(mode, "r"))
    return 0;
if (fd <= 0)
fd = open("/var/tmp/yagf.fifo", O_WRONLY);
f = real_fopen(path, mode);
return f;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb,
                     FILE *stream)
{
    FILE* (*real_fwrite)(const void *ptr, size_t size, size_t nmemb,
                     FILE *stream) =
dlsym(RTLD_NEXT, "fwrite");
    if (stream == f)
        write(fd, ptr, size*nmemb);
   return real_fwrite(ptr, size, nmemb, stream);

}

int fclose(FILE *fp) {
    char * endmark = "PIPETZ";
    int (* real_fclose)(FILE *fp) = dlsym(RTLD_NEXT, "fclose");
    if (f == fp)
        write(fd, endmark, 6);
    return real_fclose(fp);
}
