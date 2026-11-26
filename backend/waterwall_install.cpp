#include "waterwall_install.h"

#include "wave_version.h"

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))

#include <openssl/evp.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

extern char** environ;

namespace wave {
namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

constexpr std::string_view pinned_release = "1.6.0";
#if defined(__x86_64__)
constexpr std::string_view package = "Waterwall-linux-gcc-x64-old-cpu.zip";
constexpr std::string_view expected_sha256 =
    "0121139392483e7308c8521ce37c4393afd28a394473fec22ebab776f720bab3";
#else
constexpr std::string_view package = "Waterwall-linux-gcc-arm64-old-cpu.zip";
constexpr std::string_view expected_sha256 =
    "a51324868bb69f4a89add08e55fd63ef78ef346f85225c5c88637a0e05b598d7";
#endif
constexpr std::size_t max_archive_size = 64 * 1024 * 1024;
constexpr std::size_t max_binary_size = 128 * 1024 * 1024;
constexpr char cancelled[] = "WaterWall installation was cancelled.";

class File {
  public:
    explicit File(int fd) : fd_(fd)
    {
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    ~File()
    {
        close();
    }
    int get() const
    {
        return fd_;
    }

    void close()
    {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

  private:
    int fd_;
};

class Staging {
  public:
    explicit Staging(const fs::path& directory) : name_((directory / ".waterwall-install-XXXXXX").string())
    {
        if (!mkdtemp(name_.data())) {
            name_.clear();
        }
    }

    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    ~Staging()
    {
        if (!name_.empty()) {
            std::error_code ignored;
            fs::remove_all(name_, ignored);
        }
    }

    const std::string& name() const
    {
        return name_;
    }

  private:
    std::string name_;
};

class Child {
  public:
    explicit Child(pid_t pid) : pid_(pid)
    {
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child()
    {
        if (pid_ > 0) {
            kill(-pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
            }
        }
    }

    pid_t poll(int& status)
    {
        const auto reaped = waitpid(pid_, &status, WNOHANG);
        if (reaped > 0 || (reaped < 0 && errno == ECHILD)) {
            pid_ = 0;
        }
        return reaped;
    }

  private:
    pid_t pid_;
};

int spawn_to_pipe(pid_t& pid, const std::vector<char*>& argv, int writer)
{
    posix_spawn_file_actions_t actions;
    if (const int error = posix_spawn_file_actions_init(&actions)) {
        return error;
    }
    posix_spawnattr_t attributes;
    int error = posix_spawnattr_init(&attributes);
    if (error != 0) {
        posix_spawn_file_actions_destroy(&actions);
        return error;
    }
    if ((error = posix_spawn_file_actions_adddup2(&actions, writer, STDOUT_FILENO)) == 0 &&
        (error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0)) == 0 &&
        (error = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0)) == 0 &&
        (error = posix_spawnattr_setpgroup(&attributes, 0)) == 0 &&
        (error = posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP)) == 0) {
        error = posix_spawnp(&pid, argv.front(), &actions, &attributes, argv.data(), environ);
    }
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return error;
}

bool write_bytes(int fd, std::string_view data)
{
    while (!data.empty()) {
        const auto amount = write(fd, data.data(), data.size());
        if (amount < 0 && errno == EINTR) {
            continue;
        }
        if (amount <= 0) {
            return false;
        }
        data.remove_prefix(static_cast<std::size_t>(amount));
    }
    return true;
}

std::string run_to_file(std::vector<std::string> command, const fs::path& target, std::size_t limit,
                        std::chrono::seconds timeout, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return cancelled;
    }
    const File output(open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600));
    if (output.get() < 0) {
        return "Cannot create a staged WaterWall file.";
    }
    int ends[2]{};
    if (pipe2(ends, O_CLOEXEC) != 0) {
        return "Cannot read from the WaterWall installer tool.";
    }
    const File reader(ends[0]);
    File writer(ends[1]);
    std::vector<char*> argv;
    for (auto& argument : command) {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);
    const auto& tool = command.front();
    const auto failure = "WaterWall installer command '" + tool + "' failed.";
    pid_t pid = 0;
    if (const int error = spawn_to_pipe(pid, argv, writer.get())) {
        return error == ENOENT ? "WaterWall installation requires " + tool + " on PATH." : failure;
    }
    Child child(pid);
    writer.close();
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t saved = 0;
    std::array<char, 64 * 1024> bytes;
    bool input_ended = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop.stop_requested()) {
            return cancelled;
        }
        if (input_ended) {
            int status = 0;
            const auto reaped = child.poll(status);
            if (reaped > 0) {
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || saved == 0) {
                    return failure;
                }
                return fsync(output.get()) == 0 ? "" : "Cannot commit the staged WaterWall file.";
            }
            if (reaped < 0 && errno != EINTR) {
                return failure;
            }
            std::this_thread::sleep_for(10ms);
            continue;
        }
        pollfd input{reader.get(), POLLIN, 0};
        const int available = poll(&input, 1, 100);
        if (available < 0 && errno != EINTR) {
            return failure;
        }
        if (available <= 0) {
            continue;
        }
        const auto received = read(reader.get(), bytes.data(), bytes.size());
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            return failure;
        }
        if (received == 0) {
            input_ended = true;
            continue;
        }
        const auto count = static_cast<std::size_t>(received);
        if (count > limit - saved) {
            return "A WaterWall installation file exceeds its size limit.";
        }
        if (!write_bytes(output.get(), {bytes.data(), count})) {
            return "Cannot write the staged WaterWall file.";
        }
        saved += count;
    }
    return "WaterWall installer command '" + tool + "' timed out.";
}

bool verify_sha256(const fs::path& archive, std::stop_token stop)
{
    std::ifstream input(archive, std::ios::binary);
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!input || !context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) {
        return false;
    }
    std::array<char, 64 * 1024> bytes;
    while (input) {
        if (stop.stop_requested()) {
            return false;
        }
        input.read(bytes.data(), bytes.size());
        if (EVP_DigestUpdate(context.get(), bytes.data(), static_cast<std::size_t>(input.gcount())) != 1) {
            return false;
        }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest;
    unsigned int size = 0;
    if (input.bad() || !input.eof() || EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1) {
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string hash;
    for (unsigned int i = 0; i < size; ++i) {
        hash += hex[digest[i] >> 4];
        hash += hex[digest[i] & 15];
    }
    return hash == expected_sha256;
}

} // namespace

std::string install_waterwall(const fs::path& directory, std::stop_token stop)
{
    if (stop.stop_requested()) {
        return cancelled;
    }
    if (std::string_view(version) != pinned_release) {
        return "This Wave version has no verified WaterWall release archive.";
    }
    std::error_code error;
    const auto destination = fs::absolute(directory, error);
    if (error) {
        return "Cannot locate the WaterWall binary directory.";
    }
    const Staging staging(destination);
    if (staging.name().empty()) {
        return "Cannot create a private staging directory for WaterWall.";
    }
    const fs::path archive = fs::path(staging.name()) / "Waterwall.zip";
    const auto url = std::string("https://github.com/radkesvat/WaterWall/releases/download/v") + version +
                     '/' + std::string(package);
    if (const auto failure =
            run_to_file({"curl", "-q", "--fail", "--silent", "--show-error", "--location", "--proto",
                         "=https", "--proto-redir", "=https", "--max-redirs", "5", "--connect-timeout", "10",
                         "--max-time", "120", "--max-filesize", std::to_string(max_archive_size), url},
                        archive, max_archive_size, 125s, stop);
        !failure.empty()) {
        return failure;
    }
    if (!verify_sha256(archive, stop)) {
        return stop.stop_requested() ? cancelled : "The WaterWall archive failed its SHA-256 check.";
    }
    const fs::path binary = fs::path(staging.name()) / "Waterwall";
    if (const auto failure =
            run_to_file({"unzip", "-p", archive.string(), "Waterwall"}, binary, max_binary_size, 30s, stop);
        !failure.empty()) {
        return failure;
    }
    if (stop.stop_requested()) {
        return cancelled;
    }
    if (chmod(binary.c_str(), 0700) != 0) {
        return "Cannot mark the installed WaterWall binary executable.";
    }
    // Publish the fully extracted file in one step and preserve an existing installation.
    fs::create_hard_link(binary, destination / "Waterwall", error);
    return error ? "Cannot publish the WaterWall binary in its installation directory." : "";
}

} // namespace wave

#else

namespace wave {

std::string install_waterwall(const std::filesystem::path&, std::stop_token)
{
    return "Automatic WaterWall installation requires x86-64 or ARM64 Linux.";
}

} // namespace wave

#endif
