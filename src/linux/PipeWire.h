// Linux: set the PipeWire graph rate via "pw-metadata -n settings 0 clock.force-rate <Hz>".
// If the graph runs at the stream's rate, PipeWire does not resample.
#pragma once
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>

#include "../Common.h"

extern char** environ;

namespace ar {
namespace lnx {

class PipeWire {
    uint32_t forced_ = 0;

    static bool InPath(const char* exe) {
        const char* path = getenv("PATH");
        std::string p = path ? path : "/usr/local/bin:/usr/bin:/bin";
        size_t start = 0;
        while (start <= p.size()) {
            size_t end = p.find(':', start);
            if (end == std::string::npos) end = p.size();
            std::string full = p.substr(start, end - start) + "/" + exe;
            if (access(full.c_str(), X_OK) == 0) return true;
            start = end + 1;
        }
        return false;
    }

    static bool Run(std::vector<std::string> args) {
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        if (rc != 0) return false;
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return false;
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    // Runs a program and collects what it prints; gives up after timeoutMs (the program is ended then)
    static bool Capture(std::vector<std::string> args, std::string& out, int timeoutMs) {
        int fd[2];
        if (pipe(fd) != 0) return false;
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(&a[0]);
        argv.push_back(nullptr);
        posix_spawn_file_actions_t fa;
        posix_spawn_file_actions_init(&fa);
        posix_spawn_file_actions_adddup2(&fa, fd[1], 1);
        posix_spawn_file_actions_addclose(&fa, fd[0]);
        posix_spawn_file_actions_addclose(&fa, fd[1]);
        posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
        pid_t pid = 0;
        int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&fa);
        close(fd[1]);
        if (rc != 0) {
            close(fd[0]);
            return false;
        }
        bool done = false;
        int64_t end = NowMs() + timeoutMs;
        char buf[4096];
        while (!done) {
            int64_t now = NowMs();
            if (now >= end) break;
            pollfd p{fd[0], POLLIN, 0};
            int r = poll(&p, 1, (int)(end - now));
            if (r < 0 && errno == EINTR) continue;
            if (r <= 0) break;
            ssize_t n = read(fd[0], buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) done = true;  // end of output: the program has finished
            else if (out.size() < (1u << 20)) out.append(buf, (size_t)n);
        }
        close(fd[0]);
        if (!done) kill(pid, SIGKILL);
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) return false;
        return done && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

public:
    static bool Available() { return InPath("pw-metadata"); }

    uint32_t Forced() const { return forced_; }

    // The rate PipeWire is forced to right now (0 = none) - another program or a restart of PipeWire may have
    // changed it since the plugin set it. False if it could not be read.
    static bool ReadForced(uint32_t& hz) {
        std::string out;
        if (!Capture({"pw-metadata", "-n", "settings", "0", "clock.force-rate"}, out, 2000)) return false;
        hz = 0;  // not listed: not forced
        size_t k = out.find("key:'clock.force-rate'");
        if (k == std::string::npos) return true;
        size_t v = out.find("value:'", k);
        if (v != std::string::npos) hz = (uint32_t)strtoul(out.c_str() + v + 7, nullptr, 10);
        return true;
    }

    bool ForceRate(uint32_t hz) {
        bool ok = Run({"pw-metadata", "-n", "settings", "0", "clock.force-rate", std::to_string(hz)});
        if (ok) forced_ = hz;
        return ok;
    }

    // 0 = release the forced rate, PipeWire chooses again
    void Release() {
        if (!forced_) return;
        Run({"pw-metadata", "-n", "settings", "0", "clock.force-rate", "0"});
        forced_ = 0;
    }
};

}  // namespace lnx
}  // namespace ar
