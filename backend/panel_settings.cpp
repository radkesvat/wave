#include "panel_settings.h"

#include "panel_base_path.h"
#include "session_settings.h"
#include "waterwall_schematic.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <aclapi.h>
#include <sddl.h>
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#include <vector>
#endif

#ifndef _WIN32
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wave {
namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;
constexpr char default_host[] = "0.0.0.0";
constexpr int default_port = 2045;
constexpr char database_name[] = "wave-db.json";
constexpr std::array<std::string_view, 7> panel_backup_keys{
    "admin", panel_base_path_key, "listen", "tls", "session_expiry_days", "theme", "donation",
};
constexpr std::array<std::string_view, 3> tls_backup_text_keys{"domain", "certificate_chain", "private_key"};

class DatabaseDirectorySyncError : public std::runtime_error {
public:
    DatabaseDirectorySyncError() : std::runtime_error("Cannot sync the wave-db.json directory after saving.")
    {
    }
};

fs::path executable_path([[maybe_unused]] const char* executable)
{
#if defined(__linux__)
    return fs::read_symlink("/proc/self/exe");
#elif defined(_WIN32)
    std::wstring path(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        throw std::runtime_error("Cannot locate the Wave executable.");
    }
    path.resize(length);
    return fs::canonical(path);
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        throw std::runtime_error("Cannot locate the Wave executable.");
    }
    return fs::canonical(path.data());
#else
    return fs::canonical(executable);
#endif
}

#ifdef _WIN32
class PrivateAccess {
  public:
    PrivateAccess()
    {
        HANDLE handle = nullptr;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle)) {
            throw std::runtime_error("Cannot identify the Wave user.");
        }
        const std::unique_ptr<void, decltype(&CloseHandle)> token(handle, CloseHandle);
        alignas(TOKEN_USER) std::array<unsigned char, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> data{};
        DWORD length = 0;
        wchar_t* account = nullptr;
        if (!GetTokenInformation(token.get(), TokenUser, data.data(), static_cast<DWORD>(data.size()),
                                 &length) ||
            !ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid, &account)) {
            throw std::runtime_error("Cannot identify the Wave user.");
        }
        const std::unique_ptr<wchar_t, decltype(&LocalFree)> sid(account, LocalFree);
        const auto sddl = std::wstring(L"D:P(A;;GA;;;SY)(A;;GA;;;") + sid.get() + L")";
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor,
                                                                  nullptr)) {
            throw std::runtime_error("Cannot configure private access to wave-db.json.");
        }
        descriptor_.reset(descriptor);
        attributes_.lpSecurityDescriptor = descriptor_.get();
    }

    SECURITY_ATTRIBUTES* attributes()
    {
        return &attributes_;
    }

    void protect(HANDLE file) const
    {
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        if (!GetSecurityDescriptorDacl(descriptor_.get(), &present, &dacl, &defaulted) || !present || !dacl ||
            SetSecurityInfo(file, SE_FILE_OBJECT,
                            DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr,
                            dacl, nullptr) != ERROR_SUCCESS) {
            throw std::runtime_error("Cannot restrict access to wave-db.json.");
        }
    }

  private:
    std::unique_ptr<void, decltype(&LocalFree)> descriptor_{nullptr, LocalFree};
    SECURITY_ATTRIBUTES attributes_{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
};
#else
class File {
  public:
    explicit File(int fd) : fd_(fd)
    {
    }
    ~File()
    {
        if (fd_ >= 0) {
            close(fd_);
        }
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const
    {
        return fd_;
    }

  private:
    int fd_;
};
#endif

class DatabaseFile {
  public:
    explicit DatabaseFile(const fs::path& directory)
#ifdef _WIN32
        : path_(directory / database_name)
#else
        : directory_(open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC))
#endif
    {
#ifndef _WIN32
        if (directory_.get() < 0) {
            throw std::runtime_error("Cannot open the directory containing wave-db.json.");
        }
#endif
    }

    void lock()
    {
#ifdef _WIN32
        if (lock_) {
            return;
        }
        const auto name = L"Global\\WavePanelDatabase-" + std::to_wstring(fs::hash_value(path_));
        lock_ = CreateMutexW(access_.attributes(), FALSE, name.c_str());
        const auto acquired = lock_ ? WaitForSingleObject(lock_, 0) : WAIT_FAILED;
        if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED) {
            if (lock_) {
                CloseHandle(lock_);
                lock_ = nullptr;
            }
            throw std::runtime_error("Another Wave process is using wave-db.json.");
        }
#else
        if (flock(directory_.get(), LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Another Wave process is using wave-db.json.");
        }
#endif
    }

#ifdef _WIN32
    ~DatabaseFile()
    {
        if (lock_) {
            ReleaseMutex(lock_);
            CloseHandle(lock_);
        }
    }
#endif

    std::optional<Json> read_settings(bool protect_permissions = true) const
    {
        std::string serialized;
        char buffer[4096];
#ifdef _WIN32
        const auto access = GENERIC_READ | (protect_permissions ? WRITE_DAC : 0);
        const auto handle = CreateFileW(path_.c_str(), access, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_FILE_NOT_FOUND) {
            return std::nullopt;
        }
        if (handle == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot read wave-db.json.");
        }
        const std::unique_ptr<void, decltype(&CloseHandle)> file(handle, CloseHandle);
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(file.get(), &info) ||
            (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
            info.nNumberOfLinks != 1) {
            throw std::runtime_error("wave-db.json must be a regular file beside the executable.");
        }
        if (protect_permissions) {
            access_.protect(file.get());
        }
        for (;;) {
            DWORD count = 0;
            if (!ReadFile(file.get(), buffer, sizeof(buffer), &count, nullptr)) {
                throw std::runtime_error("Cannot read wave-db.json.");
            }
            if (count == 0) {
                break;
            }
            serialized.append(buffer, count);
        }
#else
        const File file(
            openat(directory_.get(), database_name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (file.get() < 0 && errno == ENOENT) {
            return std::nullopt;
        }
        struct stat info{};
        if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1) {
            throw std::runtime_error("wave-db.json must be a regular file beside the executable.");
        }
        if (info.st_uid != geteuid() ||
            (protect_permissions && (info.st_mode & 07777) != 0600 && fchmod(file.get(), 0600) != 0)) {
            throw std::runtime_error("wave-db.json must be owned by the Wave user with mode 0600.");
        }
        for (;;) {
            const auto count = read(file.get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0) {
                throw std::runtime_error("Cannot read wave-db.json.");
            }
            if (count == 0) {
                break;
            }
            serialized.append(buffer, static_cast<std::size_t>(count));
        }
#endif
        auto contents = Json::parse(serialized, nullptr, false);
        if (!contents.is_object()) {
            throw std::runtime_error("wave-db.json must contain a JSON object.");
        }
        return contents;
    }

    void save(const Json& contents)
    {
        const auto serialized = contents.dump(2) + '\n';
        const auto temporary = ".wave-db-" + std::to_string(std::random_device{}());
#ifdef _WIN32
        const auto pending = path_.parent_path() / temporary;
        const auto file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, access_.attributes(), CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot stage wave-db.json.");
        }
        DWORD written = 0;
        const bool saved =
            serialized.size() <= MAXDWORD &&
            WriteFile(file, serialized.data(), static_cast<DWORD>(serialized.size()), &written, nullptr) &&
            written == serialized.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!saved || !MoveFileExW(pending.c_str(), path_.c_str(),
                                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(pending.c_str());
            throw std::runtime_error("Cannot commit wave-db.json.");
        }
#else
        const auto directory = directory_.get();
        const File file(openat(directory, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (file.get() < 0) {
            throw std::runtime_error("Cannot stage wave-db.json.");
        }
        try {
            if (fchmod(file.get(), 0600) != 0) {
                throw std::runtime_error("Cannot set wave-db.json permissions.");
            }
            std::string_view remaining = serialized;
            while (!remaining.empty()) {
                const auto count = write(file.get(), remaining.data(), remaining.size());
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    throw std::runtime_error("Cannot write wave-db.json.");
                }
                remaining.remove_prefix(static_cast<std::size_t>(count));
            }
            if (fsync(file.get()) != 0 || renameat(directory, temporary.c_str(), directory, database_name) != 0) {
                throw std::runtime_error("Cannot commit wave-db.json.");
            }
        } catch (...) {
            unlinkat(directory, temporary.c_str(), 0);
            throw;
        }
        if (fsync(directory) != 0) {
            throw DatabaseDirectorySyncError();
        }
#endif
    }

  private:
#ifdef _WIN32
    fs::path path_;
    PrivateAccess access_;
    HANDLE lock_ = nullptr;
#else
    File directory_;
#endif
};

std::string required_text(const Json& tls, const char* key)
{
    const auto field = tls.find(key);
    if (field == tls.end() || !field->is_string() || field->get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string("Enabled TLS requires a nonempty tls.") + key +
                                 " in wave-db.json.");
    }
    return field->get<std::string>();
}

std::optional<TlsSettings> load_tls(const Json& settings)
{
    const auto tls = settings.find("tls");
    if (tls == settings.end()) {
        return {};
    }
    if (!tls->is_object()) {
        throw std::runtime_error("tls in wave-db.json must be a JSON object.");
    }
    const auto enabled = tls->find("enabled");
    if (enabled == tls->end()) {
        return {};
    }
    if (!enabled->is_boolean()) {
        throw std::runtime_error("tls.enabled in wave-db.json must be true or false.");
    }
    if (!enabled->get<bool>()) {
        return {};
    }
    return TlsSettings{required_text(*tls, "domain"), required_text(*tls, "certificate_chain"),
                       required_text(*tls, "private_key")};
}

fs::path database_directory(const char* executable, const fs::path& site_dir)
{
    const auto directory = executable_path(executable).parent_path();
    if (site_dir.empty()) {
        return directory;
    }
    const auto path = directory / database_name;
    const auto relative = path.lexically_relative(fs::weakly_canonical(fs::absolute(site_dir)));
    if (!relative.empty() && *relative.begin() != "..") {
        throw std::runtime_error("Keep wave-db.json outside the site directory.");
    }
    return directory;
}

struct LoadedDatabase {
    Json contents;
    PanelSettings settings;
    int session_expiry_days;
    bool changed;
};

LoadedDatabase load_database(Json settings)
{
    const auto [admins, added_admins] = settings.emplace("admin", Json::array());
    if (!admins->is_array() || admins->size() > 1) {
        throw std::runtime_error("admin in wave-db.json must be an array "
                                 "containing at most one administrator.");
    }
    auto& listen = *settings.emplace("listen", Json::object()).first;
    if (!listen.is_object()) {
        throw std::runtime_error("listen in wave-db.json must be a JSON object.");
    }
    const auto [host, added_host] = listen.emplace("host", default_host);
    const auto [port, added_port] = listen.emplace("port", default_port);
    if (!host->is_string() || host->get_ref<const std::string&>().empty() ||
        std::ranges::any_of(host->get_ref<const std::string&>(),
                            [](unsigned char ch) { return ch <= 0x20 || ch == 0x7f; })) {
        throw std::runtime_error("listen.host in wave-db.json must be a nonempty "
                                 "address without spaces.");
    }
    if (!port->is_number_integer() || port->get<std::uint64_t>() > 65535) {
        throw std::runtime_error("listen.port in wave-db.json must be an integer from 0 to 65535.");
    }
    auto session_expiry_days = default_session_expiry_days;
    if (const auto expiry = settings.find("session_expiry_days"); expiry != settings.end()) {
        if (!expiry->is_number_integer() || expiry->get<std::uint64_t>() < 1 ||
            expiry->get<std::uint64_t>() > max_session_expiry_days) {
            throw std::runtime_error("session_expiry_days in wave-db.json must be an integer from 1 to " +
                                     std::to_string(max_session_expiry_days) + ".");
        }
        session_expiry_days = expiry->get<int>();
    }
    PanelSettings listener{host->get<std::string>(), port->get<int>(), load_tls(settings)};
    return {std::move(settings), std::move(listener), session_expiry_days, added_admins || added_host || added_port};
}

bool has_exact_keys(const Json& object, std::initializer_list<std::string_view> keys)
{
    return object.is_object() && object.size() == keys.size() &&
           std::ranges::all_of(keys, [&](auto key) { return object.contains(key); });
}

bool donation_backup_record(const Json& donation)
{
    return has_exact_keys(donation, {"answer", "answered_at"}) && donation.at("answer").is_string() &&
           donation.at("answered_at").is_string();
}

bool tls_backup_text(const Json& value)
{
    return value.is_string() && std::ranges::none_of(value.get_ref<const std::string&>(),
                                                    [](unsigned char ch) { return ch < 0x20 || ch == 0x7f; });
}

} // namespace

struct PanelDatabase::Impl {
    Impl(const char* executable, const fs::path& document_root, bool check_only)
        : directory(database_directory(executable, document_root)), site_dir(document_root), file(directory)
    {
        if (!check_only) {
            file.lock();
        }
        auto loaded = load_database(file.read_settings(!check_only).value_or(Json::object()));
        if (const auto path = loaded.contents.find(panel_base_path_key); path != loaded.contents.end()) {
            if (!path->is_string() || !valid_panel_base_path(path->get_ref<const std::string&>())) {
                throw std::runtime_error("Invalid panel-base-path in wave-db.json.");
            }
            base_path = path->get<std::string>();
        }
        if (loaded.changed && !check_only) {
            save(loaded.contents);
        }
        contents = std::move(loaded.contents);
        settings = std::move(loaded.settings);
        session_expiry_days = loaded.session_expiry_days;
    }

    Json read_existing() const
    {
        auto current = file.read_settings();
        if (!current) {
            throw std::runtime_error("Cannot find wave-db.json.");
        }
        return std::move(*current);
    }

    void save(const Json& next)
    {
        try {
            file.save(next);
        } catch (const DatabaseDirectorySyncError& error) {
            // The replacement is already visible. Do not roll back its associated config files.
            std::cerr << error.what() << '\n';
        }
    }

    void save_fields(std::initializer_list<std::pair<std::string_view, std::reference_wrapper<const Json>>> fields)
    {
        std::lock_guard lock(write_mutex);
        auto current = read_existing();
        for (const auto& [key, value] : fields) {
            current[key] = value.get();
        }
        save(current);
    }

    void publish_listener() const
    {
        if (listener_observer) {
            listener_observer(settings, base_path);
        }
    }

    fs::path directory;
    fs::path site_dir;
    DatabaseFile file;
    std::mutex write_mutex;
    Json contents;
    PanelSettings settings{};
    std::string base_path;
    std::function<void(const PanelSettings&, std::string_view)> listener_observer;
    int session_expiry_days = default_session_expiry_days;
    bool listener_restart_pending = false;
};

PanelDatabase::PanelDatabase(const char* executable, const fs::path& site_dir, bool check_only)
    : impl_(std::make_unique<Impl>(executable, site_dir, check_only))
{
}

PanelDatabase::~PanelDatabase() = default;

const fs::path& PanelDatabase::directory() const
{
    return impl_->directory;
}

PanelSettings PanelDatabase::settings() const
{
    std::lock_guard lock(impl_->write_mutex);
    return impl_->settings;
}

const Json& PanelDatabase::initial_contents() const
{
    return impl_->contents;
}

std::string PanelDatabase::panel_base_path() const
{
    std::lock_guard lock(impl_->write_mutex);
    return impl_->base_path;
}

void PanelDatabase::observe_listener(std::function<void(const PanelSettings&, std::string_view)> publish)
{
    std::lock_guard lock(impl_->write_mutex);
    impl_->listener_observer = std::move(publish);
    impl_->publish_listener();
}

bool PanelDatabase::save_panel_base_path(std::string_view previous, std::string_view path)
{
    if (!valid_panel_base_path(path)) {
        throw std::invalid_argument("Invalid panel base path.");
    }
    std::string next(path);
    std::lock_guard lock(impl_->write_mutex);
    if (impl_->listener_restart_pending || impl_->base_path != previous) {
        return false;
    }
    auto current = impl_->read_existing();
    const auto saved = current.find(panel_base_path_key);
    if (previous.empty() ? saved != current.end() : saved == current.end() || *saved != previous) {
        return false;
    }
    if (previous == path) {
        return true;
    }
    current[panel_base_path_key] = next;
    impl_->save(current);
    impl_->base_path = std::move(next);
    impl_->publish_listener();
    return true;
}

Json PanelDatabase::save_tls_settings(const std::optional<TlsSettings>& tls,
                                      std::unique_ptr<httplib::Server>* activation)
{
    std::lock_guard lock(impl_->write_mutex);
    auto current = impl_->read_existing();
    std::unique_ptr<httplib::Server> validated;
    if (tls) {
#ifdef WAVE_ENABLE_AUTH
        if (activation) {
            validate_tls_settings(*tls, impl_->settings.listen_port);
        }
        validated = make_tls_server(*tls, impl_->site_dir, current.at("listen").at("port").get<int>());
#else
        throw std::runtime_error("This Wave build does not support HTTPS.");
#endif
    }

    auto& saved = current["tls"];
    if (!saved.is_object()) {
        saved = Json::object();
    }
    saved["enabled"] = tls.has_value();
    if (tls) {
        saved["domain"] = tls->domain;
        saved["certificate_chain"] = tls->certificate_chain.string();
        saved["private_key"] = tls->private_key.string();
    }
    impl_->save(current);
    if (activation) {
        *activation = std::move(validated);
        // The acknowledgement contains the current base path. Keep it stable through the switch.
        impl_->listener_restart_pending = static_cast<bool>(*activation);
    }
    return std::move(saved);
}

void PanelDatabase::begin_listener_restart()
{
    std::lock_guard lock(impl_->write_mutex);
    impl_->listener_restart_pending = true;
}

void PanelDatabase::finish_listener_restart(std::optional<PanelSettings> settings)
{
    std::lock_guard lock(impl_->write_mutex);
    if (settings) {
        impl_->settings = std::move(*settings);
    }
    impl_->listener_restart_pending = false;
}

std::unique_ptr<httplib::Server> PanelDatabase::make_listener(const PanelSettings& settings) const
{
    if (!settings.tls) {
        return std::make_unique<httplib::Server>();
    }
#ifdef WAVE_ENABLE_AUTH
    return make_tls_server(*settings.tls, impl_->site_dir, settings.listen_port);
#else
    throw std::runtime_error("TLS is enabled in wave-db.json. Build with WAVE_ENABLE_AUTH=ON to use it.");
#endif
}

int PanelDatabase::session_expiry_days() const
{
    std::lock_guard lock(impl_->write_mutex);
    return impl_->session_expiry_days;
}

void PanelDatabase::save_session_expiry_days(int days)
{
    std::lock_guard lock(impl_->write_mutex);
    auto current = impl_->read_existing();
    current["session_expiry_days"] = days;
    impl_->save(current);
    impl_->session_expiry_days = days;
}

void PanelDatabase::save_theme(theme::Preset preset)
{
    const Json value = theme::preset_key(preset);
    impl_->save_fields({{"theme", value}});
}

void PanelDatabase::save_donation(std::string_view answer, std::string_view answered_at)
{
    const Json value = {{"answer", answer}, {"answered_at", answered_at}};
    impl_->save_fields({{"donation", value}});
}

bool PanelDatabase::claim_waterwall_install()
{
    std::lock_guard lock(impl_->write_mutex);
    auto current = impl_->read_existing();
    const auto attempted = current.find("waterwall_install_attempted");
    if (attempted != current.end() && *attempted == true) {
        return false;
    }
    current["waterwall_install_attempted"] = true;
    impl_->save(current);
    return true;
}

void PanelDatabase::save_waterwall_version(std::string_view version)
{
    if (!valid_waterwall_version(version)) {
        throw std::invalid_argument("Invalid WaterWall version.");
    }
    const Json value = version;
    impl_->save_fields({{"waterwall_version", value}});
}

void PanelDatabase::save_auth(const Json& admins, const Json& auth)
{
    std::lock_guard lock(impl_->write_mutex);
    auto current = impl_->read_existing();
    current["admin"] = admins;
    current["auth"] = auth;
    current.erase(setup_token_key);
    impl_->save(current);
}

void PanelDatabase::save_core_settings(const Json& settings)
{
    impl_->save_fields({{"core_settings", settings}});
}

void PanelDatabase::save_core_configs(const Json& configs)
{
    impl_->save_fields({{"core_configs", configs}});
}

Json PanelDatabase::capture_panel_backup() const
{
    std::lock_guard lock(impl_->write_mutex);
    const auto saved = impl_->read_existing();
    const auto& admin = saved.at("admin").at(0);
    const auto& listen = saved.at("listen");
    const auto saved_tls = saved.value("tls", Json::object());
    Json tls = {{"enabled", saved_tls.value("enabled", false)}};
    for (const auto key : tls_backup_text_keys) {
        const auto field = saved_tls.find(key);
        if (field != saved_tls.end() && tls_backup_text(*field)) {
            tls[key] = *field;
        }
    }
    const auto saved_theme = saved.find("theme");
    const auto preset = saved_theme != saved.end() && saved_theme->is_string()
                            ? theme::parse_preset(saved_theme->get_ref<const std::string&>())
                            : std::nullopt;
    Json donation;
    if (const auto item = saved.find("donation"); item != saved.end() && item->is_object()) {
        const auto answer = item->find("answer");
        const auto time = item->find("answered_at");
        if (answer != item->end() && answer->is_string() && time != item->end() && time->is_string()) {
            donation = {{"answer", *answer}, {"answered_at", *time}};
        }
    }
    return {{"admin", Json::array({{{"username", admin.at("username")}, {"password", admin.at("password")}}})},
            {panel_base_path_key, saved.at(panel_base_path_key)},
            {"listen", {{"host", listen.at("host")}, {"port", listen.at("port")}}},
            {"tls", std::move(tls)},
            {"session_expiry_days", saved.value("session_expiry_days", default_session_expiry_days)},
            {"theme", theme::preset_key(preset.value_or(theme::Preset::standard))},
            {"donation", std::move(donation)}};
}

void PanelDatabase::validate_panel_backup(const Json& panel) const
{
    if (!panel.is_object() || panel.size() != panel_backup_keys.size() ||
        !std::ranges::all_of(panel_backup_keys, [&](auto key) { return panel.contains(key); })) {
        throw std::runtime_error("Wave panel settings in the backup are invalid.");
    }
    const auto& path = panel.at(panel_base_path_key);
    if (!path.is_string() || !valid_panel_base_path(path.get_ref<const std::string&>())) {
        throw std::runtime_error("The backup contains an invalid panel base path.");
    }
    const auto& admins = panel.at("admin");
    if (!admins.is_array() || admins.size() != 1 ||
        !has_exact_keys(admins.front(), {"username", "password"}) || !admins.front().at("username").is_string() ||
        !admins.front().at("password").is_string()) {
        throw std::runtime_error("The backup must contain one Wave administrator with a username and password hash.");
    }
    if (!has_exact_keys(panel.at("listen"), {"host", "port"})) {
        throw std::runtime_error("The backup contains invalid Wave listener settings.");
    }
    const auto& tls = panel.at("tls");
    if (!tls.is_object() || !tls.contains("enabled") || !tls.at("enabled").is_boolean()) {
        throw std::runtime_error("The backup contains invalid Wave TLS settings.");
    }
    std::size_t tls_field_count = 1;
    for (const auto key : tls_backup_text_keys) {
        const auto field = tls.find(key);
        if (field != tls.end() && tls_backup_text(*field)) {
            ++tls_field_count;
        }
    }
    if (tls.size() != tls_field_count) {
        throw std::runtime_error("The backup contains invalid Wave TLS settings.");
    }
    const auto& preset = panel.at("theme");
    if (!preset.is_string() || !theme::parse_preset(preset.get_ref<const std::string&>())) {
        throw std::runtime_error("The backup contains an invalid Wave theme.");
    }
    const auto& donation = panel.at("donation");
    if (!donation.is_null() && !donation_backup_record(donation)) {
        throw std::runtime_error("The backup contains an invalid donation response.");
    }
    const auto saved = load_database(panel);
    if (saved.settings.tls) {
#ifdef WAVE_ENABLE_AUTH
        validate_tls_settings(*saved.settings.tls, saved.settings.listen_port);
#else
        throw std::runtime_error("This Wave build does not support the backup's HTTPS settings.");
#endif
    }
}

PanelSettings PanelDatabase::panel_backup_listener_settings(const Json& panel) const
{
    const auto& listen = panel.at("listen");
    return {listen.at("host").get<std::string>(), listen.at("port").get<int>(), load_tls(panel)};
}

std::string PanelDatabase::panel_backup_tls_error(const Json& panel) const
{
    try {
        const auto settings = panel_backup_listener_settings(panel);
        if (settings.tls) {
            make_listener(settings);
        }
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

void PanelDatabase::save_workspace_restore(const Json& manifest, const Json& restore_id, const Json& auth_state)
{
    std::lock_guard lock(impl_->write_mutex);
    auto current = impl_->read_existing();
    current["waterwall_version"] = manifest.at("waterwall_version");
    current["core_settings"] = manifest.at("core_settings");
    current["core_configs"] = manifest.at("configs");
    current["workspace_restore_id"] = restore_id;
    const auto panel = manifest.find("panel_settings");
    if (panel == manifest.end()) {
        impl_->save(current);
        return;
    }
    if (!auth_state.is_object()) {
        throw std::invalid_argument("The restored administrator requires fresh authentication state.");
    }
    auto path = panel->at(panel_base_path_key).get<std::string>();
    const int expiry = panel->at("session_expiry_days").get<int>();
    for (const auto key : panel_backup_keys) {
        current[key] = panel->at(key);
    }
    if (current.at("donation").is_null()) {
        current.erase("donation");
    }
    current["auth"] = auth_state;
    impl_->save(current);
    impl_->base_path = std::move(path);
    impl_->session_expiry_days = expiry;
    impl_->publish_listener();
}

} // namespace wave
