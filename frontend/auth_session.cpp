#include "auth_session.h"

#include <emscripten.h>
#include <nlohmann/json.hpp>

#include <cstdlib>

namespace wave {
namespace {

// clang-format off
EM_JS(int, read_auth_page, (), {
    if (location.protocol !== 'http:' && location.protocol !== 'https:') return 0;
    switch (location.pathname.slice(Module['basePath'].length)) {
        case '/setup': return 1;
        case '/sign-in': return 2;
        case '/': return 3;
        case '/base-path': return 4;
        default: return 0;
    }
});

EM_JS_DEPS(read_panel_base_path, "$stringToNewUTF8");
EM_JS(char*, read_panel_base_path, (), {
    return stringToNewUTF8(Module['basePath']);
});

EM_JS(void, send_auth_request, (AuthSession* session, const char* path_value, const char* json), {
    const path = UTF8ToString(path_value);
    const basePath = Module['basePath'];
    fetch(basePath + path, {
        method: 'POST', mode: 'same-origin', credentials: 'same-origin', cache: 'no-store', redirect: 'error',
        headers: Module['authRequestHeaders'](path),
        body: UTF8ToString(json), signal: AbortSignal.timeout(10000),
    }).then(response => {
        if (Module['panelLocationPending']()) return;
        if (path === '/api/auth/base-path' && response.status === 201) {
            if (!Module['followPanelLocation'](response.headers.get('Location'))) {
                Module['_wave_auth_request_failed'](session, 0);
            }
            return;
        }
        const sessionEnded = response.status === 401 && (path === '/api/auth/admin' ||
            path === '/api/auth/sign-out' || path === '/api/auth/base-path');
        if ((response.ok && path !== '/api/auth/base-path') || sessionEnded ||
            (path === '/api/auth/setup' && response.status === 409)) {
            if (path === '/api/auth/setup') Module['clearSetupToken']();
            location.replace(basePath + '/');
        } else {
            Module['_wave_auth_request_failed'](session, response.status);
        }
    }).catch(() => {
        if (!Module['panelLocationPending']()) Module['_wave_auth_request_failed'](session, 0);
    });
});

EM_JS_DEPS(monitor_session, "$stringToNewUTF8,free");
EM_JS(void, monitor_session, (AuthSession* session), {
    window.addEventListener('pageshow', event => {
        if (event.persisted) location.reload();
    });
    const protocol = location.protocol === 'https:' ? 'wss://' : 'ws://';
    const basePath = Module['basePath'];
    const panelUrl = basePath + '/';
    const reconnectDelay = 2000;
    const locationPendingAfterRestore = async () => {
        // Restore revokes this session before it replies with the new panel address.
        await Module['restoreApplyFinished']();
        return Module['panelLocationPending']();
    };
    const applyIdentity = data => {
        if (data?.type !== 'session' || typeof data.username !== 'string' || !data.username ||
            typeof data.session_id !== 'string' || !data.session_id) {
            return false;
        }
        const username = stringToNewUTF8(data.username);
        const id = stringToNewUTF8(data.session_id);
        Module['_wave_auth_identity'](session, username, id);
        _free(id);
        _free(username);
        return true;
    };
    const connect = () => {
        if (Module['panelLocationPending']()) return;
        let deadline;
        let finished = false;
        let identified = false;
        const recover = async code => {
            if (finished || Module['panelLocationPending']()) return;
            finished = true;
            clearTimeout(deadline);
            if (await locationPendingAfterRestore()) return;
            if (identified || code === 1008) Module['_wave_auth_identity'](session, 0, 0);
            if (code === 1008) {
                location.replace(panelUrl);
                return;
            }
            // Verify with the signed cookie when the monitor is unavailable or never sends identity.
            let valid = false;
            try {
                const response = await fetch(panelUrl + 'api/session', {
                    mode: 'same-origin', credentials: 'same-origin', cache: 'no-store', redirect: 'error',
                    signal: AbortSignal.timeout(5000),
                });
                if (await locationPendingAfterRestore()) return;
                if (response.status === 401) {
                    Module['_wave_auth_identity'](session, 0, 0);
                    location.replace(panelUrl);
                    return;
                }
                if (response.ok) {
                    const data = await response.json();
                    if (Module['panelLocationPending']()) return;
                    valid = applyIdentity(data);
                }
            } catch {}
            if (await locationPendingAfterRestore()) return;
            if (!valid) Module['_wave_auth_identity'](session, 0, 0);
            setTimeout(connect, reconnectDelay);
        };
        let socket;
        try {
            socket = new WebSocket(protocol + location.host + basePath + '/ws/panel');
        } catch {
            recover();
            return;
        }
        deadline = setTimeout(() => {
            recover();
            socket.close();
        }, reconnectDelay);
        socket.addEventListener('message', event => {
            if (finished || Module['panelLocationPending']()) return;
            let data;
            try { data = JSON.parse(event.data); } catch { return; }
            if (applyIdentity(data)) {
                identified = true;
                clearTimeout(deadline);
            } else if (data?.type === 'base_path' && typeof data.path === 'string' && data.path !== basePath) {
                Module['followPanelLocation'](data.path + '/');
            }
        });
        socket.addEventListener('close', event => recover(event.code));
    };
    connect();
});
// clang-format on

} // namespace

void AuthSession::start()
{
    page_ = static_cast<Page>(read_auth_page());
    if (page_ == Page::panel) {
        if (auto* path = read_panel_base_path()) {
            base_path_ = path;
            std::free(path);
        }
        monitor_session(this);
    }
}

void AuthSession::submit(Action action, const nlohmann::json& input)
{
    if (pending_) {
        return;
    }
    action_ = action;
    pending_ = true;
    error_.clear();
    const char* path = "";
    switch (action) {
    case Action::setup:
        path = "/api/auth/setup";
        break;
    case Action::sign_in:
        path = "/api/auth/sign-in";
        break;
    case Action::sign_out:
        path = "/api/auth/sign-out";
        break;
    case Action::update_administrator:
        path = "/api/auth/admin";
        break;
    case Action::base_path:
        path = "/api/auth/base-path";
        break;
    }
    send_auth_request(this, path, input.dump().c_str());
}

void AuthSession::request_failed(int status)
{
    pending_ = false;
    if (status == 429) {
        error_ = "Wait a moment and try again.";
    } else if (status == 422 && (action_ == Action::setup || action_ == Action::update_administrator)) {
        error_ = "Choose a less predictable password. A longer password may help.";
    } else if (action_ == Action::setup && status == 400) {
        error_ = "Check the username and password requirements.";
    } else if (action_ == Action::base_path && status == 400) {
        error_ = base_path_format_error;
    } else if (status == 403 &&
               (action_ == Action::setup || (action_ == Action::base_path && page_ == Page::base_path))) {
        error_ = "Open the setup URL printed by the installer.";
    } else if (action_ == Action::sign_in && status == 401) {
        error_ = "The username or password was rejected.";
    } else if (action_ == Action::update_administrator && status == 403) {
        error_ = "The current password was rejected.";
    } else if (action_ == Action::update_administrator && status == 400) {
        error_ = "Check the username and new password requirements.";
    } else {
        error_ = "Wave could not complete the request. Try again.";
    }
}

void AuthSession::set_identity(const char* username, const char* session_id)
{
    username_ = username ? username : "";
    session_id_ = session_id ? session_id : "";
}

} // namespace wave

extern "C" EMSCRIPTEN_KEEPALIVE void wave_auth_request_failed(wave::AuthSession* session, int status)
{
    session->request_failed(status);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_auth_identity(wave::AuthSession* session, const char* username,
                                                         const char* id)
{
    session->set_identity(username, id);
}
