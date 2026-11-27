#include "waterwall.h"

#include "child_process.h"
#include "waterwall_install.h"
#include "waterwall_schematic.h"
#include "waterwall_schematic_file.h"

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace wave {
namespace {

using namespace std::chrono_literals;
namespace fs = std::filesystem;

bool trusted(const struct stat& info)
{
    return (info.st_uid == ::geteuid() || info.st_uid == 0) && !(info.st_mode & (S_IWGRP | S_IWOTH));
}

bool trusted_directory(const fs::path& path)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode) && trusted(info);
}

bool prepare_directory(const fs::path& path)
{
    return (::mkdir(path.c_str(), 0700) == 0 || errno == EEXIST) && trusted_directory(path);
}

int core_input(std::string_view json)
{
    const int fd = above_stdio(::memfd_create("core.json", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (fd < 0) {
        return -1;
    }
    while (!json.empty()) {
        const auto written = ::write(fd, json.data(), json.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ::close(fd);
            return -1;
        }
        json.remove_prefix(static_cast<std::size_t>(written));
    }
    constexpr int seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
    if (::lseek(fd, 0, SEEK_SET) < 0 || ::fcntl(fd, F_ADD_SEALS, seals) < 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

// Takes ownership of the configuration descriptor.
pid_t launch(const fs::path& executable, const fs::path& directory, int input)
{
    int result_pipe[2];
    if (::pipe2(result_pipe, O_CLOEXEC) != 0) {
        ::close(input);
        return -1;
    }
    result_pipe[1] = above_stdio(result_pipe[1]);
    if (result_pipe[1] < 0) {
        ::close(result_pipe[0]);
        ::close(input);
        return -1;
    }
    const auto descriptor_limit = open_file_limit();
    const auto parent = ::getpid();
    const char* program = executable.c_str();
    const char* working_dir = directory.c_str();
    const auto child = ::fork();
    if (child == 0) {
        // Keep the child tied to the sampler even when Wave is terminated by a signal.
        ::close(result_pipe[0]);
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) == 0 && ::getppid() == parent && ::chdir(working_dir) == 0 &&
            ::dup2(input, STDIN_FILENO) >= 0) {
            ::close(input);
            close_inherited_files(result_pipe[1], descriptor_limit);
            ::execl(program, program, "--config:stdin", static_cast<char*>(nullptr));
        }
        const char failed = 1;
        const auto written = ::write(result_pipe[1], &failed, sizeof(failed));
        static_cast<void>(written);
        ::_exit(127);
    }
    ::close(input);
    ::close(result_pipe[1]);
    char failed;
    ssize_t received;
    do {
        received = ::read(result_pipe[0], &failed, sizeof(failed));
    } while (received < 0 && errno == EINTR);
    ::close(result_pipe[0]);
    // The close-on-exec pipe returns EOF when the program has been launched.
    if (child > 0 && received != 0) {
        ::kill(child, SIGKILL);
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
        }
        return -1;
    }
    return child;
}

} // namespace

WaterWall::WaterWall(const fs::path& directory, std::string_view version)
    : working_dir_(directory),
      release_dir_(waterwall_release_directory(waterwall_bin_directory(directory), version)),
      executable_(release_dir_ / "Waterwall")
{
    if (!prepare_directory(release_dir_.parent_path()) || !prepare_directory(release_dir_)) {
        error_ = "Cannot create or use the WaterWall release directory.";
    }
}

WaterWall::~WaterWall()
{
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
}

std::string_view WaterWall::state() const
{
    if (installation_.valid()) {
        return "installing";
    }
    if (core_json_) {
        return "starting";
    }
    if (pid_ > 0) {
        return stop_deadline_ ? "stopping" : "running";
    }
    return installed() ? "stopped" : "missing";
}

const std::string& WaterWall::error() const
{
    return error_;
}

pid_t WaterWall::running_pid() const
{
    return stop_deadline_ ? -1 : pid_;
}

bool WaterWall::installed() const
{
    struct stat info{};
    return trusted_directory(release_dir_.parent_path()) && trusted_directory(release_dir_) &&
           ::lstat(executable_.c_str(), &info) == 0 && S_ISREG(info.st_mode) && trusted(info) &&
           !(info.st_mode & (S_ISUID | S_ISGID)) && ::access(executable_.c_str(), X_OK) == 0;
}

void WaterWall::install_if_missing(const nlohmann::json& schematic)
{
    if (state() == "missing") {
        error_ = install(schematic);
    }
}

std::string WaterWall::install(const nlohmann::json& schematic)
{
    if (state() != "missing") {
        return "WaterWall is already installed or an operation is in progress.";
    }
    if (!prepare_directory(release_dir_.parent_path()) || !prepare_directory(release_dir_)) {
        return "Cannot create or use the WaterWall release directory.";
    }
    std::packaged_task<std::string(std::stop_token)> task(
        [directory = release_dir_, schematic](std::stop_token stop) {
            try {
                return install_waterwall(directory, schematic, stop);
            } catch (const std::exception&) {
                return std::string("WaterWall installation could not be completed.");
            }
        });
    auto result = task.get_future();
    try {
        installer_ = std::jthread(std::move(task));
    } catch (const std::system_error&) {
        return "Cannot start the WaterWall installer.";
    }
    installation_ = std::move(result);
    error_.clear();
    return {};
}

std::string WaterWall::start(std::string core_json)
{
    if (state() != "stopped") {
        return "WaterWall must be installed and stopped before it can be started.";
    }
    error_.clear();
    core_json_ = std::move(core_json);
    return {};
}

std::string WaterWall::stop()
{
    if (state() != "running") {
        return "WaterWall must be running before it can be stopped.";
    }
    if (::kill(pid_, SIGTERM) != 0 && errno != ESRCH) {
        return "Cannot signal the WaterWall process.";
    }
    stop_deadline_ = std::chrono::steady_clock::now() + 5s;
    return {};
}

void WaterWall::update()
{
    if (installation_.valid() && installation_.wait_for(0s) == std::future_status::ready) {
        error_ = installation_.get();
    }
    if (core_json_) {
        if (!installed()) {
            error_ = "Cannot launch the WaterWall executable.";
        } else if (const int input = core_input(*core_json_); input < 0) {
            error_ = "Cannot prepare the WaterWall core configuration.";
        } else if ((pid_ = launch(executable_, working_dir_, input)) < 0) {
            error_ = "Cannot launch the WaterWall executable.";
        }
        core_json_.reset();
    }
    if (pid_ <= 0) {
        return;
    }
    const auto child = ::waitpid(pid_, nullptr, WNOHANG);
    if (child > 0 || (child < 0 && errno == ECHILD)) {
        if (!stop_deadline_) {
            error_ = "WaterWall exited. Check its configuration and logs.";
        }
        pid_ = -1;
        stop_deadline_.reset();
    } else if (stop_deadline_ && std::chrono::steady_clock::now() >= *stop_deadline_) {
        ::kill(pid_, SIGKILL);
    }
}

} // namespace wave
