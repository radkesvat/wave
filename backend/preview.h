#pragma once

namespace httplib {
class Server;
}

namespace wave {

void configure_preview_route(httplib::Server& server);

} // namespace wave
