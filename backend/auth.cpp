#include "auth.h"

#include "auth_store.h"
#include "panel_base_path.h"
#include "panel_settings.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace wave {
namespace {

using Json = nlohmann::json;
using httplib::Request;
using httplib::Response;
using httplib::Server;
using Decision = Server::HandlerResponse;

constexpr std::string_view panel_socket = "/ws/panel";
constexpr std::string_view page_routes[] = {"/", "/index.html", "/setup", "/sign-in", "/base-path"};
constexpr std::string_view websocket_routes[] = {"/ws", panel_socket, "/ws/console", "/ws/overview",
                                                  "/ws/workspace"};
constexpr std::string_view sign_in_route = "/api/auth/sign-in";
constexpr std::string_view base_path_route = "/api/auth/base-path";

struct SiteAsset {
    std::string_view path;
    std::string_view content_type;
    bool public_file = false;
};

constexpr SiteAsset site_assets[] = {
    {"/index.js", "text/javascript; charset=utf-8", true},
    {"/index.wasm", "application/wasm", true},
    {"/console.js", "text/javascript; charset=utf-8"},
    {"/terminal/xterm.js", "text/javascript; charset=utf-8"},
    {"/terminal/addon-fit.js", "text/javascript; charset=utf-8"},
    {"/terminal/xterm.css", "text/css; charset=utf-8"},
    {"/licenses/wave.txt", "text/plain; charset=utf-8"},
    {"/licenses/dear-imgui.txt", "text/plain; charset=utf-8"},
    {"/licenses/poppins.txt", "text/plain; charset=utf-8"},
    {"/licenses/nlohmann-json.txt", "text/plain; charset=utf-8"},
    {"/licenses/nlohmann-json-third-party.txt", "text/plain; charset=utf-8"},
    {"/licenses/implot.txt", "text/plain; charset=utf-8"},
    {"/licenses/xterm.txt", "text/plain; charset=utf-8"},
    {"/licenses/xterm-addon-fit.txt", "text/plain; charset=utf-8"},
};

enum class Access { missing, entry, authenticated, websocket, base_path_setup };

Access route_access(std::string_view route, std::string_view method, bool bootstrap)
{
    if (method == "POST") {
        if (route == sign_in_route) {
            return Access::entry;
        }
        if (bootstrap) {
            return route == base_path_route ? Access::base_path_setup : Access::missing;
        }
        if (route == "/api/auth/setup" || route == "/api/auth/sign-out") {
            return Access::entry;
        }
        return route == "/api/auth/admin" ? Access::authenticated : Access::missing;
    }
    if (method != "GET" && method != "HEAD") {
        return Access::missing;
    }
    if (std::ranges::find(page_routes, route) != std::end(page_routes)) {
        if ((bootstrap && (route == "/index.html" || route == "/setup")) ||
            (!bootstrap && route == "/base-path")) {
            return Access::missing;
        }
        return Access::entry;
    }
    if (const auto asset = std::ranges::find(site_assets, route, &SiteAsset::path); asset != std::end(site_assets)) {
        return asset->public_file ? Access::entry : bootstrap ? Access::missing : Access::authenticated;
    }
    if (bootstrap) {
        return Access::missing;
    }
    if (std::ranges::find(websocket_routes, route) != std::end(websocket_routes)) {
        return Access::websocket;
    }
    return route == "/api/session" ? Access::authenticated : Access::missing;
}

std::string_view setup_page(bool installed, bool authenticated)
{
    return installed && !authenticated ? "/sign-in" : "/base-path";
}

bool secure_transport(const Request& request)
{
#ifdef CPPHTTPLIB_SSL_ENABLED
    return request.ssl != nullptr;
#else
    static_cast<void>(request);
    return false;
#endif
}

std::string_view cookie_name(bool secure)
{
    return secure ? "__Host-wave_session=" : "wave_session=";
}

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
    const auto name = cookie_name(secure_transport(request));
    std::string_view token;
    bool found = false;
    while (!cookies.empty()) {
        const auto delimiter = cookies.find(';');
        auto cookie = cookies.substr(0, delimiter);
        const auto first = cookie.find_first_not_of(" \t");
        if (first != cookie.npos) {
            cookie.remove_prefix(first);
            if (cookie.starts_with(name)) {
                if (found) {
                    return {};
                }
                found = true;
                token = cookie.substr(name.size());
            }
        }
        if (delimiter == cookies.npos) {
            break;
        }
        cookies.remove_prefix(delimiter + 1);
    }
    return token.size() <= 1024 ? std::string(token) : std::string{};
}

void set_cookie(Response& response, bool secure, std::string_view token, int lifetime_seconds = 0)
{
    auto value = std::string(cookie_name(secure)) + std::string(token) + "; Path=/";
    if (secure) {
        value += "; Secure";
    }
    value += "; HttpOnly; SameSite=Strict; Max-Age=";
    value += token.empty() ? "0; Expires=Thu, 01 Jan 1970 00:00:00 GMT"
                           : std::to_string(lifetime_seconds);
    response.set_header("Set-Cookie", value);
}

Decision reject(Response& response, int status)
{
    response.status = status;
    return Decision::Handled;
}

Decision not_found(Response& response)
{
    response.set_content("<html>\r\n"
                         "<head><title>404 Not Found</title></head>\r\n"
                         "<body>\r\n"
                         "<center><h1>404 Not Found</h1></center>\r\n"
                         "<hr><center>nginx</center>\r\n"
                         "</body>\r\n"
                         "</html>\r\n",
                         "text/html");
    return reject(response, 404);
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

std::string panel_route(std::string_view suffix)
{
    std::string pattern = "((?:/[A-Za-z0-9_-]+)+)";
    for (const auto character : suffix) {
        if (std::string_view(R"(.^$|()[]{}*+?\)").find(character) != std::string_view::npos) {
            pattern += '\\';
        }
        pattern += character;
    }
    return pattern;
}

std::function<bool()> websocket_session(const std::shared_ptr<AuthStore>& store,
                                        const Request& request)
{
    return [store, token = session_cookie(request)] { return store->session(token).has_value(); };
}

void configure_authenticated_routes(Server& server, const std::filesystem::path& site_dir,
                                    std::shared_ptr<AuthStore> store, std::shared_ptr<PanelDatabase> database, bool secure,
                                    const std::string& authority)
{
    server.set_default_headers({{"Cache-Control", "no-store"},
                                {"Referrer-Policy", "same-origin"},
                                {"X-Content-Type-Options", "nosniff"},
                                {"Content-Security-Policy", "frame-ancestors 'none'; base-uri 'none'"}});
    server.set_payload_max_length(4096);

    // The pre-routing hook checks the configured prefix before HTTP dispatch and WebSocket upgrades.
    server.set_pre_routing_handler([store, database, secure, authority](const Request& req, Response& res) {
        const auto raw_path = std::string_view(req.target).substr(0, req.target.find('?'));
        if (raw_path != req.path) {
            return not_found(res);
        }
        const auto base_path = database->panel_base_path();
        const bool bootstrap = base_path.empty();
        const auto prefix = bootstrap ? panel_setup_prefix : std::string_view(base_path);
        const bool read = req.method == "GET" || req.method == "HEAD";
        const bool setup_redirect = bootstrap && read && (raw_path == "/" || raw_path == "/panel");
        if (!setup_redirect && !has_panel_prefix(raw_path, prefix)) {
            return not_found(res);
        }
        const auto suffix = setup_redirect ? std::string_view{} : raw_path.substr(prefix.size());
        const bool redirect = setup_redirect || (!bootstrap && read && suffix.empty());
        const auto access = route_access(suffix, req.method, bootstrap);
        if (!redirect && access == Access::missing) {
            return not_found(res);
        }

        const auto host = req.get_header_value("Host");
        if (secure_transport(req) != secure || req.get_header_value_count("Host") != 1 ||
            (secure && host != authority)) {
            return reject(res, 403);
        }
        if (redirect) {
            const auto destination = setup_redirect ? setup_page(store->installed(),
                                                                 store->session(session_cookie(req)).has_value())
                                                    : "/";
            res.set_redirect(std::string(prefix).append(destination), 303);
            return Decision::Handled;
        }
        const auto origin = (secure ? "https://" : "http://") + host;
        if (access == Access::websocket && !exact_header(req, "Origin", origin)) {
            return reject(res, 403);
        }
        if (!read && (!exact_header(req, "Origin", origin) || !exact_header(req, "X-Wave-CSRF", "1") ||
                      !exact_header(req, "Content-Type", "application/json"))) {
            return reject(res, 403);
        }
        if (access == Access::entry || (access == Access::base_path_setup && !store->installed()) ||
            store->session(session_cookie(req))) {
            return Decision::Unhandled;
        }
        return reject(res, 401);
    });

    const auto page = [store, secure, index = (site_dir / "index.html").string()](const Request& req, Response& res) {
        const auto prefix = req.matches[1].str();
        const auto token = session_cookie(req);
        const auto session = store->session(token);
        const bool installed = store->installed();
        const auto suffix = prefix == panel_setup_prefix ? setup_page(installed, session.has_value())
                                                       : session ? "/" : installed ? "/sign-in" : "/setup";
        const auto current = prefix + std::string(suffix);
        if (!session && !token.empty()) {
            set_cookie(res, secure, {});
        }
        if (req.target != current) {
            res.set_redirect(current, 303);
            return;
        }
        res.set_file_content(index, "text/html; charset=utf-8");
    };
    for (const auto route : page_routes) {
        server.Get(panel_route(route), page);
    }
    for (const auto& asset : site_assets) {
        server.Get(panel_route(asset.path), [file = (site_dir / asset.path.substr(1)).string(),
                                           type = std::string(asset.content_type)](const Request&, Response& res) {
            res.set_file_content(file, type);
        });
    }

    server.Post(std::string(panel_setup_prefix).append(base_path_route), [database, store](const Request& req,
                                                                                         Response& res) {
        if (store->installed() && !store->session(session_cookie(req))) {
            res.status = 401;
            return;
        }
        const auto input = Json::parse(req.body, nullptr, false);
        const auto value = input.find(panel_base_path_key);
        if (value == input.end() || !value->is_string() || !valid_panel_base_path(value->get_ref<const std::string&>())) {
            res.status = 400;
            return;
        }
        const auto& path = value->get_ref<const std::string&>();
        try {
            if (!database->save_panel_base_path(path)) {
                not_found(res);
                return;
            }
        } catch (const std::runtime_error&) {
            res.status = 500;
            return;
        }
        res.set_header("Location", path + '/');
        res.status = 201;
    });

    server.Post(panel_route("/api/auth/setup"), [store](const Request& req, Response& res) {
        if (store->installed()) {
            res.status = 409;
            return;
        }
        const auto input = Json::parse(req.body, nullptr, false);
        const auto username = credential(input, "username");
        const auto password = credential(input, "password");
        if (!AuthStore::valid_username(username)) {
            res.status = 400;
            return;
        }
        switch (store->setup(username, password)) {
        case AuthStore::SetupResult::created:
            res.status = 201;
            break;
        case AuthStore::SetupResult::closed:
            res.status = 409;
            break;
        case AuthStore::SetupResult::invalid_password:
            res.status = 400;
            break;
        case AuthStore::SetupResult::weak_password:
            res.status = 422;
            break;
        }
    });

    server.Post(panel_route(sign_in_route), [store, secure](const Request& req, Response& res) {
        const auto input = Json::parse(req.body, nullptr, false);
        const auto username = credential(input, "username");
        const auto password = credential(input, "password");
        if (!AuthStore::valid_username(username) || !AuthStore::valid_password_input(password)) {
            res.status = 401;
            return;
        }
        AuthStore::IssuedSession session;
        switch (store->sign_in(username, password, session)) {
        case AuthStore::SignInResult::accepted:
            set_cookie(res, secure, session.token, session.lifetime_seconds);
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

    server.Post(panel_route("/api/auth/sign-out"), [store, secure](const Request& req, Response& res) {
        store->sign_out(session_cookie(req));
        set_cookie(res, secure, {});
        res.status = 204;
    });

    server.Post(panel_route("/api/auth/admin"), [store, secure](const Request& req, Response& res) {
        const auto input = Json::parse(req.body, nullptr, false);
        const auto username = credential(input, "username");
        const auto current_password = credential(input, "current_password");
        const auto new_password = input.find("password");
        if (!AuthStore::valid_username(username) || !AuthStore::valid_password_input(current_password) ||
            new_password == input.end() || !new_password->is_string()) {
            res.status = 400;
            return;
        }
        const auto& password = new_password->get_ref<const std::string&>();
        switch (store->update_admin(session_cookie(req), username, current_password, password)) {
        case AuthStore::UpdateResult::updated:
            set_cookie(res, secure, {});
            res.status = 204;
            break;
        case AuthStore::UpdateResult::unauthorized:
            res.status = 401;
            break;
        case AuthStore::UpdateResult::invalid:
            res.status = 403;
            break;
        case AuthStore::UpdateResult::invalid_password:
            res.status = 400;
            break;
        case AuthStore::UpdateResult::weak_password:
            res.status = 422;
            break;
        case AuthStore::UpdateResult::busy:
            res.set_header("Retry-After", "1");
            res.status = 429;
            break;
        }
    });

    server.Get(panel_route("/api/session"), [store](const Request& req, Response& res) {
        if (const auto session = store->session(session_cookie(req))) {
            res.set_content(session_data(*session).dump(), "application/json");
        } else {
            res.status = 401;
        }
    });

    server.WebSocket(panel_route(panel_socket), [store](const Request& req, httplib::ws::WebSocket& socket) {
        const auto token = session_cookie(req);
        const auto session = store->session(token);
        if (!session) {
            socket.close(httplib::ws::CloseStatus::PolicyViolation, "Session ended.");
            return;
        }
        if (!socket.send(session_data(*session).dump())) {
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
