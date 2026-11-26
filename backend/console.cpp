#include "console.h"

#include "auth.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <utility>

#ifdef __linux__
#include "child_process.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <termios.h>
#endif

namespace wave {
namespace {

using Json = nlohmann::json;
using httplib::ws::CloseStatus;
using httplib::ws::WebSocket;

void console_error(WebSocket& socket, const char* message, CloseStatus status)
{
    if (socket.send(Json{{"type", "error"}, {"message", message}}.dump())) {
        socket.close(status, message);
    }
}

#ifdef __linux__
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr auto session_check_interval = 250ms;
constexpr std::size_t max_input = 64 * 1024;
constexpr int max_dimension = 1000;
// Each terminal occupies an HTTP worker even while idle. The default pool starts with at least eight.
constexpr int max_consoles = 4;

class ConsoleSlot {
  public:
    ConsoleSlot() : acquired_(slots_.try_acquire()) {}
    ~ConsoleSlot()
    {
        if (acquired_) {
            slots_.release();
        }
    }
    ConsoleSlot(const ConsoleSlot&) = delete;
    ConsoleSlot& operator=(const ConsoleSlot&) = delete;
    explicit operator bool() const { return acquired_; }

  private:
    inline static std::counting_semaphore<max_consoles> slots_{max_consoles};
    bool acquired_;
};

class Descriptor {
  public:
    explicit Descriptor(int fd) : fd_(fd) {}
    ~Descriptor() { close(); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const { return fd_; }
    int release() { return std::exchange(fd_, -1); }
    void close()
    {
        if (fd_ >= 0) {
            ::close(release());
        }
    }

  private:
    int fd_;
};

int above_stdio(int fd)
{
    if (fd < 0 || fd > STDERR_FILENO) {
        return fd;
    }
    const int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    ::close(fd);
    return moved;
}

class Pseudoterminal {
  public:
    Pseudoterminal() = default;
    ~Pseudoterminal()
    {
        finish();
        if (master_ >= 0) {
            ::close(master_);
        }
    }
    Pseudoterminal(const Pseudoterminal&) = delete;
    Pseudoterminal& operator=(const Pseudoterminal&) = delete;

    bool start(const std::filesystem::path& directory, const winsize& size)
    {
        Descriptor master(above_stdio(::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK)));
        if (master.get() < 0 || ::grantpt(master.get()) != 0 || ::unlockpt(master.get()) != 0) {
            return false;
        }
        Descriptor slave(above_stdio(::ioctl(master.get(), TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC)));
        if (slave.get() < 0 || ::ioctl(slave.get(), TIOCSWINSZ, &size) != 0) {
            return false;
        }
        termios settings{};
        if (::tcgetattr(slave.get(), &settings) == 0) {
            settings.c_iflag |= IUTF8;
            ::tcsetattr(slave.get(), TCSANOW, &settings);
        }

        const char* shell = ::access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
        char term[] = "TERM=xterm-256color";
        char colorterm[] = "COLORTERM=truecolor";
        auto shell_env = std::string("SHELL=") + shell;
        std::vector<char*> env;
        for (auto variable = ::environ; *variable; ++variable) {
            const std::string_view entry = *variable;
            if (!entry.starts_with("TERM=") && !entry.starts_with("COLORTERM=") &&
                !entry.starts_with("SHELL=") && !entry.starts_with("COLUMNS=") && !entry.starts_with("LINES=")) {
                env.push_back(*variable);
            }
        }
        env.insert(env.end(), {term, colorterm, shell_env.data(), nullptr});
        char interactive[] = "-i";
        char* arguments[] = {const_cast<char*>(shell), interactive, nullptr};
        const auto environment = env.data();
        const auto working_dir = directory.c_str();
        const auto descriptor_limit = open_file_limit();
        const auto parent = ::getpid();
        struct sigaction default_pipe{};
        default_pipe.sa_handler = SIG_DFL;
        ::sigemptyset(&default_pipe.sa_mask);

        int launch_pipe[2];
        if (::pipe2(launch_pipe, O_CLOEXEC) != 0) {
            return false;
        }
        Descriptor reader(launch_pipe[0]);
        Descriptor writer(above_stdio(launch_pipe[1]));
        if (writer.get() < 0) {
            return false;
        }

        const auto child = ::fork();
        if (child == 0) {
            reader.close();
            master.close();
            if (::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == parent && ::setsid() >= 0 &&
                ::ioctl(slave.get(), TIOCSCTTY, 0) == 0 && ::chdir(working_dir) == 0 &&
                ::sigaction(SIGPIPE, &default_pipe, nullptr) == 0 && ::dup2(slave.get(), STDIN_FILENO) >= 0 &&
                ::dup2(slave.get(), STDOUT_FILENO) >= 0 && ::dup2(slave.get(), STDERR_FILENO) >= 0) {
                close_inherited_files(writer.get(), descriptor_limit);
                ::execve(shell, arguments, environment);
            }
            const char failed = 1;
            const auto ignored = ::write(writer.get(), &failed, sizeof(failed));
            static_cast<void>(ignored);
            ::_exit(127);
        }
        if (child < 0) {
            return false;
        }
        pid_ = child;
        master_ = master.release();
        writer.close();
        slave.close();
        char failed;
        ssize_t result;
        do {
            result = ::read(reader.get(), &failed, sizeof(failed));
        } while (result < 0 && errno == EINTR);
        return result == 0;
    }

    int master() const { return master_; }
    bool resize(const winsize& size) const { return ::ioctl(master_, TIOCSWINSZ, &size) == 0; }
    bool exited() const
    {
        siginfo_t info{};
        return ::waitid(P_PID, pid_, &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid_;
    }

    void finish()
    {
        const std::lock_guard lock(finish_mutex_);
        if (pid_ < 0 || reaped_) {
            return;
        }
        const auto foreground = ::tcgetpgrp(master_);
        if (foreground > 0 && foreground != pid_) {
            ::kill(-foreground, SIGHUP);
            ::kill(-foreground, SIGCONT);
        }
        ::kill(pid_, SIGHUP);
        ::kill(pid_, SIGCONT);
        const auto deadline = Clock::now() + 150ms;
        while (!exited() && Clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        if (foreground > 0 && foreground != pid_ && ::getsid(foreground) == pid_) {
            ::kill(-foreground, SIGKILL);
        }
        if (!exited()) {
            ::kill(-pid_, SIGKILL);
            ::kill(pid_, SIGKILL);
        }
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
        reaped_ = true;
    }

  private:
    int master_ = -1;
    pid_t pid_ = -1;
    std::mutex finish_mutex_;
    bool reaped_ = false;
};

std::optional<winsize> console_size(const std::string& message)
{
    if (message.size() > 256) {
        return {};
    }
    const auto input = Json::parse(message, nullptr, false);
    if (!input.is_object() || !input.contains("type") || input["type"] != "resize") {
        return {};
    }
    const auto dimension = [&](const char* name) -> unsigned short {
        const auto field = input.find(name);
        if (field == input.end() || !field->is_number_integer()) {
            return 0;
        }
        const auto value = field->get<unsigned long long>();
        return value <= max_dimension ? static_cast<unsigned short>(value) : 0;
    };
    const auto rows = dimension("rows");
    const auto cols = dimension("cols");
    if (!rows || !cols) {
        return {};
    }
    return winsize{.ws_row = rows, .ws_col = cols, .ws_xpixel = 0, .ws_ypixel = 0};
}

enum class End { active, disconnected, revoked, exited, io_error };

End stream_output(std::stop_token stop, const Pseudoterminal& terminal, WebSocket& socket,
                  const std::function<bool()>& session)
{
    std::array<char, 16384> buffer;
    pollfd master{terminal.master(), POLLIN, 0};
    int exit_reads = 16;
    while (!stop.stop_requested() && socket.is_open()) {
        const int ready = ::poll(&master, 1, session_check_interval.count());
        if (ready < 0 && errno != EINTR) {
            return End::io_error;
        }
        if (stop.stop_requested()) {
            break;
        }
        if (!session()) {
            return End::revoked;
        }
        if (ready > 0) {
            const auto count = ::read(master.fd, buffer.data(), buffer.size());
            if (count > 0) {
                if (!socket.send(buffer.data(), static_cast<std::size_t>(count))) {
                    return End::disconnected;
                }
                // Drain queued output, with a bound if a background process is still writing.
                if (terminal.exited() && --exit_reads == 0) {
                    return End::exited;
                }
                continue;
            }
            // Linux PTY masters report EIO when the last slave closes.
            if (count == 0 || errno == EIO) {
                return End::exited;
            }
            if (errno != EAGAIN && errno != EINTR) {
                return End::io_error;
            }
        }
        // An inherited slave must not keep the console open after its shell exits.
        if (terminal.exited()) {
            return End::exited;
        }
    }
    return End::disconnected;
}

bool write_input(const Pseudoterminal& terminal, std::string_view bytes, const std::function<bool()>& session,
                 const std::atomic<End>& output_end)
{
    // A stopped or busy foreground job can fill the PTY input queue.
    const auto deadline = Clock::now() + 2s;
    while (!bytes.empty() && output_end.load() == End::active && session() && Clock::now() < deadline) {
        const auto count = ::write(terminal.master(), bytes.data(), bytes.size());
        if (count > 0) {
            bytes.remove_prefix(static_cast<std::size_t>(count));
        } else if (count < 0 && errno == EAGAIN) {
            pollfd master{terminal.master(), POLLOUT, 0};
            const auto ready = ::poll(&master, 1, session_check_interval.count());
            if ((ready < 0 && errno != EINTR) || (ready > 0 && (master.revents & (POLLERR | POLLHUP | POLLNVAL)))) {
                return false;
            }
        } else if (count >= 0 || errno != EINTR) {
            return false;
        }
    }
    return bytes.empty();
}

void run_console(Pseudoterminal& terminal, WebSocket& socket, const std::function<bool()>& session)
{
    if (!session()) {
        terminal.finish();
        socket.close(CloseStatus::PolicyViolation, "Session ended.");
        return;
    }
    if (!socket.send(Json{{"type", "ready"}}.dump())) {
        return;
    }

    std::atomic<End> output_end = End::active;
    std::jthread output([&](std::stop_token stop) {
        const auto end = stream_output(stop, terminal, socket, session);
        output_end = end;
        // The input handler can be busy assembling a WebSocket message when the terminal ends.
        terminal.finish();
        if (end == End::revoked) {
            socket.close(CloseStatus::PolicyViolation, "Session ended.");
        }
    });
    std::string message;
    auto end = End::active;
    const char* input_error = nullptr;
    while ((end = output_end.load()) == End::active) {
        const auto result = socket.read(message);
        if (!session()) {
            end = End::revoked;
            break;
        }
        if ((end = output_end.load()) != End::active) {
            break;
        }
        if (result == httplib::ws::Fail) {
            end = End::disconnected;
            break;
        }
        if (result == httplib::ws::Text) {
            const auto size = console_size(message);
            if (!size) {
                input_error = "Invalid terminal dimensions.";
                break;
            }
            if (!terminal.resize(*size)) {
                end = End::io_error;
                break;
            }
        } else if (result == httplib::ws::Binary) {
            if (message.size() > max_input) {
                input_error = "Terminal input message is too large.";
                break;
            }
            if (!write_input(terminal, message, session, output_end)) {
                end = output_end.load();
                if (end == End::active) {
                    end = End::io_error;
                }
                break;
            }
        }
    }

    output.request_stop();
    terminal.finish();
    output.join();
    if (end == End::revoked || !session()) {
        socket.close(CloseStatus::PolicyViolation, "Session ended.");
    } else if (input_error) {
        console_error(socket, input_error, CloseStatus::InvalidPayload);
    } else if (end == End::io_error) {
        console_error(socket, "Terminal I/O failed.", CloseStatus::InternalError);
    } else if (end == End::exited && socket.send(Json{{"type", "exit"}, {"message", "Console exited."}}.dump())) {
        socket.close();
    }
}

void serve_console(WebSocket& socket, const std::function<bool()>& session, const std::filesystem::path& directory)
{
    const ConsoleSlot slot;
    if (!slot) {
        console_error(socket, "All console sessions are in use.", CloseStatus::PolicyViolation);
        return;
    }
    socket.set_read_timeout(session_check_interval);
    std::string message;
    const auto deadline = Clock::now() + 10s;
    while (Clock::now() < deadline) {
        const auto result = socket.read(message);
        if (!session()) {
            socket.close(CloseStatus::PolicyViolation, "Session ended.");
            return;
        }
        if (result == httplib::ws::Timeout) {
            continue;
        }
        if (result == httplib::ws::Fail) {
            return;
        }
        const auto size = result == httplib::ws::Text ? console_size(message) : std::nullopt;
        if (!size) {
            console_error(socket, "Send initial terminal dimensions before input.", CloseStatus::InvalidPayload);
            return;
        }
        Pseudoterminal terminal;
        if (!terminal.start(directory, *size)) {
            terminal.finish();
            console_error(socket, "Cannot start the terminal shell.", CloseStatus::InternalError);
            return;
        }
        try {
            run_console(terminal, socket, session);
        } catch (const std::system_error&) {
            terminal.finish();
            console_error(socket, "Cannot run the terminal session.", CloseStatus::InternalError);
        }
        return;
    }
    console_error(socket, "Timed out waiting for terminal dimensions.", CloseStatus::PolicyViolation);
}
#endif

} // namespace

void configure_console_route(httplib::Server& server, std::shared_ptr<AuthStore> store,
                             const std::filesystem::path& directory)
{
    server.WebSocket("/ws/console", [store = std::move(store), directory](const httplib::Request& request,
                                                                      WebSocket& socket) {
        const auto session = websocket_session(store, request);
        if (!session()) {
            socket.close(CloseStatus::PolicyViolation, "Session ended.");
            return;
        }
#ifdef __linux__
        serve_console(socket, session, directory);
#else
        console_error(socket, "The interactive console requires a Linux backend.", CloseStatus::InternalError);
#endif
    });
}

} // namespace wave
