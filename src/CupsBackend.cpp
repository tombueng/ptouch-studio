// CUPS backend for Bluetooth RFCOMM ports.
//
// Needed because none of the backends shipped with CUPS work here: `serial`
// expects modem control lines an RFCOMM tty does not carry, and `file` opens the
// device non-blocking, which fails on RFCOMM.
//
// Device URI:  rfcomm:/dev/rfcomm0
//
// The ending matters most: after the last byte the printer is still busy for a
// while (transfer, tape feed, cutting). Closing the connection at that point makes
// it discard the rest — with multiple copies the last label goes missing. So we
// wait until the printer reports itself idle again.
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

// CUPS exit codes
constexpr int BackendOk = 0;
constexpr int BackendFailed = 1;
constexpr int BackendHold = 6;

constexpr int StatusLength = 32;
constexpr int OpenAttempts = 8;
constexpr int DrainTimeoutSeconds = 300;

// This backend runs as root under cupsd; its stderr goes to /var/log/cups/error_log,
// which an ordinary user cannot read. Mirror every line to a world-readable file so
// `ptouch-studio diagnostics` can fold the printer's own replies — the key evidence
// when a label prints blank or the tape just feeds — into a bug report.
// Keep this path in sync with ptouch::log::BackendLogPath in Log.h.
const char *const BackendLogPath = "/var/log/ptouch-studio-backend.log";

void logFile(const std::string &message)
{
    FILE *f = std::fopen(BackendLogPath, "a");
    if (!f)
        return;
    ::fchmod(::fileno(f), 0644);
    char when[32] = {0};
    const time_t now = ::time(nullptr);
    tm tmv{};
    ::localtime_r(&now, &tmv);
    ::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tmv);
    std::fprintf(f, "%s [%ld] %s\n", when, long(::getpid()), message.c_str());
    std::fclose(f);
}

std::string toHex(const unsigned char *data, int n)
{
    static const char *digits = "0123456789abcdef";
    std::string s;
    s.reserve(n * 3);
    for (int i = 0; i < n; ++i) {
        s += digits[data[i] >> 4];
        s += digits[data[i] & 0x0f];
        s += ' ';
    }
    return s;
}

void logInfo(const std::string &message)
{
    std::fprintf(stderr, "INFO: %s\n", message.c_str());
    std::fflush(stderr);
    logFile("INFO: " + message);
}

void logError(const std::string &message)
{
    std::fprintf(stderr, "ERROR: %s\n", message.c_str());
    std::fflush(stderr);
    logFile("ERROR: " + message);
}

double monotonicSeconds()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}

void sleepMs(int ms)
{
    timespec ts{ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, nullptr);
}

int openDevice(const std::string &device)
{
    for (int attempt = 1; attempt <= OpenAttempts; ++attempt) {
        // A blocking open is what brings up the Bluetooth connection.
        const int fd = ::open(device.c_str(), O_RDWR | O_NOCTTY);
        if (fd >= 0)
            return fd;
        const int e = errno;
        logInfo("connection attempt " + std::to_string(attempt) + " to " + device
                + ": " + std::strerror(e));
        if (e == ENOENT) {
            // No RFCOMM port bound — retrying will not change that.
            logError("port " + device + " does not exist — is ptouch-rfcomm.service running?");
            return -1;
        }
        sleepMs(3000);
    }
    return -1;
}

void configurePort(int fd)
{
    termios tio{};
    if (::tcgetattr(fd, &tio) != 0)
        return;
    tio.c_cflag |= CLOCAL | CREAD;
    tio.c_iflag = tio.c_oflag = tio.c_lflag = 0;
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 10;
    ::tcsetattr(fd, TCSANOW, &tio);
}

std::vector<char> readAll(int fd)
{
    std::vector<char> data;
    char buffer[65536];
    ssize_t n;
    while ((n = ::read(fd, buffer, sizeof(buffer))) > 0)
        data.insert(data.end(), buffer, buffer + n);
    return data;
}

bool writeAll(int fd, const char *data, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const ssize_t n = ::write(fd, data + written, size - written);
        if (n <= 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        written += size_t(n);
    }
    return true;
}

// Reads a status reply, if the printer sends one.
bool readStatus(int fd, unsigned char *out, int timeoutMs)
{
    int got = 0;
    const double deadline = monotonicSeconds() + timeoutMs / 1000.0;
    while (got < StatusLength && monotonicSeconds() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (::poll(&pfd, 1, 200) > 0) {
            const ssize_t n = ::read(fd, out + got, StatusLength - got);
            if (n > 0)
                got += int(n);
        }
    }
    return got == StatusLength;
}

// Waits until the printer has worked through the job.
//
// The printer reports back whenever a phase ends; on top of that we ask every two
// seconds. It is done once it is back in the waiting state (phase type 0) — until
// then the connection must stay up.
void waitUntilIdle(int fd, int expectedLabels)
{
    const char statusRequest[] = {0x1b, 0x69, 0x53};   // ESC i S
    const double deadline = monotonicSeconds() + DrainTimeoutSeconds;
    int idleReadings = 0;
    int seenPrinting = 0;

    // Give the printer a moment to even get going.
    sleepMs(1500);

    while (monotonicSeconds() < deadline) {
        unsigned char status[StatusLength] = {0};
        if (!writeAll(fd, statusRequest, sizeof(statusRequest))) {
            logInfo("cannot query status — falling back to a fixed wait");
            sleepMs(2000 * expectedLabels);
            return;
        }
        if (readStatus(fd, status, 3000)) {
            const int errors = status[8] | status[9];
            const int statusType = status[18];   // 2 = error, 1 = finished
            const int phase = status[19];        // 1 = printing
            // The full reply, byte for byte: tape width (10), media (11), the two
            // error bytes (8/9) and the phase all live here, and a blank-feed fault
            // usually shows up as an error bit or a phase that never leaves 1.
            logFile("status: " + toHex(status, StatusLength)
                    + "(err 0x" + std::to_string(status[8]) + "/0x" + std::to_string(status[9])
                    + " type " + std::to_string(statusType) + " phase " + std::to_string(phase) + ")");
            if (errors || statusType == 2) {
                logError("printer reports an error (0x" + std::to_string(status[8])
                         + "/0x" + std::to_string(status[9]) + ")");
                return;
            }
            if (phase != 0) {
                seenPrinting++;
                idleReadings = 0;
            } else {
                idleReadings++;
                // Two quiet readings in a row: the job has gone through.
                if (idleReadings >= 2 && (seenPrinting > 0 || idleReadings >= 4)) {
                    logInfo("printer is back in waiting state");
                    return;
                }
            }
        }
        sleepMs(2000);
    }
    logInfo("timed out waiting for the printer to finish");
}

} // namespace

int main(int argc, char *argv[])
{
    // Without arguments CUPS asks the backend to list its devices.
    if (argc < 2) {
        std::printf("direct rfcomm \"Unknown\" \"Bluetooth RFCOMM (P-touch)\"\n");
        return BackendOk;
    }

    // One banner per job, so a report shows exactly which run went wrong.
    {
        std::string argline = "job start:";
        for (int i = 1; i < argc && i <= 5; ++i)
            argline += std::string(" argv[") + std::to_string(i) + "]=" + argv[i];
        logFile("========================================");
        logFile(argline);
        if (const char *u = std::getenv("DEVICE_URI"))
            logFile(std::string("DEVICE_URI=") + u);
        if (const char *p = std::getenv("FINAL_CONTENT_TYPE"))
            logFile(std::string("FINAL_CONTENT_TYPE=") + p);
    }

    const char *uri = std::getenv("DEVICE_URI");
    std::string device = uri ? uri : "";
    const size_t colon = device.find(':');
    device = colon == std::string::npos ? "/dev/rfcomm0" : device.substr(colon + 1);

    std::vector<char> data;
    if (argc > 6) {
        const int in = ::open(argv[6], O_RDONLY);
        if (in < 0) {
            logError(std::string("cannot read print file: ") + argv[6]);
            return BackendFailed;
        }
        data = readAll(in);
        ::close(in);
    } else {
        data = readAll(STDIN_FILENO);
    }

    if (data.empty()) {
        logError("empty job");
        return BackendFailed;
    }

    const int copies = argc > 4 ? std::atoi(argv[4]) : 1;

    const int fd = openDevice(device);
    if (fd < 0) {
        logError("Bluetooth connection to the printer failed — is it switched on?");
        return BackendHold;    // CUPS keeps the job instead of discarding it
    }
    configurePort(fd);

    if (!writeAll(fd, data.data(), data.size())) {
        logError("transfer aborted");
        ::close(fd);
        return BackendHold;
    }
    ::tcdrain(fd);
    logInfo(std::to_string(data.size()) + " bytes sent");

    waitUntilIdle(fd, copies > 0 ? copies : 1);
    ::close(fd);

    logInfo("job complete");
    return BackendOk;
}
