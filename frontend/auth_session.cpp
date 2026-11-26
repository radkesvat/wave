#include "auth_session.h"

#include <emscripten.h>
#include <nlohmann/json.hpp>

namespace wave {
namespace {

// clang-format off
EM_JS(int, read_auth_page, (), {
    if (location.protocol !== 'https:') return 0;
    switch (location.pathname) {
        case '/setup': return 1;
        case '/sign-in': return 2;
        case '/panel': return 3;
        default: return 0;
    }
});

EM_JS(void, send_auth_request, (AuthSession* session, const char* path_value, const char* json), {
    const path = UTF8ToString(path_value);
    fetch(path, {
        method: 'POST', mode: 'same-origin', credentials: 'same-origin', cache: 'no-store', redirect: 'error',
        headers: { 'Content-Type': 'application/json', 'X-Wave-CSRF': '1' },
        body: UTF8ToString(json), signal: AbortSignal.timeout(10000),
    }).then(response => {
        if (response.ok || (path === '/api/auth/setup' && response.status === 409)) {
            location.replace('/');
        } else {
            Module['_wave_auth_request_failed'](session, response.status);
        }
    }).catch(() => Module['_wave_auth_request_failed'](session, 0));
});

EM_JS_DEPS(monitor_session, "$stringToNewUTF8,free");
EM_JS(void, monitor_session, (AuthSession* session), {
    window.addEventListener('pageshow', event => {
        if (event.persisted) location.reload();
    });
    const connect = () => {
        const socket = new WebSocket('wss://' + location.host + '/ws/panel');
        socket.addEventListener('message', event => {
            const data = JSON.parse(event.data);
            if (data.type === 'session' && typeof data.username === 'string') {
                const username = stringToNewUTF8(data.username);
                Module['_wave_auth_identity'](session, username);
                _free(username);
            }
        });
        socket.addEventListener('close', async event => {
            Module['_wave_auth_identity'](session, 0);
            if (event.code === 1008) {
                location.replace('/');
                return;
            }
            // A failed upgrade has no HTTP status in the WebSocket API.
            try {
                const response = await fetch('/api/session', {
                    credentials: 'same-origin', cache: 'no-store', signal: AbortSignal.timeout(5000),
                });
                if (response.status === 401) {
                    location.replace('/');
                    return;
                }
            } catch {}
            setTimeout(connect, 2000);
        });
    };
    connect();
});
// clang-format on

} // namespace

void AuthSession::start()
{
    page_ = static_cast<Page>(read_auth_page());
    if (page_ == Page::panel) {
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
    const char* path = action == Action::setup     ? "/api/auth/setup"
                       : action == Action::sign_in ? "/api/auth/sign-in"
                                                   : "/api/auth/sign-out";
    send_auth_request(this, path, input.dump().c_str());
}

void AuthSession::request_failed(int status)
{
    pending_ = false;
    if (status == 429) {
        error_ = "Wait a moment and try again.";
    } else if (action_ == Action::setup && status == 403) {
        error_ = "The bootstrap token was rejected.";
    } else if (action_ == Action::setup && status == 400) {
        error_ = "Check the username, password, and acknowledgments.";
    } else if (action_ == Action::sign_in && status == 401) {
        error_ = "The username or password was rejected.";
    } else {
        error_ = "Wave could not complete the request. Try again.";
    }
}

void AuthSession::set_username(const char* username)
{
    username_ = username ? username : "";
}

} // namespace wave

extern "C" EMSCRIPTEN_KEEPALIVE void wave_auth_request_failed(wave::AuthSession* session, int status)
{
    session->request_failed(status);
}

extern "C" EMSCRIPTEN_KEEPALIVE void wave_auth_identity(wave::AuthSession* session, const char* username)
{
    session->set_username(username);
}
