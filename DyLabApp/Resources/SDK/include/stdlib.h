#ifndef _DYLAB_STDLIB_H
#define _DYLAB_STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t, size_t);
void *realloc(void *, size_t);
void free(void *);
void exit(int);
int abs(int);
long labs(long);
double atof(const char *);
int atoi(const char *);
long atol(const char *);
char *getenv(const char *);
#endif
