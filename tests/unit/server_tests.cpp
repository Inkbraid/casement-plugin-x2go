// The X2Go server scripts' command lines and output, and connection
// settings. The outputs are copies of what the scripts printed on an
// Ubuntu 24.04 server (x2goserver 4.1.0.6).

#include "check.h"

#include <string>

#include "fixtures.h"
#include "util.h"
#include "x2go/kdrive.h"
#include "x2go/options.h"
#include "x2go/server.h"

using namespace x2go;
using namespace x2go::server;

namespace {

constexpr std::string_view startagent_output =
    "50\nb2cffb29b4fd3d46b289cacf10efb8e5\n28314\nx2gotest-50-1792613533_stKx2go-test-desktop_dp24\n10600\n10601\n"
    "10602\n";
constexpr std::string_view list_running =
    "28314|x2gotest-50-1792613533_stKx2go-test-desktop_dp24|50|vm|R|2026-10-21T20:12:13|"
    "b2cffb29b4fd3d46b289cacf10efb8e5|127.0.0.1|10600|10601|2026-10-21T20:12:15|x2gotest|27|10602|-1|-1\n";
constexpr std::string_view list_suspended =
    "28314|x2gotest-50-1792613533_stKx2go-test-desktop_dp24|50|vm|S|2026-10-21T20:12:13|"
    "b2cffb29b4fd3d46b289cacf10efb8e5|127.0.0.1|10600|10601|2026-10-21T20:12:41|x2gotest|30|10602|-1|-1\n";
constexpr std::string_view resume_output = "gr_port=10600\nsound_port=10601\nfs_port=10602\n";

}  // namespace

TEST(wraps_commands_in_a_login_shell) {
    const std::string w = wrap("x2golistsessions", "m1");
    CHECK_EQ(w, std::string("bash -l -c 'echo \"X2GODATABEGIN:m1\"; export TERM=\"dumb\"; x2golistsessions; "
                            "x2go_status=$?; echo \"X2GODATAEND:m1\"; exit $x2go_status'"));
    auto out = unwrap("Welcome to the server\nX2GODATABEGIN:m1\nline 1\nline 2\nX2GODATAEND:m1\n", "m1");
    REQUIRE(out);
    CHECK_EQ(*out, std::string("line 1\nline 2\n"));
    CHECK(!unwrap("bash: x2golistsessions: command not found\n", "m1"));
}

TEST(parses_x2gostartagent_output) {
    auto s = parse_startagent(startagent_output);
    REQUIRE(s);
    CHECK_EQ(s->display, std::string("50"));
    CHECK_EQ(s->cookie, std::string("b2cffb29b4fd3d46b289cacf10efb8e5"));
    CHECK_EQ(s->agent_pid, 28314u);
    CHECK_EQ(s->session_id, std::string("x2gotest-50-1792613533_stKx2go-test-desktop_dp24"));
    CHECK_EQ(s->graphics_port, 10600);
    CHECK_EQ(s->sound_port, 10601);
    CHECK_EQ(s->files_port, 10602);
    CHECK(!parse_startagent("50\n02f5e163\n"));
    CHECK(!parse_startagent("50\nnot-a-cookie-not-a-cookie-xxxxxx\n1\nid\n1\n2\n3\n"));
    CHECK(!parse_startagent("50\nb2cffb29b4fd3d46b289cacf10efb8e5\n28314\nbad id;rm\n10600\n10601\n10602\n"));
}

TEST(handshake_starts_with_the_session_cookie) {
    // The cookie the client sent is the one x2gostartagent printed for
    // that session, as 32 ASCII characters.
    auto started = parse_startagent(
        "50\nb2cffb29b4fd3d46b289cacf10efb8e5\n28314\nx2gotest-50-1792613533_stKx2go-test-desktop_dp24\n10600\n"
        "10601\n10602\n");
    REQUIRE(started);
    CHECK(testing::bytes(started->cookie) == testing::bytes(fixtures::client_cookie));
    CHECK_EQ(started->cookie.size(), kdrive::cookie_size);
}

TEST(explains_x2gostartagent_failures) {
    CHECK(explain_startagent(127, "bash: line 1: x2gostartagent: command not found").find("isn't installed") !=
          std::string::npos);
    CHECK(explain_startagent(9, "LIMIT 1").find("limit") != std::string::npos);
    CHECK(explain_startagent(12, "").find("xserver-x2gokdrive") != std::string::npos);
    CHECK(explain_startagent(42, "something odd\nmore").find("something odd") != std::string::npos);
}

TEST(parses_x2golistsessions_output) {
    auto running = parse_listsessions(list_running);
    REQUIRE(running.size() == 1);
    CHECK_EQ(running[0].status, 'R');
    CHECK_EQ(running[0].agent_pid, 28314u);
    CHECK_EQ(running[0].graphics_port, 10600);
    CHECK_EQ(running[0].cookie, std::string("b2cffb29b4fd3d46b289cacf10efb8e5"));
    std::string noisy = "Use of uninitialized value in concatenation at line 7.\n" + std::string(list_suspended) + "\n";
    auto suspended = parse_listsessions(noisy);
    REQUIRE(suspended.size() == 1);
    CHECK_EQ(suspended[0].status, 'S');
    CHECK(parse_listsessions("").empty());
}

TEST(parses_session_ids) {
    auto id = parse_session_id("x2gotest-50-1792613533_stKx2go-test-desktop_dp24");
    REQUIRE(id);
    CHECK_EQ(id->user, std::string("x2gotest"));
    CHECK_EQ(id->display, std::string("50"));
    CHECK_EQ(id->time, 1792613533u);
    CHECK_EQ(id->type, 'K');
    CHECK_EQ(id->command, std::string("x2go-test-desktop"));
    auto odd = parse_session_id("a-b_st-51-1700000000_stKXFCE_dp24");
    REQUIRE(odd);
    CHECK_EQ(odd->user, std::string("a-b_st"));
    CHECK_EQ(odd->command, std::string("XFCE"));
    CHECK(!parse_session_id("nonsense"));
}

TEST(picks_the_newest_suspended_kdrive_session_with_the_command) {
    std::string many = std::string(list_running);
    many += "100|ann-51-1700000100_stKXFCE_dp24|51|vm|S|2023-11-14T22:15:00|0123456789abcdef0123456789abcdef|"
            "1.2.3.4|30001|30002|x|ann|5|30003|-1|-1\n";
    many += "101|ann-52-1700000200_stKXFCE_dp24|52|vm|S|2023-11-14T22:16:40|0123456789abcdef0123456789abcdeA|"
            "1.2.3.4|30011|30012|x|ann|5|30013|-1|-1\n";
    many += "102|ann-53-1700000300_stDXFCE_dp24|53|vm|S|2023-11-14T22:18:20|0123456789abcdef0123456789abcdeB|"
            "1.2.3.4|30021|30022|x|ann|5|30023|-1|-1\n";
    many += "103|ann-54-1700000400_stKXFCE_dp24|54|vm|R|2023-11-14T22:20:00|0123456789abcdef0123456789abcdeC|"
            "1.2.3.4|30031|30032|x|ann|5|30033|-1|-1\n";
    auto sessions = parse_listsessions(many);
    CHECK_EQ(sessions.size(), 5u);
    auto pick = pick_resumable(sessions, "XFCE");
    REQUIRE(pick);
    CHECK_EQ(pick->session_id, std::string("ann-52-1700000200_stKXFCE_dp24"));  // not NX (D), not running
    CHECK(!pick_resumable(sessions, "LXDE"));
    CHECK(!pick_resumable(parse_listsessions(list_running), "/home/x2gotest/bin/x2go-test-desktop"));
    auto mine = pick_resumable(parse_listsessions(list_suspended), "/home/x2gotest/bin/x2go-test-desktop");
    REQUIRE(mine);
    CHECK_EQ(mine->graphics_port, 10600);
}

TEST(parses_x2goresume_session_output) {
    auto r = parse_resume(resume_output);
    REQUIRE(r);
    CHECK_EQ(r->graphics_port, 10600);
    CHECK_EQ(r->sound_port, 10601);
    CHECK_EQ(r->files_port, 10602);
    CHECK(!parse_resume("ERROR: failed to resume session with ID x\n"));
}

TEST(builds_session_commands) {
    CHECK_EQ(startagent_command({1024, 768}, "de(nodeadkeys)", "XFCE"),
             std::string("X2GO_XINERAMA=no x2gostartagent 1024x768 lan 16m-jpeg-9 unix-kde-depth_24 de "
                         "pc105/de\\(nodeadkeys\\) 1 K XFCE both"));
    CHECK_EQ(startagent_command({1280, 720}, "us", "/usr/bin/xterm -bg black"),
             std::string("X2GO_XINERAMA=no x2gostartagent 1280x720 lan 16m-jpeg-9 unix-kde-depth_24 us pc105/us 1 K "
                         "xterm both"));
    CHECK_EQ(resume_command("x2gotest-50-1792613533_stKXFCE_dp24", {800, 600}, "fr"),
             std::string("x2goresume-session x2gotest-50-1792613533_stKXFCE_dp24 800x600 lan 16m-jpeg-9 fr pc105/fr "
                         "1 both no"));
    auto s = parse_startagent(startagent_output);
    REQUIRE(s);
    CHECK_EQ(runcommand_command(*s, "/usr/bin/xterm -bg black"),
             std::string("(setsid x2goruncommand 50 28314 x2gotest-50-1792613533_stKx2go-test-desktop_dp24 10601 "
                         "/usr/bin/xtermX2GO_SPACE_CHAR-bgX2GO_SPACE_CHARblack nosnd D </dev/null >/dev/null 2>&1 &)"));
    CHECK_EQ(suspend_command("id1"), std::string("x2gosuspend-session id1"));
    CHECK_EQ(terminate_command("id1"), std::string("x2goterminate-session id1"));
}

TEST(reads_why_a_session_ended) {
    auto s = parse_state("TERMINATED\nexec /usr/bin/xfce4-session\nX2GORUNCOMMAND ERR NOEXEC:/usr/bin/xfce4-session\n");
    CHECK_EQ(s.agent, std::string("TERMINATED"));
    REQUIRE(s.not_found);
    CHECK_EQ(*s.not_found, std::string("/usr/bin/xfce4-session"));
    auto t = parse_state("SUSPENDED\n");
    CHECK_EQ(t.agent, std::string("SUSPENDED"));
    CHECK(!t.not_found);
    CHECK(parse_state("").agent.empty());
}

TEST(normalizes_desktop_commands) {
    std::string error;
    CHECK(normalize_command("xfce", error) == std::string("XFCE"));
    CHECK(normalize_command(" lxqt ", error) == std::string("LXQt"));
    CHECK(normalize_command("Gnome", error) == std::string("GNOME"));
    CHECK(normalize_command("openbox", error) == std::string("openbox"));
    CHECK(normalize_command("/usr/bin/xterm   -bg black", error) == std::string("/usr/bin/xterm -bg black"));
    CHECK(!normalize_command("xterm; rm -rf ~", error));
    CHECK(error.find(";") != std::string::npos);
    CHECK(!normalize_command("it's", error));
    CHECK(!normalize_command("$(reboot)", error));
    CHECK(!normalize_command("   ", error));
    CHECK_EQ(session_token("/usr/bin/xterm -bg black"), std::string("xterm"));
    CHECK_EQ(session_token("XFCE"), std::string("XFCE"));
    CHECK_EQ(session_token("tool:v2"), std::string("toolPPv2"));
    CHECK_EQ(session_token("+"), std::string("desktop"));
}

TEST(reads_connection_settings) {
    std::string error;
    auto o = parse_options("x2go://ann@server.example.test:2222?command=lxqt&layout=de(nodeadkeys)&new&close="
                           "terminate&hostkey=SHA256%3ATAiBO%2Fs3yDG4w9T2qXFdss0mBLD0cIszJ%2FCUzboewuM&key=C%3A%5Ckeys%5Cid",
                           "", error);
    REQUIRE(o);
    CHECK_EQ(o->host, std::string("server.example.test"));
    CHECK_EQ(o->port, 2222);
    CHECK_EQ(o->user, std::string("ann"));
    CHECK_EQ(o->command, std::string("LXQt"));
    CHECK_EQ(o->layout, std::string("de(nodeadkeys)"));
    CHECK(!o->resume);
    CHECK(o->terminate_on_close);
    CHECK_EQ(o->hostkey, std::string("SHA256:TAiBO/s3yDG4w9T2qXFdss0mBLD0cIszJ/CUzboewuM"));
    CHECK_EQ(o->key, std::string("C:\\keys\\id"));

    auto d = parse_options("X2GO://server", "bob", error);
    REQUIRE(d);
    CHECK_EQ(d->port, 22);
    CHECK_EQ(d->user, std::string("bob"));
    CHECK_EQ(d->command, std::string("XFCE"));
    CHECK(d->layout.empty());
    CHECK(d->resume);
    CHECK(!d->terminate_on_close);
    CHECK(d->hostkey.empty());
    // Casement's user name wins over the address's.
    CHECK_EQ(parse_options("x2go://ann@server", "bob", error)->user, std::string("bob"));
    CHECK(parse_options("x2go://server?close=suspend", "", error).has_value());

    CHECK(!parse_options("vnc://server", "", error));
    CHECK(!parse_options("x2go://server?close=later", "", error));
    CHECK(error.find("close") != std::string::npos);
    CHECK(!parse_options("x2go://server?layout=De", "", error));
    CHECK(!parse_options("x2go://server?hostkey=abc", "", error));
    CHECK(!parse_options("x2go://server?command=a%7Cb", "", error));
}
