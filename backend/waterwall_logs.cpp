#include "waterwall_logs.h"

#include <nlohmann/json.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <deque>
#include <iterator>

namespace wave {
namespace {

constexpr std::array sources{"core", "dns", "network", "internal"};
constexpr off_t max_read_bytes = 32 * 1024;
constexpr std::size_t max_line_bytes = 2048;

std::string print_time(const std::tm& date, const char* format)
{
    char buffer[32];
    return {buffer, std::strftime(buffer, sizeof(buffer), format, &date)};
}

std::tm local_date()
{
    const auto now = std::time(nullptr);
    std::tm date{};
    localtime_r(&now, &date);
    return date;
}

std::filesystem::path log_base(const std::filesystem::path& directory, const nlohmann::json& settings,
                               const char* source)
{
    const auto log = settings.value("log", nlohmann::json::object());
    const auto logger = log.value(source, nlohmann::json::object());
    auto base = log.value("path", std::string("log/")) + logger.value("file", std::string(source) + ".log");
    // WaterWall joins the strings directly and treats a final slash as a disabled file sink.
    if (base.empty() || base.back() == '/') {
        return {};
    }
    if (base.ends_with(".log")) {
        base.resize(base.size() - 4);
    }
    // WaterWall's rotated file path must fit in its 256-byte buffer.
    if (base.empty() || base.size() + std::string_view(".YYYYMMDD.log").size() >= 256) {
        return {};
    }
    return directory / base;
}

WaterWallLog make_record(std::string_view source, std::string line, std::string_view now)
{
    // WaterWall 1.6.0 uses YYYY-MM-DD HH:MM:SS.mmm followed by a space and the level.
    std::string timestamp(now);
    std::string_view level = "info";
    if (line.size() > 24 && line[4] == '-' && line[7] == '-' && line[10] == ' ' && line[13] == ':' &&
        line[16] == ':' && line[19] == '.' && line[23] == ' ') {
        const auto separator = line.find(' ', 24);
        const auto token = std::string_view(line).substr(24, separator - 24);
        if (token == "WARN") {
            level = "warning";
        } else if (token == "ERROR") {
            level = "error";
        } else if (token == "FATAL") {
            level = "fatal";
        } else if (token == "DEBUG") {
            level = "debug";
        } else if (token == "VERBO") {
            level = "verbose";
        }
        timestamp = line.substr(0, 23);
        line.erase(0, line.find_first_not_of(' ', separator));
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    std::ranges::replace_if(line, [](unsigned char ch) { return ch < ' ' && ch != '\t'; }, ' ');
    return {source, std::move(timestamp), level, std::move(line)};
}

} // namespace

WaterWallLogs::WaterWallLogs(const std::filesystem::path& directory) : directory_(directory)
{
}

void WaterWallLogs::start(const nlohmann::json& settings)
{
    stop();
    active_ = true;
    for (std::size_t i = 0; i < sources.size(); ++i) {
        loggers_[i].base = log_base(directory_, settings, sources[i]);
    }
    // A shared file has no information identifying which logger wrote a line.
    const auto with_paths = loggers_;
    for (auto& logger : loggers_) {
        const auto same_path = [&](const Logger& other) {
            return logger.base.lexically_normal() == other.base.lexically_normal();
        };
        if (!logger.base.empty() && std::ranges::count_if(with_paths, same_path) > 1) {
            logger.base.clear();
        }
    }
    update_files(local_date());
    for (auto& logger : loggers_) {
        for (auto& file : logger.files) {
            read_file(file, true);
        }
    }
}

void WaterWallLogs::stop()
{
    active_ = false;
    loggers_ = {};
    dates_ = {};
}

void WaterWallLogs::update_files(const std::tm& date)
{
    const auto today = print_time(date, "%Y%m%d");
    if (dates_[1] == today) {
        return;
    }
    auto prior_date = date;
    --prior_date.tm_mday;
    prior_date.tm_isdst = -1;
    std::mktime(&prior_date);
    auto yesterday = print_time(prior_date, "%Y%m%d");
    // WaterWall can keep writing to yesterday's file until its own rotation boundary.
    for (auto& logger : loggers_) {
        if (logger.base.empty()) {
            continue;
        }
        if (dates_[1] == yesterday) {
            logger.files[0] = std::move(logger.files[1]);
        } else if (dates_[0] != yesterday) {
            logger.files[0] = File{logger.base.string() + '.' + yesterday + ".log"};
        }
        logger.files[1] = File{logger.base.string() + '.' + today + ".log"};
    }
    dates_ = {std::move(yesterday), today};
}

std::string WaterWallLogs::read_file(File& file, bool skip_existing)
{
    if (file.path.empty()) {
        return {};
    }
    struct Descriptor {
        int fd;
        ~Descriptor()
        {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    } input{::open(file.path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK)};
    struct stat info{};
    if (input.fd < 0 || ::fstat(input.fd, &info) != 0 || !S_ISREG(info.st_mode)) {
        return {};
    }
    if (file.device != info.st_dev || file.inode != info.st_ino || file.offset > info.st_size) {
        file.device = info.st_dev;
        file.inode = info.st_ino;
        file.offset = 0;
        file.partial.clear();
        file.skip_line = file.truncated = false;
    }
    if (skip_existing || info.st_size - file.offset > max_read_bytes) {
        file.offset = skip_existing ? info.st_size : info.st_size - max_read_bytes;
        file.partial.clear();
        file.truncated = false;
        char previous = '\0';
        file.skip_line =
            file.offset > 0 && (::pread(input.fd, &previous, 1, file.offset - 1) != 1 || previous != '\n');
    }
    const auto available = info.st_size - file.offset;
    if (available <= 0) {
        return {};
    }
    std::string bytes(static_cast<std::size_t>(available), '\0');
    ssize_t received;
    do {
        received = ::pread(input.fd, bytes.data(), bytes.size(), file.offset);
    } while (received < 0 && errno == EINTR);
    if (received <= 0) {
        return {};
    }
    file.offset += received;
    bytes.resize(static_cast<std::size_t>(received));
    return bytes;
}

std::vector<WaterWallLog> WaterWallLogs::read()
{
    std::vector<WaterWallLog> logs;
    if (!active_) {
        return logs;
    }
    const auto date = local_date();
    const auto now = print_time(date, "%Y-%m-%d %H:%M:%S");
    update_files(date);
    for (std::size_t i = 0; i < loggers_.size(); ++i) {
        std::deque<WaterWallLog> recent;
        for (auto& file : loggers_[i].files) {
            const auto bytes = read_file(file);
            std::string_view pending(bytes);
            while (!pending.empty()) {
                const auto newline = pending.find('\n');
                const auto line = pending.substr(0, newline);
                if (!file.skip_line) {
                    const auto remaining = max_line_bytes - file.partial.size();
                    file.partial.append(line.substr(0, remaining));
                    file.truncated |= line.size() > remaining;
                }
                if (newline == pending.npos) {
                    break;
                }
                if (!file.partial.empty()) {
                    auto entry = make_record(sources[i], std::move(file.partial), now);
                    if (!entry.message.empty()) {
                        if (file.truncated) {
                            entry.message += "…";
                        }
                        if (recent.size() == log_history_per_source) {
                            recent.pop_front();
                        }
                        recent.push_back(std::move(entry));
                    }
                }
                file.partial.clear();
                file.skip_line = file.truncated = false;
                pending.remove_prefix(newline + 1);
            }
        }
        logs.insert(logs.end(), std::make_move_iterator(recent.begin()),
                    std::make_move_iterator(recent.end()));
    }
    std::ranges::stable_sort(logs, {}, &WaterWallLog::timestamp);
    return logs;
}

} // namespace wave
