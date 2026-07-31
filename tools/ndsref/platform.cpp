/* Minimal melonDS Platform implementation for the headless reference runner.
 *
 * melonDS's core leaves these for the frontend to supply. Only the file and
 * threading calls actually matter here; the rest exist so the core links, and
 * are stubs because nothing in a headless single-threaded run reaches them.
 */
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "Platform.h"

namespace melonDS::Platform {

struct FileHandle { FILE *f; };

struct Thread { std::thread t; };
struct Semaphore { std::mutex m; std::condition_variable cv; int n = 0; };
struct Mutex { std::mutex m; };

void SignalStop(StopReason, void *) {}

std::string InstanceFileSuffix() { return ""; }

std::string GetLocalFilePath(const std::string &f) { return f; }

static const char *mode_string(FileMode mode)
{
    bool read = (mode & FileMode::Read) != FileMode::None;
    bool write = (mode & FileMode::Write) != FileMode::None;
    bool append = (mode & FileMode::Append) != FileMode::None;
    bool preserve = (mode & FileMode::Preserve) != FileMode::None;
    bool text = (mode & FileMode::Text) != FileMode::None;

    if (append) return text ? "a" : "ab";
    if (write && read) return preserve ? (text ? "r+" : "r+b")
                                       : (text ? "w+" : "w+b");
    if (write) return text ? "w" : "wb";
    return text ? "r" : "rb";
}

FileHandle *OpenFile(const std::string &path, FileMode mode)
{
    FILE *f = fopen(path.c_str(), mode_string(mode));

    if (!f) return nullptr;
    return new FileHandle{f};
}

FileHandle *OpenLocalFile(const std::string &path, FileMode mode)
{
    return OpenFile(path, mode);
}

bool FileExists(const std::string &name)
{
    FILE *f = fopen(name.c_str(), "rb");

    if (!f) return false;
    fclose(f);
    return true;
}

bool LocalFileExists(const std::string &name) { return FileExists(name); }

bool CheckFileWritable(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "ab");

    if (!f) return false;
    fclose(f);
    return true;
}

bool CheckLocalFileWritable(const std::string &path)
{
    return CheckFileWritable(path);
}

bool CloseFile(FileHandle *file)
{
    if (!file) return false;
    bool ok = fclose(file->f) == 0;
    delete file;
    return ok;
}

bool IsEndOfFile(FileHandle *file) { return feof(file->f) != 0; }

bool FileReadLine(char *str, int count, FileHandle *file)
{
    return fgets(str, count, file->f) != nullptr;
}

u64 FilePosition(FileHandle *file) { return (u64)ftell(file->f); }

bool FileSeek(FileHandle *file, s64 offset, FileSeekOrigin origin)
{
    int o = origin == FileSeekOrigin::Start ? SEEK_SET
          : origin == FileSeekOrigin::Current ? SEEK_CUR : SEEK_END;

    return fseek(file->f, (long)offset, o) == 0;
}

void FileRewind(FileHandle *file) { rewind(file->f); }

u64 FileRead(void *data, u64 size, u64 count, FileHandle *file)
{
    return fread(data, size, count, file->f);
}

bool FileFlush(FileHandle *file) { return fflush(file->f) == 0; }

u64 FileWrite(const void *data, u64 size, u64 count, FileHandle *file)
{
    return fwrite(data, size, count, file->f);
}

u64 FileWriteFormatted(FileHandle *file, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vfprintf(file->f, fmt, ap);
    va_end(ap);
    return n < 0 ? 0 : (u64)n;
}

u64 FileLength(FileHandle *file)
{
    long pos = ftell(file->f), len;

    fseek(file->f, 0, SEEK_END);
    len = ftell(file->f);
    fseek(file->f, pos, SEEK_SET);
    return (u64)len;
}

/* Quiet by default: the core logs a lot at boot and none of it is interesting
 * unless something is wrong. NDSREF_VERBOSE=1 turns it back on. */
void Log(LogLevel level, const char *fmt, ...)
{
    static int verbose = -1;
    va_list ap;

    if (verbose < 0) {
        const char *e = getenv("NDSREF_VERBOSE");
        verbose = e && *e != '0';
    }
    if (!verbose && level < LogLevel::Warn) return;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

Thread *Thread_Create(std::function<void()> func)
{
    return new Thread{std::thread(func)};
}

void Thread_Free(Thread *thread) { delete thread; }

void Thread_Wait(Thread *thread) { thread->t.join(); }

Semaphore *Semaphore_Create() { return new Semaphore; }
void Semaphore_Free(Semaphore *s) { delete s; }

void Semaphore_Reset(Semaphore *s)
{
    std::lock_guard<std::mutex> lk(s->m);
    s->n = 0;
}

void Semaphore_Wait(Semaphore *s)
{
    std::unique_lock<std::mutex> lk(s->m);
    s->cv.wait(lk, [&] { return s->n > 0; });
    s->n--;
}

bool Semaphore_TryWait(Semaphore *s, int timeout_ms)
{
    std::unique_lock<std::mutex> lk(s->m);

    if (!s->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                        [&] { return s->n > 0; }))
        return false;
    s->n--;
    return true;
}

void Semaphore_Post(Semaphore *s, int count)
{
    std::lock_guard<std::mutex> lk(s->m);
    s->n += count;
    s->cv.notify_all();
}

Mutex *Mutex_Create() { return new Mutex; }
void Mutex_Free(Mutex *m) { delete m; }
void Mutex_Lock(Mutex *m) { m->m.lock(); }
void Mutex_Unlock(Mutex *m) { m->m.unlock(); }
bool Mutex_TryLock(Mutex *m) { return m->m.try_lock(); }

void Sleep(u64 usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

u64 GetMSCount()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(
               steady_clock::now().time_since_epoch()).count();
}

u64 GetUSCount()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(
               steady_clock::now().time_since_epoch()).count();
}

/* The runner owns the save image; see main.cpp. */
extern void SaveWritten(const u8 *data, u32 len);

void WriteNDSSave(const u8 *savedata, u32 savelen, u32, u32, void *)
{
    SaveWritten(savedata, savelen);
}

void WriteGBASave(const u8 *, u32, u32, u32, void *) {}
void WriteFirmware(const Firmware &, u32, u32, void *) {}
void WriteDateTime(int, int, int, int, int, int, void *) {}

void MP_Begin(void *) {}
void MP_End(void *) {}
int MP_SendPacket(u8 *, int, u64, void *) { return 0; }
int MP_RecvPacket(u8 *, u64 *, void *) { return 0; }
int MP_SendCmd(u8 *, int, u64, void *) { return 0; }
int MP_SendReply(u8 *, int, u64, u16, void *) { return 0; }
int MP_SendAck(u8 *, int, u64, void *) { return 0; }
int MP_RecvHostPacket(u8 *, u64 *, void *) { return 0; }
u16 MP_RecvReplies(u8 *, u64, u16, void *) { return 0; }

int Net_SendPacket(u8 *, int, void *) { return 0; }
int Net_RecvPacket(u8 *, void *) { return 0; }

void Camera_Start(int, void *) {}
void Camera_Stop(int, void *) {}
void Camera_CaptureFrame(int, u32 *, int, int, bool, void *) {}

void Mic_Start(void *) {}
void Mic_Stop(void *) {}
int Mic_ReadInput(s16 *, int, void *) { return 0; }

bool Addon_KeyDown(KeyType, void *) { return false; }
void Addon_RumbleStart(u32, void *) {}
void Addon_RumbleStop(void *) {}
float Addon_MotionQuery(MotionQueryType, void *) { return 0.0f; }

/* DSi-only AAC ucode; a DS cart never reaches it. */
AACDecoder *AAC_Init() { return nullptr; }
void AAC_DeInit(AACDecoder *) {}
bool AAC_Configure(AACDecoder *, int, int) { return false; }
bool AAC_DecodeFrame(AACDecoder *, const void *, int, void *, int)
{
    return false;
}

DynamicLibrary *DynamicLibrary_Load(const char *) { return nullptr; }
void DynamicLibrary_Unload(DynamicLibrary *) {}
void *DynamicLibrary_LoadFunction(DynamicLibrary *, const char *)
{
    return nullptr;
}

}  /* namespace melonDS::Platform */
