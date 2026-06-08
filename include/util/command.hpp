#pragma once

#include <fcntl.h>
#include <giomm.h>
#include <spdlog/spdlog.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif
#ifdef __FreeBSD__
#include <sys/procctl.h>
#endif

#include <array>

extern std::mutex reap_mtx;
extern std::list<pid_t> reap;

namespace waybar::util::command {

constexpr int kExecFailureExitCode = 127;

struct res {
  int exit_code;
  std::string out;
};

inline std::string read(FILE* fp) {
  std::array<char, 128> buffer = {0};
  std::string output;
  while (feof(fp) == 0) {
    if (fgets(buffer.data(), 128, fp) != nullptr) {
      output += buffer.data();
    }
  }

  // Remove last newline
  if (!output.empty() && output[output.length() - 1] == '\n') {
    output.erase(output.length() - 1);
  }
  return output;
}

inline void discard(FILE* fp) {
  std::array<char, 128> buffer = {0};
  while (feof(fp) == 0) {
    (void)fgets(buffer.data(), 128, fp);
  }
}

inline int close(FILE* fp, pid_t pid) {
  int stat = -1;
  pid_t ret;

  fclose(fp);
  do {
    ret = waitpid(pid, &stat, WCONTINUED | WUNTRACED);
    if (ret == -1) {
      if (errno == EINTR) continue;
      spdlog::debug("waitpid failed: {}", strerror(errno));
      break;
    }

    if (WIFEXITED(stat)) {
      spdlog::debug("Cmd exited with code {}", WEXITSTATUS(stat));
    } else if (WIFSIGNALED(stat)) {
      spdlog::debug("Cmd killed by {}", WTERMSIG(stat));
    } else if (WIFSTOPPED(stat)) {
      spdlog::debug("Cmd stopped by {}", WSTOPSIG(stat));
    } else if (WIFCONTINUED(stat)) {
      spdlog::debug("Cmd continued");
    } else {
      break;
    }
  } while (!WIFEXITED(stat) && !WIFSIGNALED(stat));
  return stat;
}

inline int exitCodeFromStatus(int stat) {
  if (stat == -1) return -1;
  if (WIFEXITED(stat)) return WEXITSTATUS(stat);
  if (WIFSIGNALED(stat)) return 128 + WTERMSIG(stat);
  return -1;
}

inline FILE* open(const std::string& cmd, int& pid, const std::string& output_name) {
  if (cmd == "") return nullptr;
  int fd[2];
  // Open the pipe with the close-on-exec flag set, so it will not be inherited
  // by any other subprocesses launched by other threads (which could result in
  // the pipe staying open after this child dies, causing us to hang when trying
  // to read from it)
  if (pipe2(fd, O_CLOEXEC) != 0) {
    spdlog::error("Unable to pipe fd");
    return nullptr;
  }

  pid_t child_pid = fork();

  if (child_pid < 0) {
    spdlog::error("Unable to exec cmd {}, error {}", cmd.c_str(), strerror(errno));
    ::close(fd[0]);
    ::close(fd[1]);
    return nullptr;
  }

  if (!child_pid) {
    int err;
    sigset_t mask;
    sigfillset(&mask);
    // Reset sigmask
    err = pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    if (err != 0) spdlog::error("pthread_sigmask in open failed: {}", strerror(err));
    // Kill child if Waybar exits
    int deathsig = SIGTERM;
#ifdef __linux__
    if (prctl(PR_SET_PDEATHSIG, deathsig) != 0) {
      spdlog::error("prctl(PR_SET_PDEATHSIG) in open failed: {}", strerror(errno));
    }
#endif
#ifdef __FreeBSD__
    if (procctl(P_PID, 0, PROC_PDEATHSIG_CTL, reinterpret_cast<void*>(&deathsig)) == -1) {
      spdlog::error("procctl(PROC_PDEATHSIG_CTL) in open failed: {}", strerror(errno));
    }
#endif
    ::close(fd[0]);
    dup2(fd[1], 1);
    setpgid(child_pid, child_pid);
    if (!output_name.empty()) {
      setenv("WAYBAR_OUTPUT_NAME", output_name.c_str(), 1);
    }
    execlp("/bin/sh", "sh", "-c", cmd.c_str(), (char*)0);
    const int saved_errno = errno;
    spdlog::error("execlp(/bin/sh) failed in open: {}", strerror(saved_errno));
    _exit(kExecFailureExitCode);
  } else {
    ::close(fd[1]);
  }
  pid = child_pid;
  return fdopen(fd[0], "r");
}

inline struct res exec(const std::string& cmd, const std::string& output_name) {
  int pid;
  auto fp = command::open(cmd, pid, output_name);
  if (!fp) return {-1, ""};
  auto output = command::read(fp);
  auto stat = command::close(fp, pid);
  return {exitCodeFromStatus(stat), output};
}

inline struct res execNoRead(const std::string& cmd) {
  int pid;
  auto fp = command::open(cmd, pid, "");
  if (!fp) return {-1, ""};
  command::discard(fp);
  auto stat = command::close(fp, pid);
  return {exitCodeFromStatus(stat), ""};
}

inline int32_t forkExec(const std::string& cmd, const std::string& output_name) {
  if (cmd == "") return -1;

  pid_t pid = fork();

  if (pid < 0) {
    spdlog::error("Unable to exec cmd {}, error {}", cmd.c_str(), strerror(errno));
    return pid;
  }

  // Child executes the command
  if (!pid) {
    int err;
    sigset_t mask;
    sigfillset(&mask);
    // Reset sigmask
    err = pthread_sigmask(SIG_UNBLOCK, &mask, nullptr);
    if (err != 0) spdlog::error("pthread_sigmask in forkExec failed: {}", strerror(err));
    setpgid(pid, pid);
    if (!output_name.empty()) {
      setenv("WAYBAR_OUTPUT_NAME", output_name.c_str(), 1);
    }
    execl("/bin/sh", "sh", "-c", cmd.c_str(), (char*)0);
    const int saved_errno = errno;
    spdlog::error("execl(/bin/sh) failed in forkExec: {}", strerror(saved_errno));
    _exit(kExecFailureExitCode);
  } else {
    reap_mtx.lock();
    reap.push_back(pid);
    reap_mtx.unlock();
    spdlog::debug("Added child to reap list: {}", pid);
  }

  return pid;
}

inline int32_t forkExec(const std::string& cmd) { return forkExec(cmd, ""); }

}  // namespace waybar::util::command
