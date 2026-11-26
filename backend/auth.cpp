#include "auth.h"

#include "auth_store.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <memory>
#include <string_view>

namespace wave {
namespace {

using Json = nlohmann::json;
using httplib::Request;
using httplib::Response;
using httplib::Server;
using Decision = Server::HandlerResponse;

constexpr std::string_view cookie_name = "__Host-wave_session=";
constexpr std::string_view panel_socket = "/ws/panel";

bool exact_header(const Request& request, const char* name, std::string_view value)
{
    return request.get_header_value_count(name) == 1 && request.get_header_value(name) == value;
}

std::string session_cookie(const Request& request)
{
    if (request.get_header_value_count("Cookie") != 1) {
        return {};
    }
    const auto header = request.get_header_value("Cookie");
    std::string_view cookies = header;
    std::string_view token;
    bool found = false;
    while (!cookies.empty()) {
        const auto delimiter = cookies.find(';');
        auto cookie = cookies.substr(0, delimiter);
        const auto first = cookie.find_first_not_of(" \t");
        if (first != cookie.npos) {
            cookie.remove_prefix(first);
            if (cookie.starts_with(cookie_name)) {
                if (found) {
                    return {};
                }
                found = true;
                token = cookie.substr(cookie_name.size());
            }
        }
        if (delimiter == cookies.npos) {
            break;
        }
        cookies.remove_prefix(delimiter + 1);
    }
    return token.size() <= 1024 ? std::string(token) : std::string{};
}

void set_cookie(Response& response, std::string_view token)
{
    auto value = std::string(cookie_name) + std::string(token) +
                 "; Path=/; Secure; HttpOnly; SameSite=Strict; Max-Age=";
    value += token.empty() ? "0; Expires=Thu, 01 Jan 1970 00:00:00 GMT"
                           : std::to_string(AuthStore::session_seconds);
    response.set_header("Set-Cookie", value);
}

Decision reject(Response& response, int status)
{
    response.status = status;
    return Decision::Handled;
}

bool is_entry_asset(const Request& request)
{
    if (request.method != "GET" && request.method != "HEAD") {
        return false;
    }
    return request.path == "/" || request.path == "/index.html" || request.path == "/setup" ||
           request.path == "/sign-in" || request.path == "/panel" || request.path == "/index.js" ||
           request.path == "/index.wasm";
}

std::string_view credential(const Json& input, const char* name)
{
    const auto field = input.find(name);
    return field != input.end() && field->is_string() ? field->get_ref<const std::string&>()
                                                      : std::string_view{};
}

Json session_data(const AuthStore::Session& session)
{
    return {{"type", "session"}, {"username", session.username}, {"expires_at", session.expires_at}};
}

} // namespace

void configure_authenticated_routes(Server& server, const std::filesystem::path& site_dir,
                                    const std::filesystem::path& state_dir, const std::string& authority)
{
    const auto origin = "https://" + authority;
    const auto store = std::make_shared<AuthStore>(state_dir);
    server.set_default_headers({{"Cache-Control", "no-store"},
                                {"Referrer-Policy", "same-origin"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Content-Security-Policy", "frame-ancestors 'none'; base-uri 'none'"}});
    server.set_payload_max_length(4096);

    // The pre-routing hook also runs before WebSocket upgrades. Only entry resources and
    // the three same-origin auth requests may proceed without an active session.
    server.set_pre_routing_handler([store, authority, origin](const Request& req, Response& res) {
        if (!req.ssl || !exact_header(req, "Host", authority)) {
            return reject(res, 403);
        }
        if (req.path == panel_socket && !exact_header(req, "Origin", origin)) {
            return reject(res, 403);
        }
        if (req.method != "GET" && req.method != "HEAD") {
            if (!exact_header(req, "Origin", origin) || !exact_header(req, "X-Wave-CSRF", "1") ||
                !exact_header(req, "Content-Type", "application/json")) {
                return reject(res, 403);
            }
            if (req.method == "POST" && (req.path == "/api/auth/setup" || req.path == "/api/auth/sign-in" ||
                                         req.path == "/api/auth/sign-out")) {
                return Decision::Unhandled;
            }
        }
        if (is_entry_asset(req) || store->session(session_cookie(req))) {
            return Decision::Unhandled;
        }
        return reject(res, 401);
    });

    const auto page = [store, index = (site_dir / "index.html").string()](const Request& req, Response& res) {
        const auto token = session_cookie(req);
        const auto session = store->session(token);
        const auto current = session ? "/panel" : store->installed() ? "/sign-in" : "/setup";
        if (!session && !token.empty()) {
            set_cookie(res, {});
        }
        if (req.target != current) {
            res.set_redirect(current, 303);
            return;
        }
        res.set_file_content(index, "text/html; charset=utf-8");
    };
    for (const auto* route : {"/", "/index.html", "/setup", "/sign-in", "/panel"}) {
        server.Get(route, page);
    }
    server.Get("/index.js", [file = (site_dir / "index.js").string()](const Request&, Response& res) {
        res.set_file_content(file, "text/javascript; charset=utf-8");
    });
    server.Get("/index.wasm", [file = (site_dir / "index.wasm").string()](const Request&, Response& res) {
        res.set_file_content(file, "application/wasm");
    });
    for (const auto* name : {"wave.txt", "dear-imgui.txt", "poppins.txt", "nlohmann-json.txt",
                             "nlohmann-json-third-party.txt", "nayuki-qr.txt", "implot.txt"}) {
        server.Get(std::string("/licenses/") + name,
                   [file = (site_dir / "licenses" / name).string()](const Request&, Response& res) {
                       res.set_file_content(file, "text/plain; charset=utf-8");
                   });
    }

    server.Post("/api/auth/setup", [store](const Request& req, Response& res) {
        if (store->installed()) {
            res.status = 409;
            return;
        }
        const auto input = Json::parse(req.body, nullptr, false);
        const auto username = credential(input, "username");
        const auto password = credential(input, "password");
        if (!AuthStore::valid_username(username) || !AuthStore::valid_password(password) ||
            !input.contains("acknowledged") || input["acknowledged"] != true) {
            res.status = 400;
            return;
        }
        switch (store->setup(username, password, credential(input, "bootstrap"))) {
        case AuthStore::SetupResult::created:
            res.status = 201;
            break;
        case AuthStore::SetupResult::closed:
            res.status = 409;
            break;
        case AuthStore::SetupResult::forbidden:
            res.status = 403;
            break;
        }
    });

    server.Post("/api/auth/sign-in", [store](const Request& req, Response& res) {
        const auto input = Json::parse(req.body, nullptr, false);
        const auto username = credential(input, "username");
        const auto password = credential(input, "password");
        if (!AuthStore::valid_username(username) || !AuthStore::valid_password(password)) {
            res.status = 401;
            return;
        }
        std::string token;
        switch (store->sign_in(username, password, token)) {
        case AuthStore::SignInResult::accepted:
            set_cookie(res, token);
            res.status = 204;
            break;
        case AuthStore::SignInResult::invalid:
            res.status = 401;
            break;
        case AuthStore::SignInResult::busy:
            res.set_header("Retry-After", "1");
            res.status = 429;
            break;
        }
    });

    server.Post("/api/auth/sign-out", [store](const Request& req, Response& res) {
        store->sign_out(session_cookie(req));
        set_cookie(res, {});
        res.status = 204;
    });

    server.Get("/api/session", [store](const Request& req, Response& res) {
        if (const auto session = store->session(session_cookie(req))) {
            res.set_content(session_data(*session).dump(), "application/json");
        } else {
            res.status = 401;
        }
    });

    server.WebSocket(std::string(panel_socket), [store](const Request& req, httplib::ws::WebSocket& socket) {
        const auto token = session_cookie(req);
        const auto session = store->session(token);
        if (session && !socket.send(session_data(*session).dump())) {
            return;
        }
        socket.set_read_timeout(1);
        std::string message;
        while (store->session(token)) {
            const auto result = socket.read(message);
            if (result == httplib::ws::Text || result == httplib::ws::Binary) {
                socket.close(httplib::ws::CloseStatus::PolicyViolation, "This socket is read-only.");
                return;
            }
            if (result != httplib::ws::Timeout) {
                return;
            }
        }
        socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
    });
}

} // namespace wave
