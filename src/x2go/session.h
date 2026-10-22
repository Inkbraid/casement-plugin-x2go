// One X2Go connection: SSH sign-in, starting or resuming a KDrive session
// with the X2Go server scripts, and the KDrive stream over a direct-tcpip
// channel, mapped onto Casement's protocol.

#pragma once

#include <memory>

#include "casement/plugin.h"

namespace x2go {

/// Starts a connection for `connect`. Returns at once; the connection is
/// made on a thread of its own and reports through `host`.
std::unique_ptr<casement::Session> start_session(casement::Host& host, const casement::Connect& connect);

}  // namespace x2go
