#include <os/logger.h>

#include <os/android/storage_android.h>

#include <android/log.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <filesystem>
#include <mutex>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ANDROID_LOG_TAG "UnleashedRecomp"

// ---------------------------------------------------------------------------
// Persistent on-device log file
// ---------------------------------------------------------------------------
// Hangs on this port leave no crash artifact - the process simply stops making
// progress - and on some tester devices logcat is drowned out by driver debug
// spam within seconds, so it can't be relied on to capture the moment of the
// freeze. Mirror every log line, plus captured stderr (where plume/Turnip print
// Vulkan errors and GPU-fault messages), into a plain text file on external app
// storage that a tester can copy off over MTP with no root and no adb:
//   Android/data/org.libsdl.app/files/log.txt   (next to driver_import/)

static std::mutex s_logMutex;
static FILE* s_logFile = nullptr;
static bool s_logFileOpenAttempted = false;

static int GetTid()
{
    return static_cast<int>(syscall(SYS_gettid));
}

static double MonotonicSeconds()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

static const double s_startSeconds = MonotonicSeconds();

// Caller must hold s_logMutex. Opens the file lazily because external storage can
// only be resolved once SDL/JNI is up, which is later than the first log lines.
static FILE* GetLogFileLocked()
{
    if (s_logFile != nullptr || s_logFileOpenAttempted)
        return s_logFile;

    const std::filesystem::path& dir = os::android::GetExternalFilesDir();
    if (dir.empty())
        return nullptr; // not ready yet - try again on the next log line

    s_logFileOpenAttempted = true;

    // Keep the previous run's log for one generation: a tester may relaunch before
    // copying the file that captured the freeze.
    std::error_code ec;
    std::filesystem::path path = dir / "log.txt";
    if (std::filesystem::exists(path, ec))
        std::filesystem::rename(path, dir / "log_prev.txt", ec);

    s_logFile = fopen(path.c_str(), "wb");
    if (s_logFile != nullptr)
        setvbuf(s_logFile, nullptr, _IONBF, 0); // unbuffered: a hang/kill keeps the tail

    return s_logFile;
}

// Writes one "[+seconds][tTID]<tag>[func] message" line to the log file.
static void WriteLogRecord(const char* tag, const char* func, const char* msg, size_t msgLen)
{
    char prefix[80];
    int plen = snprintf(prefix, sizeof(prefix), "[%9.3f][t%d]%s",
        MonotonicSeconds() - s_startSeconds, GetTid(), tag != nullptr ? tag : "");
    if (plen < 0)
        plen = 0;
    else if (plen > (int)sizeof(prefix))
        plen = (int)sizeof(prefix);

    std::lock_guard<std::mutex> lock(s_logMutex);
    FILE* file = GetLogFileLocked();
    if (file == nullptr)
        return;

    fwrite(prefix, 1, size_t(plen), file);
    if (func != nullptr)
    {
        fputc('[', file);
        fwrite(func, 1, strlen(func), file);
        fputc(']', file);
    }
    fputc(' ', file);
    if (msg != nullptr && msgLen > 0)
        fwrite(msg, 1, msgLen, file);
    fputc('\n', file);
}

// ---------------------------------------------------------------------------
// stderr -> logcat + log file
// ---------------------------------------------------------------------------
// plume and other thirdparty code report Vulkan errors via fprintf(stderr, ...),
// which is otherwise discarded on Android. Redirect the stderr file descriptor
// through a pipe so those messages reach both logcat and log.txt.
static int s_stderrPipeReadFd = -1;

static void* StderrToLogcatThread(void*)
{
    char buffer[1024];
    ssize_t bytesRead;
    while ((bytesRead = read(s_stderrPipeReadFd, buffer, sizeof(buffer) - 1)) > 0)
    {
        if (buffer[bytesRead - 1] == '\n')
            --bytesRead;

        buffer[bytesRead] = '\0';
        __android_log_write(ANDROID_LOG_ERROR, "stderr", buffer);
        WriteLogRecord("[stderr]", nullptr, buffer, size_t(bytesRead));
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// Hang watchdog
// ---------------------------------------------------------------------------
// Present() pings Heartbeat() every frame. A dedicated thread watches that ping:
// if frames stop for HANG_THRESHOLD seconds it records a thread dump built from
// /proc/self/task/* (name + scheduler state + kernel wait channel), so a freeze
// with no adb access still tells us which thread is stuck and where - separating a
// GPU/driver hang (render thread blocked in an ioctl/fence) from a guest-side
// deadlock (a thread spinning or parked on a futex).

static std::atomic<double> s_lastHeartbeat{ 0.0 };
static std::atomic<uint64_t> s_frameCount{ 0 };
static std::atomic<bool> s_watchdogSuspended{ false };
static std::once_flag s_watchdogOnce;

static void ReadProcFileTrimmed(const char* path, char* out, size_t outSize)
{
    out[0] = '\0';
    FILE* file = fopen(path, "rb");
    if (file == nullptr)
        return;

    size_t n = fread(out, 1, outSize - 1, file);
    fclose(file);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        --n;
    out[n] = '\0';
}

static void DumpThreads()
{
    DIR* dir = opendir("/proc/self/task");
    if (dir == nullptr)
    {
        WriteLogRecord("[watchdog]", nullptr, "cannot open /proc/self/task", 27);
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr)
    {
        if (entry->d_name[0] == '.')
            continue;

        char path[128];
        char comm[64];
        char wchan[128];

        snprintf(path, sizeof(path), "/proc/self/task/%s/comm", entry->d_name);
        ReadProcFileTrimmed(path, comm, sizeof(comm));

        snprintf(path, sizeof(path), "/proc/self/task/%s/wchan", entry->d_name);
        ReadProcFileTrimmed(path, wchan, sizeof(wchan));
        if (wchan[0] == '\0')
            strcpy(wchan, "0");

        // Scheduler state is the char right after the "(comm)" field in stat.
        char state = '?';
        snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
        char stat[256];
        ReadProcFileTrimmed(path, stat, sizeof(stat));
        char* lastParen = strrchr(stat, ')');
        if (lastParen != nullptr && lastParen[1] == ' ')
            state = lastParen[2];

        char line[320];
        int m = snprintf(line, sizeof(line), "tid %s [%s] state=%c wchan=%s",
            entry->d_name, comm, state, wchan);
        WriteLogRecord("[watchdog]", nullptr, line, m > 0 ? size_t(m) : 0);
    }

    closedir(dir);
}

static void* WatchdogThread(void*)
{
    constexpr double HANG_THRESHOLD = 5.0;   // no presented frame for this long => hang
    constexpr double ALIVE_INTERVAL = 5.0;   // otherwise note liveness this often
    constexpr double REDUMP_INTERVAL = 15.0; // while still hung, re-dump this often

    bool hung = false;
    double lastAliveLog = 0.0;
    double lastDump = 0.0;

    for (;;)
    {
        usleep(1000 * 1000); // 1s

        // The game is intentionally frozen (backgrounded); missing frames are not a hang.
        if (s_watchdogSuspended.load(std::memory_order_relaxed))
            continue;

        const double now = MonotonicSeconds() - s_startSeconds;
        const double last = s_lastHeartbeat.load(std::memory_order_relaxed);
        const uint64_t frames = s_frameCount.load(std::memory_order_relaxed);
        const double sinceFrame = now - last;

        if (sinceFrame > HANG_THRESHOLD)
        {
            if (!hung)
            {
                char line[192];
                int m = snprintf(line, sizeof(line),
                    "HANG DETECTED: no frame presented for %.1fs (last frame #%llu at +%.3fs). Thread dump follows:",
                    sinceFrame, (unsigned long long)frames, last);
                WriteLogRecord("[watchdog]", nullptr, line, m > 0 ? size_t(m) : 0);
                DumpThreads();
                hung = true;
                lastDump = now;
            }
            else if (now - lastDump >= REDUMP_INTERVAL)
            {
                WriteLogRecord("[watchdog]", nullptr, "still hung, thread dump follows:", 32);
                DumpThreads();
                lastDump = now;
            }
        }
        else
        {
            if (hung)
            {
                char line[96];
                int m = snprintf(line, sizeof(line), "RESUMED at frame #%llu (was stalled)",
                    (unsigned long long)frames);
                WriteLogRecord("[watchdog]", nullptr, line, m > 0 ? size_t(m) : 0);
                hung = false;
            }

            if (now - lastAliveLog >= ALIVE_INTERVAL)
            {
                char line[96];
                int m = snprintf(line, sizeof(line), "alive: frame #%llu",
                    (unsigned long long)frames);
                WriteLogRecord("[heartbeat]", nullptr, line, m > 0 ? size_t(m) : 0);
                lastAliveLog = now;
            }
        }
    }

    return nullptr;
}

// ---------------------------------------------------------------------------
// os::logger interface
// ---------------------------------------------------------------------------

void os::logger::Init()
{
    int pipeFds[2];
    if (pipe(pipeFds) != 0)
        return;

    setvbuf(stderr, nullptr, _IONBF, 0);
    dup2(pipeFds[1], STDERR_FILENO);
    close(pipeFds[1]);

    s_stderrPipeReadFd = pipeFds[0];

    pthread_t thread;
    pthread_create(&thread, nullptr, StderrToLogcatThread, nullptr);
    pthread_detach(thread);

    // Create log.txt promptly (and roll the previous one) so a tester always finds a
    // fresh file, even if this run happens to log nothing else before a freeze.
    WriteLogRecord("[logger]", nullptr, "Unleashed Recomp log started", 28);
    static constexpr char BuildVersion[] = "=== APK VERSION: roadmap-v33-mali-bcn-gpu-detect (2026-07-11) ===";
    static constexpr char BuildId[] = "ANDROID_BUILD_ID=roadmap-v33-mali-bcn-gpu-detect";
    WriteLogRecord("[build]", nullptr, BuildVersion, sizeof(BuildVersion) - 1);
    WriteLogRecord("[build]", nullptr, BuildId, sizeof(BuildId) - 1);
}

void os::logger::Log(const std::string_view str, ELogType type, const char* func)
{
    android_LogPriority priority = ANDROID_LOG_INFO;
    const char* fileTag = "";
    switch (type)
    {
    case ELogType::Warning:
        priority = ANDROID_LOG_WARN;
        fileTag = "[warn]";
        break;
    case ELogType::Error:
        priority = ANDROID_LOG_ERROR;
        fileTag = "[error]";
        break;
    default:
        break;
    }

    if (func)
    {
        __android_log_print(priority, ANDROID_LOG_TAG, "[%s] %.*s", func, (int)(str.size()), str.data());
    }
    else
    {
        __android_log_print(priority, ANDROID_LOG_TAG, "%.*s", (int)(str.size()), str.data());
    }

    WriteLogRecord(fileTag, func, str.data(), str.size());
}

void os::logger::SetWatchdogSuspended(bool suspended)
{
    if (!suspended)
    {
        // Fresh grace period so the frames missed while frozen don't read as a hang.
        s_lastHeartbeat.store(MonotonicSeconds() - s_startSeconds, std::memory_order_relaxed);
    }

    s_watchdogSuspended.store(suspended, std::memory_order_relaxed);
}

void os::logger::Heartbeat()
{
    s_lastHeartbeat.store(MonotonicSeconds() - s_startSeconds, std::memory_order_relaxed);
    s_frameCount.fetch_add(1, std::memory_order_relaxed);

    // Start the watchdog only once frames are actually being presented, so the long,
    // frame-less startup/first-load phase can't trip a false hang.
    std::call_once(s_watchdogOnce, []()
    {
        pthread_t thread;
        if (pthread_create(&thread, nullptr, WatchdogThread, nullptr) == 0)
            pthread_detach(thread);
    });
}
