#include <stdint.h>
#include <string.h>
#include <emscripten/emscripten.h>
#include "bip32.h"
#include "hash.h"
#include "qrcodegen.h"

static uint8_t qr_temp[qrcodegen_BUFFER_LEN_MAX];
static uint8_t qr_code[qrcodegen_BUFFER_LEN_MAX];
static int qr_size;

static int derive_child(const uint8_t *xpub, uint32_t branch, uint32_t index, uint8_t *out) {
    uint8_t branch_pub[33], branch_chain[32], chain[32], h[20];
    if (memcmp(xpub, "\x04\x35\x87\xcf", 4) || xpub[4] != 3 || branch > 1 || index >= 100000) return 0;
    if (!bip32_pub_child(xpub + 45, xpub + 13, branch, branch_pub, branch_chain) ||
        !bip32_pub_child(branch_pub, branch_chain, index, out, chain))
        return 0;
    hash160(out, 33, h);
    out[33] = 0;
    out[34] = 20;
    memcpy(out + 35, h, sizeof(h));
    return 1;
}

EMSCRIPTEN_KEEPALIVE int derive_p2wpkh(const uint8_t *xpub, uint32_t branch, uint32_t index, uint8_t *out) {
    return derive_child(xpub, branch, index, out);
}

EMSCRIPTEN_KEEPALIVE int find_p2wpkh(const uint8_t *xpub, const uint8_t *program, uint32_t max_index, uint32_t *path,
                                     uint8_t *out) {
    uint8_t branch_pub[33], branch_chain[32], pub[33], chain[32], hash[20];
    if (memcmp(xpub, "\x04\x35\x87\xcf", 4) || xpub[4] != 3 || max_index >= 100000) return 0;
    for (uint32_t branch = 0; branch <= 1; branch++) {
        if (!bip32_pub_child(xpub + 45, xpub + 13, branch, branch_pub, branch_chain)) return 0;
        for (uint32_t index = 0; index <= max_index; index++) {
            if (!bip32_pub_child(branch_pub, branch_chain, index, pub, chain)) continue;
            hash160(pub, sizeof(pub), hash);
            if (memcmp(hash, program, sizeof(hash))) continue;
            memcpy(out, pub, sizeof(pub));
            out[33] = 0;
            out[34] = 20;
            memcpy(out + 35, hash, sizeof(hash));
            path[0] = branch;
            path[1] = index;
            return 1;
        }
    }
    return 0;
}

EMSCRIPTEN_KEEPALIVE int qr_start(const char *text) {
    qr_size = 0;
    if (qrcodegen_encodeText(text, qr_temp, qr_code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                             qrcodegen_Mask_AUTO, false))
        qr_size = qrcodegen_getSize(qr_code);
    return qr_size;
}

EMSCRIPTEN_KEEPALIVE int qr_module(int x, int y) {
    return qr_size && x >= 0 && y >= 0 && x < qr_size && y < qr_size && qrcodegen_getModule(qr_code, x, y);
}

int main(void) {
    return 0;
}
