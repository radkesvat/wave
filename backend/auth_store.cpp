#include "auth_store.h"

#include "panel_settings.h"

#include <argon2.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <zxcvbn.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace wave {
namespace {

using Json = nlohmann::json;
using namespace std::chrono_literals;

constexpr std::size_t max_sessions = 16;
constexpr std::size_t secret_size = 64;
constexpr std::size_t session_id_size = 32;
constexpr double minimum_password_strength_bits = 20.0;

bool valid_new_password_shape(std::string_view password)
{
    if (!AuthStore::valid_password_input(password)) {
        return false;
    }
    // The JSON parser validates UTF-8. Each byte outside the continuation
    // range starts one code point.
    const auto characters = std::ranges::count_if(password, [](unsigned char byte) {
        return (byte & 0xc0) != 0x80;
    });
    return characters >= 6 &&
           password.find_first_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ") != password.npos &&
           password.find_first_of("0123456789") != password.npos;
}

bool strong_password(std::string_view username, std::string_view password)
{
    const std::string account(username);
    const std::string candidate(password);
    const char* dictionary[] = {account.c_str(), "wave", "waterwall", nullptr};
    return ZxcvbnMatch(candidate.c_str(), dictionary, nullptr) >= minimum_password_strength_bits;
}

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

std::int64_t unix_time()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string hash_password(std::string_view password)
{
    constexpr std::uint32_t memory_kib = 64 * 1024;
    constexpr std::uint32_t passes = 3;
    constexpr std::uint32_t lanes = 4;
    constexpr std::uint32_t hash_bytes = 32;
    std::array<unsigned char, 16> salt{};
    if (RAND_bytes(salt.data(), salt.size()) != 1) {
        throw std::runtime_error("Cannot generate a password salt.");
    }
    std::string encoded(argon2_encodedlen(passes, memory_kib, lanes, salt.size(), hash_bytes, Argon2_id),
                        '\0');
    if (argon2id_hash_encoded(passes, memory_kib, lanes, password.data(), password.size(), salt.data(),
                              salt.size(), hash_bytes, encoded.data(), encoded.size()) != ARGON2_OK) {
        throw std::runtime_error("Cannot hash the administrator password.");
    }
    encoded.resize(encoded.find('\0'));
    return encoded;
}

bool valid_password_hash(std::string_view encoded)
{
    constexpr std::string_view prefix = "$argon2id$v=19$m=";
    if (!encoded.starts_with(prefix)) {
        return false;
    }
    encoded.remove_prefix(prefix.size());
    const auto read_parameter = [&](std::string_view delimiter) {
        const auto separator = encoded.find(delimiter);
        if (separator == encoded.npos || separator == 0 || encoded.front() == '0') {
            return 0u;
        }
        unsigned int value = 0;
        const auto end = encoded.data() + separator;
        const auto parsed = std::from_chars(encoded.data(), end, value);
        encoded.remove_prefix(separator + delimiter.size());
        return parsed.ec == std::errc{} && parsed.ptr == end ? value : 0u;
    };
    const auto memory = read_parameter(",t=");
    const auto passes = read_parameter(",p=");
    const auto lanes = read_parameter("$");
    if (lanes == 0 || lanes > 16 || memory < 8 * lanes || memory > 256 * 1024 || passes == 0 || passes > 10) {
        return false;
    }
    const auto separator = encoded.find('$');
    if (separator == encoded.npos) {
        return false;
    }
    const auto salt = encoded.substr(0, separator);
    const auto digest = encoded.substr(separator + 1);
    const auto base64 = [](std::string_view value, std::size_t minimum) {
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        if (value.size() < minimum || value.size() > 86 || value.find_first_not_of(alphabet) != value.npos) {
            return false;
        }
        const auto tail = alphabet.find(value.back());
        switch (value.size() % 4) {
        case 1:
            return false;
        case 2:
            return tail % 16 == 0;
        case 3:
            return tail % 4 == 0;
        default:
            return true;
        }
    };
    return base64(salt, 11) && base64(digest, 22);
}

bool verify_password(std::string_view password, const std::string& hash)
{
    return argon2id_verify(hash.c_str(), password.data(), password.size()) == ARGON2_OK;
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
    std::string password_hash;
    std::string jwt_key;
    std::vector<SessionRecord> sessions;

    SessionRecord record(std::string id, std::int64_t expiry) const
    {
        auto token = make_jwt(jwt_key, username, id, expiry);
        return {std::move(id), expiry, std::move(token)};
    }

    auto find_session(std::string_view token) const
    {
        const auto now = unix_time();
        // Accept only an exact match to a JWT signed from a persisted, unexpired
        // session record.
        return std::ranges::find_if(sessions, [&](const auto& record) {
            return record.expires_at > now && secret_equal(record.token, token);
        });
    }

    Json credential() const
    {
        return {{"username", username}, {"password", password_hash}};
    }

    Json state() const
    {
        auto records = Json::array();
        for (const auto& entry : sessions) {
            records.push_back({{"id", entry.id}, {"expires_at", entry.expires_at}});
        }
        return {{"jwt_key", jwt_key}, {"sessions", std::move(records)}};
    }
};

Admin read_admin_identity(const Json& entry, const char* error)
{
    Admin admin{text(entry, "username"), text(entry, "password"), {}, {}};
    if (!AuthStore::valid_username(admin.username) || !valid_password_hash(admin.password_hash)) {
        throw std::runtime_error(error);
    }
    return admin;
}

Admin read_backup_admin(const Json& admins)
{
    constexpr auto error =
        "A Wave panel backup requires one administrator with a username and an Argon2id password hash.";
    if (!admins.is_array() || admins.size() != 1 || !admins.front().is_object() || admins.front().size() != 2) {
        throw std::runtime_error(error);
    }
    return read_admin_identity(admins.front(), error);
}

std::optional<Admin> load_admin(const Json& database)
{
    const auto& admins = database.at("admin");
    if (admins.empty()) {
        return std::nullopt;
    }
    auto admin = read_admin_identity(admins.front(), "An administrator in wave-db.json requires a "
                                                   "username and an Argon2id password hash.");
    const auto state = database.find("auth");
    if (state == database.end()) {
        admin.jwt_key = random_hex(32);
        return admin;
    }
    const auto invalid = [] { throw std::runtime_error("Authentication state in wave-db.json is invalid."); };
    const auto sessions = state->find("sessions");
    admin.jwt_key = text(*state, "jwt_key");
    if (!is_hex(admin.jwt_key, secret_size) || sessions == state->end() || !sessions->is_array() ||
        sessions->size() > max_sessions) {
        invalid();
    }
    for (const auto& record : *sessions) {
        auto id = text(record, "id");
        const auto expiry = number(record, "expires_at");
        if (!is_hex(id, session_id_size) || expiry <= 0 ||
            std::ranges::find(admin.sessions, id, &SessionRecord::id) != admin.sessions.end()) {
            invalid();
        }
        admin.sessions.push_back(admin.record(std::move(id), expiry));
    }
    return admin;
}

std::string load_setup_token(const Json& database)
{
    const auto value = database.find(setup_token_key);
    if (value == database.end()) {
        return {};
    }
    const auto* token = value->get_ptr<const std::string*>();
    if (!token || !is_hex(*token, secret_size)) {
        throw std::runtime_error("Invalid setup_token in wave-db.json.");
    }
    return *token;
}

struct RestoredPanelPath {
    struct RevokedSession {
        std::string id;
        std::int64_t expires_at;
    };
    std::string path;
    std::vector<RevokedSession> sessions;
    std::chrono::steady_clock::time_point expires_at;
};

} // namespace

struct AuthStore::Impl {
    explicit Impl(std::shared_ptr<PanelDatabase> input)
        : database(std::move(input)), admin(load_admin(database->initial_contents())),
          setup_token(load_setup_token(database->initial_contents()))
    {
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
            database->save_auth(Json::array({next.credential()}), next.state());
        } catch (...) {
            storage_failed = true;
            throw;
        }
        admin = std::move(next);
        setup_token.clear();
    }

    std::shared_ptr<PanelDatabase> database;
    mutable std::mutex mutex;
    bool storage_failed = false;
    std::optional<Admin> admin;
    std::string setup_token;
    std::optional<RestoredPanelPath> restored_panel_path;
    std::mutex sign_in_mutex;
    std::chrono::steady_clock::time_point retry_after{};
};

AuthStore::AuthStore(std::shared_ptr<PanelDatabase> database)
    : impl_(std::make_unique<Impl>(std::move(database)))
{
}

AuthStore::~AuthStore() = default;

bool AuthStore::installed() const
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    return impl_->admin.has_value();
}

bool AuthStore::setup_token_required() const
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    return !impl_->admin && !impl_->setup_token.empty();
}

bool AuthStore::valid_username(std::string_view username)
{
    return !username.empty() && username.size() <= 64 &&
           username.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOP"
                                      "QRSTUVWXYZ0123456789._-") == username.npos;
}

bool AuthStore::valid_password_input(std::string_view password)
{
    if (password.size() > 256 || password.find_first_not_of(' ') == password.npos) {
        return false;
    }
    return std::ranges::none_of(password, [](unsigned char byte) { return byte < 0x20 || byte == 0x7f; });
}

AuthStore::SetupResult AuthStore::setup(std::string_view username, std::string_view password,
                                       std::string_view setup_token)
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if (impl_->admin) {
        return SetupResult::closed;
    }
    if (!impl_->setup_token.empty() && !secret_equal(impl_->setup_token, setup_token)) {
        return SetupResult::forbidden;
    }
    if (!valid_new_password_shape(password)) {
        return SetupResult::invalid_password;
    }
    if (!strong_password(username, password)) {
        return SetupResult::weak_password;
    }
    impl_->commit(Admin{std::string(username), hash_password(password), random_hex(32), {}});
    return SetupResult::created;
}

AuthStore::SignInResult AuthStore::sign_in(std::string_view username, std::string_view password,
                                           IssuedSession& session)
{
    // Allow one memory-hard password check at a time and back off after a failed
    // attempt.
    std::unique_lock attempt(impl_->sign_in_mutex, std::try_to_lock);
    if (!attempt || std::chrono::steady_clock::now() < impl_->retry_after) {
        return SignInResult::busy;
    }
    std::string stored_username;
    std::string stored_hash;
    std::string stored_key;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_available();
        if (!impl_->admin) {
            return SignInResult::invalid;
        }
        stored_username = impl_->admin->username;
        stored_hash = impl_->admin->password_hash;
        stored_key = impl_->admin->jwt_key;
    }
    if (!verify_password(password, stored_hash) || username != stored_username) {
        impl_->retry_after = std::chrono::steady_clock::now() + 1s;
        return SignInResult::invalid;
    }

    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if (!secret_equal(impl_->admin->jwt_key, stored_key)) {
        return SignInResult::invalid;
    }
    const auto now = unix_time();
    auto next = *impl_->admin;
    std::erase_if(next.sessions, [now](const auto& record) { return record.expires_at <= now; });
    if (next.sessions.size() == max_sessions) {
        next.sessions.erase(next.sessions.begin());
    }
    const auto lifetime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::days(impl_->database->session_expiry_days()));
    next.sessions.push_back(next.record(random_hex(16), now + lifetime.count()));
    impl_->commit(std::move(next));
    session = {impl_->admin->sessions.back().token, static_cast<int>(lifetime.count())};
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
    return Session{impl_->admin->username, match->expires_at, match->id};
}

std::optional<std::string> AuthStore::monitored_panel_path(std::string_view token, std::string_view session_id,
                                                          std::string_view connected_path) const
{
    std::lock_guard lock(impl_->mutex);
    if (impl_->storage_failed || !impl_->admin) {
        return std::nullopt;
    }
    if (impl_->admin->find_session(token) != impl_->admin->sessions.end()) {
        return impl_->database->panel_base_path();
    }

    // Only an already connected monitor gets this notice. The old cookie cannot authenticate again.
    const auto& restored = impl_->restored_panel_path;
    if (!restored || restored->path == connected_path || restored->expires_at <= std::chrono::steady_clock::now()) {
        return std::nullopt;
    }
    const auto active = std::ranges::find(restored->sessions, session_id, &RestoredPanelPath::RevokedSession::id);
    if (active == restored->sessions.end() || active->expires_at <= unix_time() ||
        restored->path != impl_->database->panel_base_path()) {
        return std::nullopt;
    }
    return restored->path;
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

AuthStore::UpdateResult AuthStore::update_admin(std::string_view token, std::string_view username,
                                                std::string_view current_password,
                                                std::string_view new_password)
{
    if (!new_password.empty() && !valid_new_password_shape(new_password)) {
        return UpdateResult::invalid_password;
    }
    std::unique_lock attempt(impl_->sign_in_mutex, std::try_to_lock);
    if (!attempt || std::chrono::steady_clock::now() < impl_->retry_after) {
        return UpdateResult::busy;
    }
    std::string stored_hash;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->check_available();
        if (!impl_->admin || impl_->admin->find_session(token) == impl_->admin->sessions.end()) {
            return UpdateResult::unauthorized;
        }
        stored_hash = impl_->admin->password_hash;
    }
    if (!verify_password(current_password, stored_hash)) {
        impl_->retry_after = std::chrono::steady_clock::now() + 1s;
        return UpdateResult::invalid;
    }
    if (!new_password.empty() && !strong_password(username, new_password)) {
        return UpdateResult::weak_password;
    }
    auto next_hash = new_password.empty() ? std::move(stored_hash) : hash_password(new_password);
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if (impl_->admin->find_session(token) == impl_->admin->sessions.end()) {
        return UpdateResult::unauthorized;
    }
    impl_->commit(Admin{std::string(username), std::move(next_hash), random_hex(32), {}});
    return UpdateResult::updated;
}

AuthStore::BasePathResult AuthStore::update_panel_base_path(std::string_view token, std::string_view previous,
                                                           std::string_view path, std::string_view setup_token)
{
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    if ((!previous.empty() || impl_->admin) &&
        (!impl_->admin || impl_->admin->find_session(token) == impl_->admin->sessions.end())) {
        return BasePathResult::unauthorized;
    }
    if (!impl_->admin && !impl_->setup_token.empty() && !secret_equal(impl_->setup_token, setup_token)) {
        return BasePathResult::setup_forbidden;
    }
    return impl_->database->save_panel_base_path(previous, path) ? BasePathResult::saved : BasePathResult::stale;
}

void AuthStore::validate_admin_backup(const Json& admins)
{
    read_backup_admin(admins);
}

bool AuthStore::restore_admin(const Json& admins, std::string_view session_id, std::string_view restored_base_path,
                              const std::function<void(const Json& local_auth_state)>& commit)
{
    auto next = read_backup_admin(admins);
    std::lock_guard lock(impl_->mutex);
    impl_->check_available();
    const auto now = unix_time();
    if (!impl_->admin || std::ranges::none_of(impl_->admin->sessions, [&](const auto& record) {
            return record.expires_at > now && record.id == session_id;
        })) {
        return false;
    }

    next.jwt_key = random_hex(32);
    RestoredPanelPath notice{std::string(restored_base_path), {}, {}};
    for (const auto& record : impl_->admin->sessions) {
        if (record.expires_at > now) {
            notice.sessions.emplace_back(record.id, record.expires_at);
        }
    }
    commit(next.state());
    notice.expires_at = std::chrono::steady_clock::now() + 1min;
    impl_->restored_panel_path = std::move(notice);
    impl_->admin = std::move(next);
    return true;
}

} // namespace wave
