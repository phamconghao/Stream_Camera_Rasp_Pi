#include "rtcp_packet.h"

#include <chrono>

// NTP epoch (1900-01-01) is 70 years before the Unix epoch
// (1970-01-01), which is exactly 2208988800 seconds - a well-known
// constant used whenever converting between the two.
static constexpr uint32_t NTP_UNIX_EPOCH_DELTA_SEC = 2208988800U;

void rtcp_get_ntp_now(uint32_t *out_ntp_sec, uint32_t *out_ntp_frac)
{
    using namespace std::chrono;

    auto now = system_clock::now().time_since_epoch();
    uint64_t total_ns = duration_cast<nanoseconds>(now).count();

    uint64_t unix_sec = total_ns / 1000000000ULL;
    uint64_t remainder_ns = total_ns % 1000000000ULL;

    *out_ntp_sec = static_cast<uint32_t>(unix_sec + NTP_UNIX_EPOCH_DELTA_SEC);

    // Fractional part: remainder_ns / 1e9 of a second, expressed in
    // 1/2^32 units as NTP requires.
    *out_ntp_frac = static_cast<uint32_t>(
        (remainder_ns * 4294967296ULL) / 1000000000ULL);
}
