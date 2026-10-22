// known_hosts matching and fingerprints. The keys are throwaway test keys;
// the expected fingerprints and hashed names come from ssh-keygen.

#include "check.h"

#include <string>

#include "util.h"
#include "x2go/known_hosts.h"
#include "x2go/options.h"

using namespace x2go;
using namespace x2go::hostkeys;
using testing::bytes;

namespace {

constexpr const char* key1 = "AAAAC3NzaC1lZDI1NTE5AAAAIM99h/0xmpf8OWOvofrdChq7ITeikG5WXQTiGMpvh478";
constexpr const char* key2 = "AAAAC3NzaC1lZDI1NTE5AAAAIMGUO5wajtm1Qlmjq3eMxOsRevO/O9HWhb6vHwVIaQgJ";
constexpr const char* key3 =
    "AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBCUySZKxt+0Ns6Lb7L9vxrHegRSa9AdJMfptjko/RjW8Qb+UPXAyuUDQ+"
    "YnMggRiaB6OnkRBqQ6pUlQQQmiHRro=";

std::string line(std::string_view hosts, std::string_view key, std::string_view type = "ssh-ed25519") {
    return std::string(hosts) + " " + std::string(type) + " " + std::string(key) + "\n";
}

}  // namespace

TEST(prints_openssh_fingerprints) {
    CHECK_EQ(sha256_fingerprint(bytes(key1)), std::string("SHA256:r/KHAbljIENocAzI2z9fRYuHRuFPYs82l6YeX2yfz5k"));
    CHECK_EQ(sha256_fingerprint(bytes(key3)), std::string("SHA256:etQJ+bkdwGDoz2huEyCR+daOYnUTcMxZSv0T8Ka4fMc"));
    CHECK_EQ(blob_type(bytes(key1)), std::string("ssh-ed25519"));
    CHECK_EQ(blob_type(bytes(key3)), std::string("ecdsa-sha2-nistp256"));
    CHECK_EQ(blob_type(std::vector<std::uint8_t>{0, 0}), std::string());
}

TEST(normalizes_pinned_fingerprints) {
    const std::string fp = "SHA256:TAiBO/s3yDG4w9T2qXFdss0mBLD0cIszJ/CUzboewuM";
    CHECK(normalize_fingerprint(fp) == fp);
    CHECK(normalize_fingerprint("TAiBO/s3yDG4w9T2qXFdss0mBLD0cIszJ/CUzboewuM=") == fp);
    CHECK(normalize_fingerprint(" sha256:TAiBO_s3yDG4w9T2qXFdss0mBLD0cIszJ_CUzboewuM ") == fp);
    CHECK(!normalize_fingerprint("SHA256:tooshort"));
    CHECK(!normalize_fingerprint("MD5:12:34"));
    CHECK_EQ(sha256_fingerprint(bytes(key2)), fp);
}

TEST(names_hosts_like_openssh) {
    CHECK_EQ(entry_name("Server.Example.TEST", 22), std::string("server.example.test"));
    CHECK_EQ(entry_name("10.1.2.3", 2222), std::string("[10.1.2.3]:2222"));
    CHECK_EQ(entry_name("fd00::5", 2222), std::string("[fd00::5]:2222"));
}

TEST(matches_plain_and_patterned_hosts) {
    CHECK(host_matches("server.example.test", "server.example.test"));
    CHECK(host_matches("other,SERVER.example.test", "server.example.test"));
    CHECK(host_matches("*.example.test", "server.example.test"));
    CHECK(host_matches("server.example.tes?", "server.example.test"));
    CHECK(!host_matches("*.example.test,!server.example.test", "server.example.test"));
    CHECK(!host_matches("server.example.test", "[server.example.test]:2222"));
    CHECK(host_matches("[server.example.test]:2222", "[server.example.test]:2222"));
    CHECK(!host_matches("example.test", "server.example.test"));
}

TEST(matches_hashed_hosts) {
    // ssh-keygen -H of "[example.test]:2222" and "server.example.test".
    CHECK(host_matches("|1|SGjkuJNSMnfr1cNy9rBxGPA6HXM=|uwDgrsCI+npWYSFfe/rxd9B2qLs=", "[example.test]:2222"));
    CHECK(host_matches("|1|NtmwnKRGXIbz2lEdjb1dlos+duM=|1QYhCPLoiyQo09qn4sGX9E5HWCI=", "server.example.test"));
    CHECK(!host_matches("|1|NtmwnKRGXIbz2lEdjb1dlos+duM=|1QYhCPLoiyQo09qn4sGX9E5HWCI=", "server.example.tes"));
    CHECK(!host_matches("|1|garbage", "server.example.test"));

    const std::string file = "# hashed\n|1|SGjkuJNSMnfr1cNy9rBxGPA6HXM=|uwDgrsCI+npWYSFfe/rxd9B2qLs= ssh-ed25519 " +
                             std::string(key1) + "\n";
    auto c = hostkeys::check(file, "Example.test", 2222, bytes(key1));
    CHECK(c.verdict == Verdict::known);
    CHECK_EQ(c.line, 2);
    CHECK(hostkeys::check(file, "example.test", 22, bytes(key1)).verdict == Verdict::unknown);
}

TEST(tells_known_changed_and_unknown_keys_apart) {
    const std::string file = "\n# comment\n" + line("server.example.test,10.0.0.1", key1) +
                             line("[server.example.test]:2222", key3, "ecdsa-sha2-nistp256") +
                             line("other.example.test", key2);
    auto known = hostkeys::check(file, "server.example.test", 22, bytes(key1));
    CHECK(known.verdict == Verdict::known);
    CHECK_EQ(known.line, 3);

    auto changed = hostkeys::check(file, "server.example.test", 22, bytes(key2));
    CHECK(changed.verdict == Verdict::changed);
    CHECK_EQ(changed.line, 3);

    // Only an ECDSA key is known on port 2222: an Ed25519 key is new, not changed.
    auto other = hostkeys::check(file, "server.example.test", 2222, bytes(key1));
    CHECK(other.verdict == Verdict::unknown);
    REQUIRE(other.types.size() == 1);
    CHECK_EQ(other.types[0], std::string("ecdsa-sha2-nistp256"));
    CHECK(known_types(file, "server.example.test", 2222) == std::vector<std::string>{"ecdsa-sha2-nistp256"});

    CHECK(hostkeys::check(file, "unknown.example.test", 22, bytes(key1)).verdict == Verdict::unknown);
    CHECK(hostkeys::check(file, "10.0.0.1", 22, bytes(key1)).verdict == Verdict::known);
}

TEST(honours_markers_and_odd_lines) {
    const std::string file = "@revoked * ssh-ed25519 " + std::string(key2) + "\r\n" +
                             "@cert-authority *.example.test ssh-ed25519 " + std::string(key1) + "\r\n" +
                             "broken-line-without-key\r\n" + line("server.example.test", key2) +
                             line("server.example.test", key1);
    CHECK(hostkeys::check(file, "server.example.test", 22, bytes(key2)).verdict == Verdict::revoked);
    // A certificate authority line is no host key.
    auto c = hostkeys::check(file, "server.example.test", 22, bytes(key1));
    CHECK(c.verdict == Verdict::known);
    CHECK_EQ(c.line, 5);
}
