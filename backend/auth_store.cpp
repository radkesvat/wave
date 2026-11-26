#include "auth_store.h"

#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr char state_file[] = "admin.json";
constexpr char bootstrap_file[] = "bootstrap-token";
constexpr std::size_t max_sessions = 16;
constexpr std::size_t secret_size = 64;
constexpr std::size_t session_id_size = 32;

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

std::string hex(const unsigned char* data, std::size_t size)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(2 * size);
    for (std::size_t i = 0; i < size; ++i) {
        result += digits[data[i] >> 4];
        result += digits[data[i] & 15];
    }
    return result;
}

std::string random_hex(std::size_t byte_count)
{
    std::array<unsigned char, 32> buffer{};
    if (RAND_bytes(buffer.data(), static_cast<int>(byte_count)) != 1) {
        throw std::runtime_error("Cannot generate authentication secrets.");
    }
    return hex(buffer.data(), byte_count);
}

bool is_hex(std::string_view value, std::size_t size)
{
    return value.size() == size && value.find_first_not_of("0123456789abcdef") == value.npos;
}

bool secret_equal(std::string_view left, std::string_view right)
{
    return left.size() == right.size() && CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

class PrivateDirectory {
  public:
    explicit PrivateDirectory(const std::filesystem::path& path)
        : directory_(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC))
    {
        struct stat info{};
        if (directory_.get() < 0 || fstat(directory_.get(), &info) != 0 || info.st_uid != geteuid() ||
            (info.st_mode & 07777) != 0700) {
            throw std::runtime_error(
                "The authentication directory must be owned by Wave's user with mode 0700.");
        }
    }

    void lock_authentication_state() const
    {
        if (flock(directory_.get(), LOCK_EX | LOCK_NB) != 0) {
            throw std::runtime_error("Cannot lock the authentication directory for exclusive use.");
        }
    }

    std::optional<std::string> read_file(const char* name, std::size_t limit) const
    {
        const File file(openat(directory_.get(), name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
        if (file.get() < 0 && errno == ENOENT) {
            return std::nullopt;
        }
        struct stat info{};
        if (file.get() < 0 || fstat(file.get(), &info) != 0 || !S_ISREG(info.st_mode) ||
            info.st_uid != geteuid() || (info.st_mode & 07777) != 0600 || info.st_nlink != 1) {
            throw std::runtime_error(
                "Authentication files must be regular files owned by Wave's user with mode 0600.");
        }

        std::string contents;
        char buffer[4096];
        for (;;) {
            const auto count = read(file.get(), buffer, sizeof(buffer));
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count < 0 || contents.size() + static_cast<std::size_t>(count) > limit) {
                throw std::runtime_error("Cannot read authentication state.");
            }
            if (count == 0) {
                return contents;
            }
            contents.append(buffer, static_cast<std::size_t>(count));
        }
    }

    void replace_file(const char* name, const char* temporary_prefix, std::string_view contents) const
    {
        const int dir = directory_.get();
        const auto temporary = temporary_prefix + random_hex(16);
        const File file(openat(dir, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600));
        if (file.get() < 0) {
            throw std::runtime_error("Cannot create private authentication state.");
        }
        try {
            if (fchmod(file.get(), 0600) != 0) {
                throw std::runtime_error("Cannot set authentication state permissions.");
            }
            while (!contents.empty()) {
                const auto count = write(file.get(), contents.data(), contents.size());
                if (count < 0 && errno == EINTR) {
                    continue;
                }
                if (count <= 0) {
                    throw std::runtime_error("Cannot write authentication state.");
                }
                contents.remove_prefix(static_cast<std::size_t>(count));
            }
            if (fsync(file.get()) != 0 || renameat(dir, temporary.c_str(), dir, name) != 0 ||
                fsync(dir) != 0) {
                throw std::runtime_error("Cannot commit authentication state.");
            }
        } catch (...) {
            unlinkat(dir, temporary.c_str(), 0);
            throw;
        }
    }

    void retire_bootstrap() const
    {
        if (unlinkat(directory_.get(), bootstrap_file, 0) == 0) {
            if (fsync(directory_.get()) == 0) {
                return;
            }
        } else if (errno == ENOENT) {
            return;
        }
        throw std::runtime_error("Cannot retire the bootstrap-token file.");
    }

  private:
    File directory_;
};

std::int64_t unix_time()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string hash_password(std::string_view password, std::string_view salt)
{
    constexpr std::uint64_t cost = 1 << 17;
    constexpr std::uint64_t memory_limit = 160 * 1024 * 1024;
    std::array<unsigned char, 32> hash{};
    if (EVP_PBE_scrypt(password.data(), password.size(), reinterpret_cast<const unsigned char*>(salt.data()),
                       salt.size(), cost, 8, 1, memory_limit, hash.data(), hash.size()) != 1) {
        throw std::runtime_error("Password hashing failed.");
    }
    return hex(hash.data(), hash.size());
}

std::string base64url(std::string_view value)
{
    std::string result(4 * ((value.size() + 2) / 3) + 1, '\0');
    const auto size =
        EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()),
                        reinterpret_cast<const unsigned char*>(value.data()), static_cast<int>(value.size()));
    result.resize(static_cast<std::size_t>(size));
    std::ranges::replace(result, '+', '-');
    std::ranges::replace(result, '/', '_');
    result.erase(result.find_last_not_of('=') + 1);
    return result;
}

std::string make_jwt(std::string_view key, std::string_view username, std::string_view id,
                     std::int64_t expiry)
{
    const Json claims{{"exp", expiry}, {"iss", "wave"}, {"jti", id}, {"sub", username}};
    auto token = base64url(R"({"alg":"HS256","typ":"JWT"})") + '.' + base64url(claims.dump());
    std::array<unsigned char, EVP_MAX_MD_SIZE> signature{};
    unsigned int size = 0;
    if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
              reinterpret_cast<const unsigned char*>(token.data()), token.size(), signature.data(), &size)) {
        throw std::runtime_error("Cannot sign authentication session.");
    }
    token += '.' + base64url({reinterpret_cast<const char*>(signature.data()), size});
    return token;
}

std::string text(const Json& value, const char* key)
{
    const auto item = value.find(key);
    return item != value.end() && item->is_string() ? item->get<std::string>() : std::string{};
}

std::int64_t number(const Json& value, const char* key)
{
    const auto item = value.find(key);
    return item != value.end() && item->is_number_integer() ? item->get<std::int64_t>() : 0;
}

struct SessionRecord {
    std::string id;
    std::int64_t expires_at;
    std::string token;
};

struct Admin {
    std::string username;
    std::string salt;
    std::string password_hash;
    std::string jwt_key;
    std::int64_t acknowledged_at;
    std::vector<SessionRecord> sessions;

    SessionRecord record(std::string id, std::int64_t expiry) const
    {
        auto token = make_jwt(jwt_key, username, id, expiry);
        return {std::move(id), expiry, std::move(token)};
    }

    auto find_session(std::string_view token) const
    {
        const auto now = unix_time();
        // Accept only an exact match to a JWT signed from a persisted, unexpired session record.
        return std::ranges::find_if(sessions, [&](const auto& record) {
            return record.expires_at > now && secret_equal(record.token, token);
        });
    }

    std::string dump() const
    {
        auto records = Json::array();
        for (const auto& entry : sessions) {
            records.push_back({{"id", entry.id}, {"expires_at", entry.expires_at}});
        }
        return Json{{"version", 1},
                    {"username", username},
                    {"password", {{"algorithm", "scrypt"}, {"salt", salt}, {"hash", password_hash}}},
                    {"acknowledgments", {{"version", 1}, {"accepted_at", acknowledged_at}}},
                    {"jwt_key", jwt_key},
                    {"sessions", std::move(records)}}
            .dump();
    }
};

Admin load_admin(std::string_view contents)
{
    const auto state = Json::parse(contents, nullptr, false);
    const auto invalid = [] { throw std::runtime_error("Stored authentication state is invalid."); };
    if (number(state, "version") != 1 || !state.contains("password") || !state.contains("acknowledgments") ||
        !state.contains("sessions")) {
        invalid();
    }
    const auto& password = state["password"];
    const auto& acknowledgments = state["acknowledgments"];
    const auto& sessions = state["sessions"];
    Admin admin{text(state, "username"),
                text(password, "salt"),
                text(password, "hash"),
                text(state, "jwt_key"),
                number(acknowledgments, "accepted_at"),
                {}};
    if (!AuthStore::valid_username(admin.username) || text(password, "algorithm") != "scrypt" ||
        !is_hex(admin.salt, secret_size) || !is_hex(admin.password_hash, secret_size) ||
        !is_hex(admin.jwt_key, secret_size) || number(acknowledgments, "version") != 1 ||
        admin.acknowledged_at <= 0 || !sessions.is_array() || sessions.size() > max_sessions) {
        invalid();
    }
    for (const auto& entry : sessions) {
        auto id = text(entry, "id");
        const auto expiry = number(entry, "expires_at");
        if (!is_hex(id, session_id_size) || expiry <= 0 ||
            std::ranges::find(admin.sessions, id, &SessionRecord::id) != admin.sessions.end()) {
            invalid();
        }
        admin.sessions.push_back(admin.record(std::move(id), expiry));
    }
    return admin;
}

} // namespace

struct AuthStore::Impl {
    explicit Impl(const std::filesystem::path& path) : directory(path)
    {
        directory.lock_authentication_state();
        if (const auto existing = directory.read_file(state_file, 16 * 1024)) {
            admin.emplace(load_admin(*existing));
            directory.retire_bootstrap();
            return;
        }
        auto token = directory.read_file(bootstrap_file, secret_size + 1);
        if (token && token->ends_with('\n')) {
            token->pop_back();
        }
        if (!token || !is_hex(*token, secret_size)) {
            throw std::runtime_error(
                "First-run setup requires a private bootstrap-token file with 64 lowercase hex digits.");
        }
        bootstrap.swap(*token);
    }

    void check_available() const
    {
        if (storage_failed) {
            throw std::runtime_error("Authentication storage is unavailable.");
        }
    }

    void commit(Admin next)
    {
        try {
            directory.replace_file(state_file, ".admin-", next.dump());
        } catch (...) {
            storage_failed = true;
            throw;
        }
        admin = std::move(next);
    }

    PrivateDirectory directory;
    mutable std::mutex mutex;
    bool storage_failed = false;
    std::optional<Admin> admin;
    std::string bootstrap;
    std::mutex sign_in_mutex;
    std::chrono::steady_clock::time_point retry_after{};
};

AuthStore::AuthStore(const std::filesystem::path& directory) : impl_(std::make_unique<Impl>(directory))
{
}

AuthStore::~AuthStore() = default;

bool AuthStore::installed() const
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    return impl_->admin.has_value();
}

bool AuthStore::valid_username(std::string_view username)
{
    return !username.empty() && username.size() <= 64 &&
           username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") ==
               username.npos;
}

bool AuthStore::valid_password(std::string_view password)
{
    if (password.size() > 256 || password.find_first_not_of(' ') == password.npos) {
        return false;
    }
    if (std::ranges::any_of(password, [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; })) {
        return false;
    }
    return std::ranges::count_if(password, [](unsigned char byte) { return (byte & 0xc0) != 0x80; }) >= 15;
}

AuthStore::SetupResult AuthStore::setup(std::string_view username, std::string_view password,
                                        std::string_view bootstrap)
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if (impl_->admin) {
        return SetupResult::closed;
    }
    if (!secret_equal(impl_->bootstrap, bootstrap)) {
        return SetupResult::forbidden;
    }
    auto salt = random_hex(32);
    auto hash = hash_password(password, salt);
    impl_->commit(
        Admin{std::string(username), std::move(salt), std::move(hash), random_hex(32), unix_time(), {}});
    OPENSSL_cleanse(impl_->bootstrap.data(), impl_->bootstrap.size());
    impl_->bootstrap.clear();
    impl_->directory.retire_bootstrap();
    return SetupResult::created;
}

AuthStore::SignInResult AuthStore::sign_in(std::string_view username, std::string_view password,
                                           std::string& token)
{
    // Allow one memory-hard password check at a time and back off after a failed attempt.
    std::unique_lock attempt(impl_->sign_in_mutex, std::try_to_lock);
    if (!attempt || std::chrono::steady_clock::now() < impl_->retry_after) {
        return SignInResult::busy;
    }
    std::string stored_username;
    std::string salt;
    std::string stored_hash;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_available();
        if (!impl_->admin) {
            return SignInResult::invalid;
        }
        stored_username = impl_->admin->username;
        salt = impl_->admin->salt;
        stored_hash = impl_->admin->password_hash;
    }
    if (!secret_equal(hash_password(password, salt), stored_hash) || username != stored_username) {
        impl_->retry_after = std::chrono::steady_clock::now() + 1s;
        return SignInResult::invalid;
    }

    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    const auto now = unix_time();
    auto next = *impl_->admin;
    std::erase_if(next.sessions, [now](const auto& record) { return record.expires_at <= now; });
    if (next.sessions.size() == max_sessions) {
        next.sessions.erase(next.sessions.begin());
    }
    next.sessions.push_back(next.record(random_hex(16), now + session_seconds));
    token = next.sessions.back().token;
    impl_->commit(std::move(next));
    return SignInResult::accepted;
}

std::optional<AuthStore::Session> AuthStore::session(std::string_view token) const
{
    if (token.empty()) {
        return std::nullopt;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->storage_failed || !impl_->admin) {
        return std::nullopt;
    }
    const auto match = impl_->admin->find_session(token);
    if (match == impl_->admin->sessions.end()) {
        return std::nullopt;
    }
    return Session{impl_->admin->username, match->expires_at};
}

void AuthStore::sign_out(std::string_view token)
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if (!impl_->admin || token.empty()) {
        return;
    }
    const auto match = impl_->admin->find_session(token);
    if (match != impl_->admin->sessions.end()) {
        auto next = *impl_->admin;
        next.sessions.erase(next.sessions.begin() + (match - impl_->admin->sessions.begin()));
        impl_->commit(std::move(next));
    }
}

} // namespace wave
