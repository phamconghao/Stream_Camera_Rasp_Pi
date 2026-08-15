#include "ice_credentials.h"

#include <random>

namespace
{

// RFC 8839 section 5.3's ice-char: ALPHA / DIGIT / "+" / "/" - exactly
// base64's alphabet minus '=' padding, conveniently.
const char ICE_CHARSET[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

constexpr size_t ICE_CHARSET_SIZE = sizeof(ICE_CHARSET) - 1; // exclude the trailing '\0'

constexpr int ICE_UFRAG_LEN = 4;
constexpr int ICE_PWD_LEN = 22;

std::string random_ice_string(int length, std::mt19937 &rng)
{
    std::uniform_int_distribution<int> dist(0, static_cast<int>(ICE_CHARSET_SIZE) - 1);

    std::string out;
    out.reserve(static_cast<size_t>(length));

    for (int i = 0; i < length; i++)
    {
        out += ICE_CHARSET[dist(rng)];
    }

    return out;
}

} // namespace

ice_credentials_t generate_ice_credentials(void)
{
    // std::random_device seeds a std::mt19937 fresh each call rather
    // than reusing one PRNG instance across every session - simplest
    // way to avoid any cross-session state/thread-safety question for
    // something called at most once per new WebRTC peer connection,
    // not in any hot path.
    std::random_device rd;
    std::mt19937 rng(rd());

    ice_credentials_t creds;
    creds.ufrag = random_ice_string(ICE_UFRAG_LEN, rng);
    creds.pwd = random_ice_string(ICE_PWD_LEN, rng);

    return creds;
}
