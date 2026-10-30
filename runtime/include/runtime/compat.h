#pragma once

#ifndef _WIN32
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

#define _stat64 stat
#define _S_IFDIR S_IFDIR
#define _ftelli64 ftello
#define _fseeki64 fseeko
#define _fileno fileno

inline int _chsize_s(int fd, int64_t size) { return ftruncate(fd, size); }
inline void* _aligned_malloc(size_t size, size_t align) { return aligned_alloc(align, (size + align - 1) & ~(align - 1)); }
inline void _aligned_free(void* p) { free(p); }
#endif
