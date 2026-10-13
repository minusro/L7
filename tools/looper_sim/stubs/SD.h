// Minimal SdFat stand-in backed by files under a host directory (simRoot).
#pragma once
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <string>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>

#define O_RDONLY 0x01
#define O_WRONLY 0x02
#define O_RDWR   0x04
#define O_CREAT  0x10
#define O_TRUNC  0x20
#define BUILTIN_SDCARD 254
#define FAT_TYPE_EXFAT 64

extern std::string simRoot;
extern bool simNoRun;   // preAllocate fails: no free run long enough
extern bool (*simFragmented)(unsigned long long inode);   // a file in pieces on the card (nullptr: none)
extern int simFailIo;      // > 0: that many reads/writes fail (card stopped responding)
extern int simReinits;
extern int simFsChanges;   // file creations/truncations/deletions/renames (counted by the test)
extern int simSampleTruncates;   // truncations of SAMPLES' files (each one: its modified date)
extern int simFileCreates; // files that didn't exist before
// Optional: called for every transfer so the test can let time pass (and the
// audio interrupt run) for as long as a real card would take.
extern void (*simSdLatency)(size_t bytes, bool write);

class FsFile {
public:
  FsFile() {}
  FsFile(const FsFile &o) = delete;
  FsFile &operator=(const FsFile &o) = delete;
  FsFile(FsFile &&o) { take(o); }
  FsFile &operator=(FsFile &&o) { close(); take(o); return *this; }
  ~FsFile() { close(); }
  bool open(const char *path, int flags) {
    close();
    std::string p = simRoot + path;
    path_ = path;
    {                                      // its name as stored (the case it was made with)
      const char *sl = strrchr(path, '/');
      name_ = sl ? sl + 1 : path;
      std::string parent = simRoot + std::string(path, sl ? sl - path : 0);
      if (DIR *pd = opendir(parent.c_str())) {
        while (struct dirent *e = readdir(pd))
          if (!strcasecmp(e->d_name, name_.c_str())) { name_ = e->d_name; break; }
        closedir(pd);
      }
    }
    struct stat st;
    bool existed = ::stat(p.c_str(), &st) == 0;
    if (existed && S_ISDIR(st.st_mode)) { dir_ = opendir(p.c_str()); return dir_ != nullptr; }
    if ((flags & O_CREAT) && !existed) simFileCreates++;
    if ((flags & O_TRUNC) || ((flags & O_CREAT) && !existed)) simFsChanges++;
    if (flags & O_TRUNC) { FILE *t = fopen(p.c_str(), "wb"); if (t) fclose(t); }
    if (flags & O_CREAT) { FILE *t = fopen(p.c_str(), "ab"); if (t) fclose(t); }
    f_ = fopen(p.c_str(), (flags & O_RDONLY) ? "rb" : "r+b");
    return f_ != nullptr;
  }
  explicit operator bool() const { return f_ != nullptr || dir_ != nullptr; }
  void close() { if (f_) fclose(f_); f_ = nullptr; if (dir_) closedir(dir_); dir_ = nullptr; }
  bool isDir() const { return dir_ != nullptr; }
  // Next entry of an open directory (skipping . and ..).
  bool openNext(FsFile *dir, int flags) {
    close();
    if (!dir->dir_) return false;
    while (struct dirent *e = readdir(dir->dir_)) {
      if (e->d_name[0] == '.') continue;
      std::string sub = dir->path_ + "/" + e->d_name;
      if (open(sub.c_str(), flags)) { name_ = e->d_name; return true; }
    }
    return false;
  }
  size_t getName(char *buf, size_t n) { snprintf(buf, n, "%s", name_.c_str()); return name_.size(); }
  int read(void *buf, size_t n) {
    if (simSdLatency) simSdLatency(n, false);
    if (simFailIo > 0) { simFailIo--; return -1; }
    return (int)fread(buf, 1, n, f_);
  }
  size_t write(const void *buf, size_t n) {
    if (simSdLatency) simSdLatency(n, true);
    if (simFailIo > 0) { simFailIo--; return (size_t)-1; }
    size_t r = fwrite(buf, 1, n, f_);
    fflush(f_);    // SdFat shares one cache between handles: writes are visible to all readers
    return r;
  }
  bool seekSet(uint64_t pos) { return fseek(f_, (long)pos, SEEK_SET) == 0; }
  uint64_t curPosition() { return f_ ? (uint64_t)ftell(f_) : 0; }
  uint64_t fileSize() { long c = ftell(f_); fseek(f_, 0, SEEK_END); long s = ftell(f_); fseek(f_, c, SEEK_SET); return s; }
  bool preAllocate(uint64_t) { return !simNoRun; }   // (simNoRun: the card's free space in pieces)
  bool contiguousRange(uint32_t *, uint32_t *) {    // (in one piece unless the test says this file isn't)
    struct stat st;
    return f_ && !(simFragmented && fstat(fileno(f_), &st) == 0 && simFragmented((unsigned long long)st.st_ino));
  }
  bool truncate(uint64_t len) { simFsChanges++; if (path_.find("/SAMPLES/") != std::string::npos) simSampleTruncates++; fflush(f_); return ftruncate(fileno(f_), len) == 0; }
  int available() { long c = ftell(f_); return (int)(fileSize() - c); }
  int fgets(char *buf, int n) { return ::fgets(buf, n, f_) ? (int)strlen(buf) : 0; }
  void flush() { fflush(f_); }
  bool sync() { fflush(f_); return true; }
  int printf(const char *fmt, ...) { va_list ap; va_start(ap, fmt); int r = vfprintf(f_, fmt, ap); va_end(ap); return r; }
private:
  void take(FsFile &o) { f_ = o.f_; dir_ = o.dir_; path_ = o.path_; name_ = o.name_; o.f_ = nullptr; o.dir_ = nullptr; }
  FILE *f_ = nullptr;
  DIR *dir_ = nullptr;
  std::string path_, name_;
};

#define DMA_SDIO 1
#define FIFO_SDIO 0
struct SdioConfig { explicit SdioConfig(int) {} };
struct FsDateTime { static void setCallback(void (*)(uint16_t *, uint16_t *)) {} };
struct SdCard { uint8_t errorCode() { return 0; } uint32_t errorData() { return 0; } uint32_t status() { return 0x900; } };   // (status: ready, in transfer)

class SdFs {
public:
  bool begin(SdioConfig) { simReinits++; return true; }
  SdCard *card() { static SdCard c; return &c; }
  FsFile open(const char *path, int flags) { FsFile f; f.open(path, flags); return f; }
  uint8_t fatType() { return 64; }   // (FAT_TYPE_EXFAT)
  bool mkdir(const char *path) { return ::mkdir((simRoot + path).c_str(), 0755) == 0; }
  bool exists(const char *path) { struct stat st; return ::stat((simRoot + path).c_str(), &st) == 0; }
  uint32_t freeClusterCount() { return 1u << 20; }
  uint32_t bytesPerCluster() { return 32768; }
  bool remove(const char *path) {
    struct stat st;
    if (::stat((simRoot + path).c_str(), &st) != 0) return false;   // nothing there: no change
    simFsChanges++;
    return ::remove((simRoot + path).c_str()) == 0;
  }
  bool rename(const char *from, const char *to) {
    simFsChanges++;
    return ::rename((simRoot + from).c_str(), (simRoot + to).c_str()) == 0;
  }
};

class SDClass {
public:
  bool begin(int) { return true; }
  SdFs sdfs;
  static void dateTime(uint16_t *date, uint16_t *time) { *date = *time = 0; }
};
extern SDClass SD;
