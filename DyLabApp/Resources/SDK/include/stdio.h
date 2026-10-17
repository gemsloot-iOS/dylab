#ifndef _DYLAB_STDIO_H
#define _DYLAB_STDIO_H
#include <stddef.h>
#include <stdarg.h>
typedef struct __dylab_FILE FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vsnprintf(char *, size_t, const char *, va_list);
FILE *fopen(const char *, const char *);
int fclose(FILE *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fputs(const char *, FILE *);
char *fgets(char *, int, FILE *);
#endif
