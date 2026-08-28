#include "hmac.h"

#include <openssl/hmac.h>
#include <openssl/crypto.h>

std::vector<uint8_t> hmac_sha256(const std::string &key, const uint8_t *data, size_t size)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    HMAC(EVP_sha256(),
         key.data(), static_cast<int>(key.size()),
         data, size,
         digest, &digest_len);

    return std::vector<uint8_t>(digest, digest + digest_len);
}

bool hmac_sha256_verify(const std::string &key, const uint8_t *data, size_t size,
                         const uint8_t *mac, size_t mac_size)
{
    std::vector<uint8_t> expected = hmac_sha256(key, data, size);

    if (expected.size() != mac_size)
    {
        return false;
    }

    return CRYPTO_memcmp(expected.data(), mac, mac_size) == 0;
}
