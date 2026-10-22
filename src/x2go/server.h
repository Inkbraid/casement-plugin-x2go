// The X2Go server scripts the plugin runs over SSH, and their output.
//
// Every command runs in its own exec channel through a login shell, the
// way x2goclient runs them, so the scripts are on the PATH:
//   bash -l -c 'echo "X2GODATABEGIN:<id>"; export TERM="dumb"; <command>;
//               x2go_status=$?; echo "X2GODATAEND:<id>"; exit $x2go_status'
// The command's output is the text between the two marker lines, which
// keeps whatever login scripts print out of it; the channel's exit status
// is the command's.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace x2go::server {

/// Wraps a command as above. The command must not contain single quotes.
std::string wrap(std::string_view command, std::string_view marker);

/// The output between the markers, or nothing when the begin marker is
/// missing (the login shell or the command never got that far).
std::optional<std::string> unwrap(std::string_view output, std::string_view marker);

/// A connection's desktop command as the server scripts want it: one of
/// x2goruncommand's desktop names (XFCE, KDE, ...), whose case is fixed up,
/// or a program with arguments. Empty optional, with `error` set, when it
/// has characters that would need quoting.
std::optional<std::string> normalize_command(std::string_view command, std::string& error);

/// The command as it appears in session ids: the desktop name, or the
/// file name of the program, with x2gostartagent's sanitising applied.
std::string session_token(std::string_view command);

struct Geometry {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

/// x2gostartagent for a new KDrive desktop session. `layout` is an XKB
/// layout with an optional variant: "de", "de(nodeadkeys)".
std::string startagent_command(Geometry geometry, std::string_view layout, std::string_view command);

struct Started {
    std::string display;
    std::string cookie;
    std::uint32_t agent_pid = 0;
    std::string session_id;
    std::uint16_t graphics_port = 0;
    std::uint16_t sound_port = 0;
    std::uint16_t files_port = 0;
};
std::optional<Started> parse_startagent(std::string_view output);

/// What a non-zero exit code of x2gostartagent means, for the user.
std::string explain_startagent(int exit_code, std::string_view stderr_text);

/// One line of x2golistsessions.
struct Listed {
    std::uint32_t agent_pid = 0;
    std::string session_id;
    std::string display;
    std::string host;
    /// R running, S suspended (F finished, T terminating: not listed).
    char status = 0;
    std::string created;
    std::string cookie;
    std::uint16_t graphics_port = 0;
    std::uint16_t sound_port = 0;
};
std::vector<Listed> parse_listsessions(std::string_view output);

/// The parts of a session id: <user>-<display>-<time>_st<type><command>_dp<depth>.
struct SessionId {
    std::string user;
    std::string display;
    std::uint64_t time = 0;
    char type = 0;
    std::string command;
};
std::optional<SessionId> parse_session_id(std::string_view id);

/// The newest suspended KDrive desktop session started with `command`.
/// Running sessions belong to another client and are never picked.
std::optional<Listed> pick_resumable(const std::vector<Listed>& sessions, std::string_view command);

/// x2goresume-session for a session, with the size and layout for it.
std::string resume_command(std::string_view session_id, Geometry geometry, std::string_view layout);

struct Resumed {
    std::uint16_t graphics_port = 0;
    std::uint16_t sound_port = 0;
    std::uint16_t files_port = 0;
};
std::optional<Resumed> parse_resume(std::string_view output);

/// Starts the desktop in a new session, in the background.
std::string runcommand_command(const Started& session, std::string_view command);

std::string suspend_command(std::string_view session_id);
std::string terminate_command(std::string_view session_id);

/// Prints the agent's state file, then the end of the desktop command's
/// output file, which says why a desktop that didn't start failed.
std::string state_command(std::string_view session_id);

struct State {
    /// STARTING, RUNNING, RESUMING, SUSPENDED, TERMINATING, TERMINATED or
    /// empty when the state file is gone.
    std::string agent;
    /// The program x2goruncommand couldn't run, if that is what happened.
    std::optional<std::string> not_found;
};
State parse_state(std::string_view output);

}  // namespace x2go::server
