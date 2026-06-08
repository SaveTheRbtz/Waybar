#include "modules/sway/ipc/client.hpp"

#include <fcntl.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string_view>

namespace waybar::modules::sway {
namespace {

uint32_t readU32(std::span<const char> data, size_t offset) {
  uint32_t value{};
  std::memcpy(&value, data.data() + offset, sizeof(value));
  return value;
}

void writeU32(std::span<char> data, size_t offset, uint32_t value) {
  std::memcpy(data.data() + offset, &value, sizeof(value));
}

}  // namespace

Ipc::Ipc() {
  const std::string& socketPath = getSocketPath();
  fd_ = util::ScopedFd(open(socketPath));
  fd_event_ = util::ScopedFd(open(socketPath));
}

Ipc::~Ipc() {
  thread_.stop();

  if (fd_ > 0) {
    // To fail the IPC header
    if (write(fd_, "close-sway-ipc", 14) == -1) {
      spdlog::error("Failed to close sway IPC");
    }
  }
  if (fd_event_ > 0) {
    if (write(fd_event_, "close-sway-ipc", 14) == -1) {
      spdlog::error("Failed to close sway IPC event handler");
    }
  }
}

void Ipc::setWorker(std::function<void()>&& func) { thread_ = func; }

const std::string Ipc::getSocketPath() const {
  const char* env = getenv("SWAYSOCK");
  if (env != nullptr) {
    return std::string(env);
  }
  std::string str;
  {
    std::string str_buf;
    FILE* in;
    char buf[512] = {0};
    if ((in = popen("sway --get-socketpath 2>/dev/null", "r")) == nullptr) {
      throw std::runtime_error("Failed to get socket path");
    }
    while (fgets(buf, sizeof(buf), in) != nullptr) {
      str_buf.append(buf, sizeof(buf));
    }
    pclose(in);
    str = str_buf;
    if (str.empty()) {
      throw std::runtime_error("Socket path is empty");
    }
  }
  if (str.back() == '\n') {
    str.pop_back();
  }
  return str;
}

int Ipc::open(const std::string& socketPath) const {
  util::ScopedFd fd(socket(AF_UNIX, SOCK_STREAM, 0));
  if (fd == -1) {
    throw std::runtime_error("Unable to open Unix socket");
  }
  (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(struct sockaddr_un));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);
  addr.sun_path[sizeof(addr.sun_path) - 1] = 0;
  int l = sizeof(struct sockaddr_un);
  if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), l) == -1) {
    throw std::runtime_error("Unable to connect to Sway");
  }
  return fd.release();
}

struct Ipc::ipc_response Ipc::recv(int fd) {
  std::array<char, ipc_header_size_> header{};
  size_t total = 0;

  while (total < header.size()) {
    auto res = ::recv(fd, header.data() + total, header.size() - total, 0);
    if (fd_event_ == -1 || fd_ == -1) {
      // IPC is closed so just return an empty response
      return {0, 0, ""};
    }
    if (res <= 0) {
      throw std::runtime_error("Unable to receive IPC header");
    }
    total += static_cast<size_t>(res);
  }
  if (!std::string_view{header.data(), header.size()}.starts_with(ipc_magic_)) {
    throw std::runtime_error("Invalid IPC magic");
  }

  const auto payload_size = readU32(std::span<const char>{header}, ipc_magic_.size());
  const auto payload_type =
      readU32(std::span<const char>{header}, ipc_magic_.size() + sizeof(uint32_t));
  total = 0;
  std::string payload;
  payload.resize(payload_size);
  while (total < payload_size) {
    auto res = ::recv(fd, payload.data() + total, payload_size - total, 0);
    if (res < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      throw std::runtime_error("Unable to receive IPC payload");
    }
    if (res == 0) {
      throw std::runtime_error("Unable to receive IPC payload");
    }
    total += static_cast<size_t>(res);
  }
  return {payload_size, payload_type, payload};
}

struct Ipc::ipc_response Ipc::send(int fd, uint32_t type, const std::string& payload) {
  if (payload.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("IPC payload is too large");
  }

  std::array<char, ipc_header_size_> header{};
  std::copy(ipc_magic_.begin(), ipc_magic_.end(), header.begin());
  writeU32(std::span<char>{header}, ipc_magic_.size(), static_cast<uint32_t>(payload.size()));
  writeU32(std::span<char>{header}, ipc_magic_.size() + sizeof(uint32_t), type);

  if (::send(fd, header.data(), header.size(), 0) == -1) {
    throw std::runtime_error("Unable to send IPC header");
  }
  if (::send(fd, payload.c_str(), payload.size(), 0) == -1) {
    throw std::runtime_error("Unable to send IPC payload");
  }
  return Ipc::recv(fd);
}

void Ipc::sendCmd(uint32_t type, const std::string& payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  const auto res = Ipc::send(fd_, type, payload);
  signal_cmd.emit(res);
}

void Ipc::subscribe(const std::string& payload) {
  auto res = Ipc::send(fd_event_, IPC_SUBSCRIBE, payload);
  if (res.payload != "{\"success\": true}") {
    throw std::runtime_error("Unable to subscribe ipc event");
  }
}

void Ipc::handleEvent() {
  const auto res = Ipc::recv(fd_event_);
  signal_event.emit(res);
}

}  // namespace waybar::modules::sway
