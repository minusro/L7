// Minimal SdFat stand-in backed by files under a host directory (simRoot).
#pragma once
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define O_RDWR   0x04
#define O_CREAT  0x10
#define O_TRUNC  0x20
#define BUILTIN_SDCARD 254

extern std::string simRoot;
extern int simFailIo;      // > 0: that many reads/writes fail (card stopped responding)
extern int simReinits;

class FsFile {
public:
  FsFile() {}
  FsFile(const FsFile &o) = delete;
  FsFile &operator=(const FsFile &o) = delete;
  FsFile(FsFile &&o) { f_ = o.f_; o.f_ = nullptr; }
  FsFile &operator=(FsFile &&o) { close(); f_ = o.f_; o.f_ = nullptr; return *this; }
  ~FsFile() { close(); }
  bool open(const char *path, int flags) {
    close();
    std::string p = simRoot + path;
    if (flags & O_TRUNC) { FILE *t = fopen(p.c_str(), "wb"); if (t) fclose(t); }
    if (flags & O_CREAT) { FILE *t = fopen(p.c_str(), "ab"); if (t) fclose(t); }
    f_ = fopen(p.c_str(), (flags & O_RDONLY) ? "rb" : "r+b");
    return f_ != nullptr;
  }
  explicit operator bool() const { return f_ != nullptr; }
  void close() { if (f_) fclose(f_); f_ = nullptr; }
  int read(void *buf, size_t n) { if (simFailIo > 0) { simFailIo--; return -1; } return (int)fread(buf, 1, n, f_); }
  size_t write(const void *buf, size_t n) { if (simFailIo > 0) { simFailIo--; return (size_t)-1; } return fwrite(buf, 1, n, f_); }
  bool seekSet(uint64_t pos) { return fseek(f_, (long)pos, SEEK_SET) == 0; }
  uint64_t fileSize() { long c = ftell(f_); fseek(f_, 0, SEEK_END); long s = ftell(f_); fseek(f_, c, SEEK_SET); return s; }
  bool preAllocate(uint64_t) { return true; }
  bool truncate(uint64_t len) { fflush(f_); return ftruncate(fileno(f_), len) == 0; }
  int available() { long c = ftell(f_); return (int)(fileSize() - c); }
  int fgets(char *buf, int n) { return ::fgets(buf, n, f_) ? (int)strlen(buf) : 0; }
  void flush() { fflush(f_); }
  bool sync() { fflush(f_); return true; }
  int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f_, fmt, ap); va_end(ap); return r; }
private:
  FILE *f_ = nullptr;
};

#define DMA_SDIO 1
#define FIFO_SDIO 0
struct SdioConfig { explicit SdioConfig(int) {} };
struct SdCard { uint8_t errorCode() { return 0; } uint32_t errorData() { return 0; } };

class SdFs {
public:
  bool begin(SdioConfig) { simReinits++; return true; }
  SdCard *card() { static SdCard c; return &c; }
  FsFile open(const char *path, int flags) { FsFile f; f.open(path, flags); return f; }
  bool mkdir(const char *path) { return ::mkdir((simRoot + path).c_str(), 0755) == 0; }
  bool remove(const char *path) { return ::remove((simRoot + path).c_str()) == 0; }
};

class SDClass {
public:
  bool begin(int) { return true; }
  SdFs sdfs;
};
extern SDClass SD;
