/* The device reviews and signs one PSBT at a time, then returns it as an animated QR.
 * Seed stays in RAM; Clear seed wipes it. TEST_SEED=1 offers a public vector and must never hold funds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/rand.h"
#include "core.h"
#include "parser_host.h"
#include "sha256.h"
#include "wipe.h"
#include "ui.h"
#include "st7789.h"
#include "buttons.h"
#include "camera.h"
#include "quirc.h"
#include "seedqr.h"
#include "parser_wasm.h"
#include "test_psbt.h"
#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#define TEST_MNEMONIC "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"

/* parser.wasm's linear memory comes out of this pool too, about 132KB with UR reassembly and
 * encoding. Measured at 149KB peak under QEMU */
static char pool[160 * 1024];
static uint8_t parser_wasm_rw[sizeof(parser_wasm)];
/* Not needed at the same time as quirc while reading (90KB, 77KB of it the image), so this is taken
 * from the heap only for the signing step */
#define SIGNED_PSBT_MAX (PARSER_PSBT_MAX + 2048)
static uint8_t *prevtx_arena, *signed_psbt, *psbt;
static int camera_ready;
static plan_t plan;
static ui_review_t review_ui;
static ui_menu_t menu;

/* With NO_LCD=1 the screens' text goes to the UART instead of the panel, which is how this was
 * checked before anything was soldered */
static void show(const ui_screen_t *s) {
    uint16_t line[UI_W];
#if NO_LCD
    printf("+--------------------------------+\n");
    for (int r = 0; r < UI_ROWS; r++)
        if (s->text[r][0]) printf("| %-30s |\n", s->text[r]);
    printf("+--------------------------------+\n");
    for (int y = 0; y < UI_H; y++) ui_render_line(s, y, line);
#else
    st7789_begin_frame();
    for (int y = 0; y < UI_H; y++) {
        ui_render_line(s, y, line);
        st7789_write_line(line);
    }
#endif
}

static int wait_key(void) {
    for (;;) {
        int key = buttons_poll();
        if (key >= 0) return key;
        sleep_ms(10);
    }
}

static void message(const char *title, const char *body, int warn) {
    static ui_screen_t s;
    ui_message(&s, title, body, warn);
    show(&s);
    printf("%s%s%s\n", title, body ? ": " : "", body ? body : "");
    wait_key();
}

/* The buffers the signing step uses, released on the way back to the menu because quirc takes the
 * same memory while reading */
static int sign_buffers_take(void) {
    prevtx_arena = malloc(PARSER_PSBT_MAX);
    signed_psbt = malloc(SIGNED_PSBT_MAX);
    return prevtx_arena && signed_psbt;
}

static void sign_buffers_give(void) {
    free(prevtx_arena);
    free(signed_psbt);
    prevtx_arena = signed_psbt = NULL;
}

/* Shows the captured frame cropped to fill the display. */
static void show_preview(const uint8_t *img, const char *status) {
#if NO_LCD
    (void)img;
    printf("%s\n", status);
#else
    uint16_t line[UI_W];
    st7789_begin_frame();
    for (int y = 0; y < UI_H; y++) {
        ui_render_scan_line(img, CAMERA_W, CAMERA_H, status, y, line);
        st7789_write_line(line);
    }
#endif
}

static void receive_psbt_part(const char *part, uint32_t part_len, int32_t *rc, unsigned int *parts, uint64_t *seen,
                              uint32_t *len, uint8_t *out, char *status, size_t status_cap) {
    unsigned int seq = 0, seq_len = 0;
    for (uint32_t i = 0; i < part_len; i++)
        if (part[i] == '/') {
            while (++i < part_len && part[i] >= '0' && part[i] <= '9') seq = seq * 10 + (unsigned int)(part[i] - '0');
            if (i < part_len && part[i] == '-')
                while (++i < part_len && part[i] >= '0' && part[i] <= '9')
                    seq_len = seq_len * 10 + (unsigned int)(part[i] - '0');
            break;
        }
    if (!parser_host_ur_receive(part, part_len, rc, out, PARSER_PSBT_MAX)) {
        snprintf(status, status_cap, "QR read, not a PSBT");
    } else if (*rc > 0) {
        *len = (uint32_t)*rc;
    } else if (*rc == 0) {
        if (seq && seq <= 64 && !(*seen >> (seq - 1) & 1)) *seen |= (uint64_t)1 << (seq - 1), (*parts)++;
        if (seq_len) snprintf(status, status_cap, "Scan PSBT  %u/%u parts", *parts, seq_len);
        else snprintf(status, status_cap, "Scan PSBT  %u parts", *parts);
    } else {
        snprintf(status, status_cap, "UR error %d", (int)*rc);
    }
}

/* Reads an animated QR and reassembles the PSBT. Returns its length, or 0 if cancelled or failed */
static uint32_t scan_psbt(void) {
    struct quirc *q;
    unsigned int parts = 0;
    uint64_t seen = 0; /* which parts have arrived, so a repeat is not counted twice */
    uint32_t len = 0;
    char status[UI_COLS + 1];

    if (!camera_ready) return message("No camera", "Wire it up or use the test PSBT.", 1), 0;
    if (!(psbt = malloc(PARSER_PSBT_MAX))) return message("Out of memory", NULL, 1), 0;
    if (!(q = quirc_new()) || quirc_resize(q, CAMERA_W, CAMERA_H) < 0) {
        free(psbt), psbt = NULL;
        return message("Out of memory", NULL, 1), 0;
    }
    parser_host_ur_reset();
    snprintf(status, sizeof(status), "Scan PSBT: aim at the QR");

    while (!len) {
        uint8_t *img = quirc_begin(q, NULL, NULL); /* captured straight into quirc's image buffer */
        int found = 0;
        if (buttons_poll() >= 0) break; /* any key cancels */
        if (!camera_capture(img, 500)) continue;
        show_preview(img, status);
        quirc_end(q);
        for (int i = 0; i < quirc_count(q) && !len; i++) {
            static struct quirc_code code;
            static struct quirc_data data;
            int32_t rc = 0;
            quirc_extract(q, i, &code);
            found = 1;
            if (quirc_decode(&code, &data)) {
                snprintf(status, sizeof(status), "QR found, cannot read");
                continue;
            }
            receive_psbt_part((const char *)data.payload, (uint32_t)data.payload_len, &rc, &parts, &seen, &len, psbt,
                              status, sizeof(status));
        }
        if (!found && parts == 0) snprintf(status, sizeof(status), "Scan PSBT: no QR in view");
    }
    quirc_destroy(q);
    if (!len) free(psbt), psbt = NULL;
    printf("scan: %u parts, psbt %u bytes\n", parts, (unsigned int)len);
    return len;
}

static int trng(uint8_t *buf, size_t len) {
    /* pico_rand is seeded from the TRNG but is not a cryptographic PRNG. It is used only as Schnorr
     * aux data during the initial bring-up */
    for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)get_rand_32();
    return 1;
}

static void draw_qr(void) {
    uint16_t line[UI_W];
#if NO_LCD
    for (int y = 0; y < UI_H; y++) ui_qr_render_line(y, line);
#else
    st7789_begin_frame();
    for (int y = 0; y < UI_H; y++) {
        ui_qr_render_line(y, line);
        st7789_write_line(line);
    }
#endif
}

/* Hands the signed PSBT back as an animated QR. Only pure parts are cycled, so a receiver that
 * cannot use mixed parts still completes, and a missed part comes round again. Any key ends it */
static void export_qr(uint32_t len) {
    long seq_len = parser_host_ur_encode_start(len, UI_UR_FRAGMENT);
    if (seq_len <= 0) return message("UR encode failed", NULL, 1);
    unsigned long part = 0;
    for (uint64_t next = 0;;) {
        static char text[1024];
        int key = buttons_poll();
        if (key == UI_KEY_UP || key == UI_KEY_DOWN) {
            ui_qr_level(key == UI_KEY_UP ? 1 : -1);
            printf("qr level %d\n", ui_qr_level_get());
            draw_qr();
        } else if (key >= 0) {
            return;
        }
        if (time_us_64() >= next) {
            if (part && part % (unsigned long)seq_len == 0) parser_host_ur_encode_start(len, UI_UR_FRAGMENT);
            if (!parser_host_ur_encode_next(text, sizeof(text)) || !ui_qr_set(text))
                return message("QR failed", NULL, 1);
            part++;
            /* also on the UART, for when the camera cannot read the screen */
            printf("%s\n", text);
            draw_qr();
            /* at 250ms a phone changes part before it has focused, and parts get missed */
            next = time_us_64() + 500 * 1000;
        }
        sleep_ms(10);
    }
}

/* Signs one PSBT and returns to the menu. psbt and psbt_len are what was read */
static void sign_flow(const uint8_t *in, uint32_t in_len) {
    core_prevtx_t prev[PLAN_MAX_INPUTS];
    core_review_t review;
    core_display_t display;
    core_sig_t sigs[PLAN_MAX_INPUTS];
    uint8_t nonces[PLAN_MAX_INPUTS][33]; /* the coordinator's n going in, then Q coming out */
    uint32_t rc = 0, out_len = 0;
    unsigned int n_sigs = 0;
    uint64_t t;
    int err, decision = UI_PENDING;

    if (!sign_buffers_take()) return sign_buffers_give(), message("Out of memory", NULL, 1);

    t = time_us_64();
    if (!parser_host_parse(in, in_len, core_fingerprint(), &rc, &plan, prev, prevtx_arena, PARSER_PSBT_MAX) || rc) {
        printf("parse failed rc=%u\n", (unsigned int)rc);
        return sign_buffers_give(), message("Invalid PSBT", NULL, 1);
    }
    printf("parse %llu us, pool highmark %u\n", (unsigned long long)(time_us_64() - t),
           (unsigned int)parser_host_pool_highmark());

    t = time_us_64();
    if ((err = core_review(&plan, prev, &review)) != CORE_OK ||
        (err = core_display(&plan, &review, &display)) != CORE_OK) {
        printf("review err=%d\n", err);
        return sign_buffers_give(), message("Rejected by review", NULL, 1);
    }
    printf("review %llu us\n", (unsigned long long)(time_us_64() - t));

    ui_review_init(&review_ui, &display);
    show(&review_ui.screens[0]);
    while (decision == UI_PENDING) {
        int key = buttons_poll();
        if (key >= 0) {
            unsigned int before = review_ui.cur;
            unsigned int selection = review_ui.screens[review_ui.cur].selection;
            decision = ui_review_key(&review_ui, key);
            if (review_ui.cur != before || review_ui.screens[review_ui.cur].selection != selection)
                show(&review_ui.screens[review_ui.cur]);
        }
        sleep_ms(10);
    }
    if (decision == UI_REJECTED) {
        sign_buffers_give();
        return;
    }

    t = time_us_64();
    if (!parser_host_nonces(nonces)) return sign_buffers_give(), message("Finalize failed", NULL, 1);
    for (unsigned int i = 0; i < PLAN_MAX_INPUTS; i++)
        if (nonces[i][0] == 1) core_set_host_nonce(i, nonces[i] + 1);
    if ((err = core_sign(&plan, trng, sigs, &n_sigs)) != CORE_OK) {
        printf("sign err=%d\n", err);
        return sign_buffers_give(), message("Sign failed", NULL, 1);
    }
    printf("sign %llu us (%u inputs)\n", (unsigned long long)(time_us_64() - t), n_sigs);
    for (unsigned int k = 0; k < PLAN_MAX_INPUTS; k++) memcpy(nonces[k], core_nonce_point(k), 33);
    if (!parser_host_set_points(nonces) || !parser_host_finalize(sigs, n_sigs, signed_psbt, SIGNED_PSBT_MAX, &out_len))
        return sign_buffers_give(), message("Finalize failed", NULL, 1);

    printf("signed psbt (%u bytes):\n", (unsigned int)out_len);
    for (uint32_t i = 0; i < out_len; i++) printf("%02x%s", signed_psbt[i], (i % 32 == 31) ? "\n" : "");
    printf("\n");
    export_qr(out_len);
    sign_buffers_give();
}

static void scan_and_sign(void) {
    uint32_t len = scan_psbt();
    if (!len) return;
    sign_flow(psbt, len);
    free(psbt), psbt = NULL;
}

#if !defined(__EMSCRIPTEN__)
static void seed_input_line(ui_screen_t *s, unsigned int row, const char *text) {
    size_t len = strnlen(text, UI_COLS);
    memcpy(s->text[row], text, len);
    s->text[row][len] = 0;
}

static void keyboard_row(char out[UI_COLS + 1], const char *keys, unsigned int selected) {
    size_t at = 0;
    for (unsigned int i = 0; keys[i]; i++) {
        if (i) out[at++] = ' ';
        if (i == selected) out[at++] = '[';
        out[at++] = keys[i];
        if (i == selected) out[at++] = ']';
    }
    out[at] = 0;
}

static void word_input_screen(ui_screen_t *s, unsigned int word, unsigned int total, const char *prefix, size_t len,
                              char matches[3][BIP39_WORD_MAX + 1], size_t count, int exact, int row, int col) {
    static const char *const keys[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const unsigned int key_count[] = {10, 9, 7};
    char line[UI_COLS + 1];

    memset(s, 0, sizeof(*s));
    snprintf(s->text[1], sizeof(s->text[1]), "Word %u/%u", word + 1, total);
    snprintf(line, sizeof(line), "Prefix: %.*s", (int)len, prefix);
    seed_input_line(s, 2, line);
    if (!len) seed_input_line(s, 3, "Type a few letters");
    else if (!count) seed_input_line(s, 3, "No matching BIP39 word");
    else if (exact) {
        snprintf(line, sizeof(line), "Exact: %.*s", (int)len, prefix);
        seed_input_line(s, 3, line);
    } else if (count == 1) {
        snprintf(line, sizeof(line), "Match: %.8s", matches[0]);
        seed_input_line(s, 3, line);
    } else {
        snprintf(line, sizeof(line), "Matches: %.8s %.8s%s", matches[0], matches[1], count > 2 ? " +" : "");
        seed_input_line(s, 3, line);
    }
    for (unsigned int r = 0; r < 3; r++) {
        keyboard_row(line, keys[r], row == (int)r ? (unsigned int)col : key_count[r]);
        seed_input_line(s, 4 + r, line);
    }
    seed_input_line(s, 8, "Arrows move, Select types");
    seed_input_line(s, 9, "A: erase  B: select  C: cancel");
}

static int enter_word(char *out, unsigned int word, unsigned int total) {
    static const char *const keys[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    static const unsigned int key_count[] = {10, 9, 7};
    char prefix[BIP39_WORD_MAX + 1] = {0}, matches[3][BIP39_WORD_MAX + 1] = {{0}};
    ui_screen_t screen;
    int row = 0, col = 0;

    for (;;) {
        size_t len = strlen(prefix);
        int exact = 0;
        size_t count = len ? bip39_word_prefix((const uint8_t *)prefix, len, matches, 3, &exact) : 0;
        word_input_screen(&screen, word, total, prefix, len, matches, count, exact, row, col);
        show(&screen);
        wipe(&screen, sizeof(screen));
        int key = wait_key();
        if (key == UI_KEY_C) {
            wipe(prefix, sizeof(prefix));
            wipe(matches, sizeof(matches));
            return 0;
        }
        if (key == UI_KEY_A) {
            if (len) prefix[len - 1] = 0;
        } else if (key == UI_KEY_B) {
            if (exact || count == 1) {
                strncpy(out, exact ? prefix : matches[0], BIP39_WORD_MAX);
                out[BIP39_WORD_MAX] = 0;
                wipe(prefix, sizeof(prefix));
                wipe(matches, sizeof(matches));
                return 1;
            }
        } else if (key == UI_KEY_LEFT && col > 0) {
            col--;
        } else if (key == UI_KEY_RIGHT && col + 1 < (int)key_count[row]) {
            col++;
        } else if (key == UI_KEY_UP && row > 0) {
            row--;
            if (col >= (int)key_count[row]) col = (int)key_count[row] - 1;
        } else if (key == UI_KEY_DOWN && row < 2) {
            row++;
            if (col >= (int)key_count[row]) col = (int)key_count[row] - 1;
        } else if (key == UI_KEY_SELECT && len < BIP39_WORD_MAX) {
            prefix[len] = keys[row][col];
            prefix[len + 1] = 0;
        }
    }
}

static int enter_mnemonic(char *mn, size_t cap) {
    static const unsigned int counts[] = {12, 15, 18, 21, 24};
    static const char *const options[] = {"12 words", "15 words", "18 words", "21 words", "24 words"};
    unsigned int words = 0;

    ui_menu_init(&menu, "Word count", options, sizeof(options) / sizeof(options[0]));
    show(&menu.screen);
    for (;;) {
        int sel = ui_menu_key(&menu, wait_key());
        if (sel == UI_MENU_BACK) return 0;
        if (sel == UI_MENU_PENDING) {
            show(&menu.screen);
            continue;
        }
        words = counts[sel];
        break;
    }
    size_t used = 0;
    mn[0] = 0;
    for (unsigned int i = 0; i < words; i++) {
        char word[BIP39_WORD_MAX + 1] = {0};
        if (!enter_word(word, i, words)) {
            wipe(word, sizeof(word));
            wipe(mn, cap);
            return 0;
        }
        if (used + strlen(word) + (i ? 1 : 0) + 1 > cap) {
            wipe(word, sizeof(word));
            wipe(mn, cap);
            return 0;
        }
        if (i) mn[used++] = ' ';
        size_t len = strlen(word);
        memcpy(mn + used, word, len);
        used += len;
        mn[used] = 0;
        wipe(word, sizeof(word));
    }
    if (!bip39_mnemonic_ok((const uint8_t *)mn, used)) {
        message("Bad checksum", "Restart mnemonic entry.", 1);
        wipe(mn, cap);
        return 0;
    }
    return 1;
}

static unsigned int passphrase_row_count(int layer, int row) {
    static const unsigned int counts[3][4] = {{10, 9, 7, 9}, {10, 9, 7, 9}, {10, 10, 10, 12}};
    if (row < (layer == 2 ? 4 : 3)) return counts[layer][row];
    return layer == 2 ? 2 : 3;
}

static unsigned int passphrase_char_rows(int layer) {
    return layer == 2 ? 4 : 3;
}

static void passphrase_input_screen(ui_screen_t *s, const char *pass, size_t len, int layer, int row, int col) {
    static const char *const layer_names[] = {"lower", "upper", "digits/symbols"};
    static const char *const keys[3][4] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm", NULL},
                                           {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", NULL},
                                           {"0123456789", "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|}~"}};
    char line[UI_COLS + 1];
    size_t start = len > 20 ? len - 20 : 0;

    memset(s, 0, sizeof(*s));
    snprintf(s->text[1], sizeof(s->text[1]), "Passphrase %u/%u", (unsigned int)len, BIP39_PASSPHRASE_MAX);
    if (start) snprintf(line, sizeof(line), "Text:[...%.*s]", 20, pass + start);
    else snprintf(line, sizeof(line), "Text:[%.*s]", (int)(len - start), pass);
    seed_input_line(s, 2, line);
    snprintf(line, sizeof(line), "Layer: %s", layer_names[layer]);
    seed_input_line(s, 3, line);
    unsigned int char_rows = passphrase_char_rows(layer);
    for (unsigned int r = 0; r < char_rows; r++) {
        keyboard_row(line, keys[layer][r], row == (int)r ? (unsigned int)col : passphrase_row_count(layer, r));
        seed_input_line(s, 4 + r, line);
    }
    unsigned int special_row = char_rows;
    if (row == (int)special_row && layer < 2) {
        snprintf(line, sizeof(line), "%s %s %s", col == 0 ? "[Shift]" : "Shift", col == 1 ? "[Space]" : "Space",
                 col == 2 ? "[Symbols]" : "Symbols");
    } else if (row == (int)special_row) {
        snprintf(line, sizeof(line), "%s %s", col == 0 ? "[Letters]" : "Letters", col == 1 ? "[Space]" : "Space");
    } else if (layer < 2) {
        snprintf(line, sizeof(line), "Shift       Space    Symbols");
    } else {
        snprintf(line, sizeof(line), "Letters               Space");
    }
    seed_input_line(s, 4 + char_rows, line);
    seed_input_line(s, 9, "Arrows move, Select types");
    seed_input_line(s, 10, "A: cancel  B: erase  C: done");
}

static int enter_passphrase(char pass[BIP39_PASSPHRASE_MAX + 1]) {
    static const char *const keys[3][4] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm", NULL},
                                           {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM", NULL},
                                           {"0123456789", "!\"#$%&'()*", "+,-./:;<=>", "?@[\\]^_`{|}~"}};
    ui_screen_t screen;
    size_t len = 0;
    int layer = 0, row = 0, col = 0;
    memset(pass, 0, BIP39_PASSPHRASE_MAX + 1);

    for (;;) {
        passphrase_input_screen(&screen, pass, len, layer, row, col);
        show(&screen);
        wipe(&screen, sizeof(screen));
        int key = wait_key();
        if (key == UI_KEY_A) {
            wipe(pass, BIP39_PASSPHRASE_MAX + 1);
            return 0;
        }
        if (key == UI_KEY_B) {
            if (len) pass[--len] = 0;
            continue;
        }
        if (key == UI_KEY_C) {
            pass[len] = 0;
            return 1;
        }
        if (key == UI_KEY_LEFT && col > 0) {
            col--;
        } else if (key == UI_KEY_RIGHT && col + 1 < (int)passphrase_row_count(layer, row)) {
            col++;
        } else if (key == UI_KEY_UP && row > 0) {
            row--;
            if (col >= (int)passphrase_row_count(layer, row)) col = (int)passphrase_row_count(layer, row) - 1;
        } else if (key == UI_KEY_DOWN && row < (int)passphrase_char_rows(layer)) {
            row++;
            if (col >= (int)passphrase_row_count(layer, row)) col = (int)passphrase_row_count(layer, row) - 1;
        } else if (key == UI_KEY_SELECT && row < (int)passphrase_char_rows(layer) && len < BIP39_PASSPHRASE_MAX) {
            pass[len++] = keys[layer][row][col];
        } else if (key == UI_KEY_SELECT && row == (int)passphrase_char_rows(layer)) {
            if (layer == 2) {
                if (col == 0) layer = 0, row = 0, col = 0;
                else if (len < BIP39_PASSPHRASE_MAX) pass[len++] = ' ';
            } else if (col == 0) {
                layer = 1 - layer;
            } else if (col == 1) {
                if (len < BIP39_PASSPHRASE_MAX) pass[len++] = ' ';
            } else {
                layer = 2, row = 0, col = 0;
            }
        }
    }
}

static int load_mnemonic(const char *mn) {
    char pass[BIP39_PASSPHRASE_MAX + 1];
    uint8_t seed[64];
    int ok = 0;

    if (enter_passphrase(pass))
        ok = bip39_seed_from_mnemonic((const uint8_t *)mn, strlen(mn), (const uint8_t *)pass, strlen(pass), seed) &&
             core_load_seed(seed);
    wipe(pass, sizeof(pass));
    wipe(seed, sizeof(seed));
    if (ok) printf("seed from input, fingerprint %08x\n", (unsigned int)core_fingerprint());
    return ok;
}
#endif

/* Reads a SeedQR and loads the seed. It is the secret itself, so it is handled here and never goes
 * through the parser */
static int scan_seed(void) {
    struct quirc *q;
    char status[UI_COLS + 1], mnemonic[256];
    static struct quirc_code code;
    static struct quirc_data data;
    int scanned = 0, ok = 0;

    if (!camera_ready) {
#if defined(__EMSCRIPTEN__)
        return message("No camera", "Connect camera to scan.", 1), 0;
#else
        return message("No camera", "Wire it up or enter mnemonic.", 1), 0;
#endif
    }
    if (!(q = quirc_new())) return message("Out of memory", NULL, 1), 0;
    if (quirc_resize(q, CAMERA_W, CAMERA_H) < 0) {
        quirc_destroy(q);
        return message("Out of memory", NULL, 1), 0;
    }
    snprintf(status, sizeof(status), "Scan SeedQR");

    while (!scanned) {
        uint8_t *img = quirc_begin(q, NULL, NULL);
        if (buttons_poll() >= 0) break;
        if (!camera_capture(img, 500)) continue;
        show_preview(img, status);
        quirc_end(q);
        for (int i = 0; i < quirc_count(q) && !scanned; i++) {
            quirc_extract(q, i, &code);
            if (quirc_decode(&code, &data)) {
                snprintf(status, sizeof(status), "QR found, cannot read");
                continue;
            }
            if (!seedqr_decode(data.payload, (size_t)data.payload_len, mnemonic, sizeof(mnemonic))) {
                snprintf(status, sizeof(status), "Not a valid SeedQR");
                continue;
            }
            scanned = 1;
        }
    }
    /* The camera frame and decoded payload contain the seed, so clear them before requesting a passphrase. */
    wipe(quirc_begin(q, NULL, NULL), (size_t)CAMERA_W * CAMERA_H);
    quirc_destroy(q);
    wipe(&code, sizeof(code));
    wipe(&data, sizeof(data));
    if (scanned) {
#if defined(__EMSCRIPTEN__)
        uint8_t seed[64];
        ok = bip39_seed_from_mnemonic((const uint8_t *)mnemonic, strlen(mnemonic), NULL, 0, seed) &&
             core_load_seed(seed);
        wipe(seed, sizeof(seed));
#else
        ok = load_mnemonic(mnemonic);
#endif
    }
    wipe(mnemonic, sizeof(mnemonic));
    if (ok) {
        printf("seed from SeedQR, fingerprint %08x\n", (unsigned int)core_fingerprint());
#if defined(__EMSCRIPTEN__)
        char xpub[CORE_XPUB_MAX], desc[CORE_DESC_MAX];
        if (core_account_xpub(84, 0, xpub, desc) == CORE_OK) printf("wallet descriptor: %s\n", desc);
#endif
    }
    return ok;
}

#if TEST_SEED
static int load_test_seed(void) {
    uint8_t seed[64];
    uint64_t t;
    int ok;

    message("TEST SEED", "Public BIP39 vector. Never send funds to this wallet.", 1);
    t = time_us_64();
    ok = bip39_seed_from_mnemonic((const uint8_t *)TEST_MNEMONIC, sizeof(TEST_MNEMONIC) - 1, NULL, 0, seed) &&
         core_load_seed(seed);
    wipe(seed, sizeof(seed));
    printf("seed %llu us, fingerprint %08x\n", (unsigned long long)(time_us_64() - t),
           (unsigned int)core_fingerprint());
#if defined(__EMSCRIPTEN__)
    if (ok) {
        char xpub[CORE_XPUB_MAX], desc[CORE_DESC_MAX];
        if (core_account_xpub(84, 0, xpub, desc) == CORE_OK) printf("wallet descriptor: %s\n", desc);
    }
#endif
    return ok;
}
#endif

static void seed_menu(void) {
    int sel;

#if defined(__EMSCRIPTEN__)
#if TEST_SEED
    static const char *const items[] = {"Scan SeedQR", "Use test seed (test only)"};
    ui_menu_init(&menu, "No seed loaded", items, 2);
#else
    static const char *const items[] = {"Scan SeedQR"};
    ui_menu_init(&menu, "No seed loaded", items, 1);
#endif
#else
    static const char *const items[] = {"Scan SeedQR", "Enter mnemonic", "Use test seed (test only)"};
    ui_menu_init(&menu, "No seed loaded", items, 2 + TEST_SEED);
#endif
    show(&menu.screen);
    for (;;) {
        sel = ui_menu_key(&menu, wait_key());
        if (sel == UI_MENU_PENDING) {
            show(&menu.screen);
            continue;
        }
        if (sel == 0) {
            if (scan_seed()) return;
            show(&menu.screen);
            continue;
        }
#if !defined(__EMSCRIPTEN__)
        if (sel == 1) {
            char mnemonic[256] = {0};
            int ok = enter_mnemonic(mnemonic, sizeof(mnemonic)) && load_mnemonic(mnemonic);
            wipe(mnemonic, sizeof(mnemonic));
            if (ok) return;
            show(&menu.screen);
            continue;
        }
#if TEST_SEED
        if (sel == 2 && load_test_seed()) return;
#endif
#else
#if TEST_SEED
        if (sel == 1 && load_test_seed()) return;
#endif
#endif
        show(&menu.screen);
    }
}

/* Shows the account xpub and descriptor. That is all the PC needs to watch the wallet, with no key. For
 * purpose 48 the descriptor is this cosigner's key expression, which the PC puts into wsh(sortedmulti()) */
static void show_xpub(unsigned int purpose) {
    static ui_screen_t s;
    static char xpub[CORE_XPUB_MAX], desc[CORE_DESC_MAX];

    if (core_account_xpub(purpose, 0, xpub, desc) != CORE_OK) return message("xpub failed", NULL, 1);
    ui_xpub(&s, xpub, core_fingerprint(), TESTNET, purpose);
    show(&s);
    printf("%s\n%s\n", xpub, desc);
    wait_key();

    /* The descriptor runs to about 145 characters, which fits QR v8, so one still image carries it */
    if (!ui_qr_set(desc)) return message("QR failed", NULL, 1);
    printf("descriptor QR: %d modules\n", ui_qr_modules());
    for (;;) {
        int key;
        draw_qr();
        key = wait_key();
        if (key != UI_KEY_UP && key != UI_KEY_DOWN) return;
        ui_qr_level(key == UI_KEY_UP ? 1 : -1);
        printf("qr level %d\n", ui_qr_level_get());
    }
}

/* The hash of the parser.wasm actually loaded. Matching it against the bitwasm-signer viewer is
 * how you confirm the device is running the same module you are */
static void show_parser_hash(void) {
    static ui_screen_t s;
    uint8_t h[32];

    sha256(parser_wasm, sizeof(parser_wasm), h);
    ui_hash(&s, "parser.wasm", (unsigned int)sizeof(parser_wasm), h);
    show(&s);
    printf("parser.wasm %u B sha256 ", (unsigned int)sizeof(parser_wasm));
    for (int i = 0; i < 32; i++) printf("%02x", h[i]);
    printf("\n");
    wait_key();
}

static void watch_only_menu(void) {
    static const char *const items[] = {"Single-sig (BIP84)", "Multisig (P2WSH)", "Back to main menu"};
    static ui_menu_t submenu;

    ui_menu_init(&submenu, "Watch-only wallet", items, 3);
    show(&submenu.screen);
    for (;;) {
        int sel = ui_menu_key(&submenu, wait_key());
        if (sel == UI_MENU_PENDING) {
            show(&submenu.screen);
            continue;
        }
        if (sel == UI_MENU_BACK || sel == 2) return;
        if (sel == 0) show_xpub(84);
        if (sel == 1) show_xpub(48);
        show(&submenu.screen);
    }
}

static int confirm_clear_seed(void) {
    static const char *const items[] = {"Keep seed", "Clear seed"};
    static ui_menu_t confirm;

    ui_menu_init(&confirm, "Clear seed?", items, 2);
    show(&confirm.screen);
    for (;;) {
        int sel = ui_menu_key(&confirm, wait_key());
        if (sel == UI_MENU_PENDING) {
            show(&confirm.screen);
            continue;
        }
        return sel == 1;
    }
}

static void main_menu(void) {
    static const char *const items[] = {
        "Scan PSBT",
        "Set up watch-only wallet",
        "Verify PSBT parser",
#if TEST_SEED
        "Sign demo PSBT (test only)",
#endif
        "Clear seed",
    };
    static char title[UI_COLS + 1];

    snprintf(title, sizeof(title), "FP %08x", (unsigned int)core_fingerprint());
    ui_menu_init(&menu, title, items, sizeof(items) / sizeof(items[0]));
    show(&menu.screen);
    for (;;) {
        int sel = ui_menu_key(&menu, wait_key());
        if (sel == 0) scan_and_sign();
        if (sel == 1) watch_only_menu();
        if (sel == 2) show_parser_hash();
#if TEST_SEED
        if (sel == 3) sign_flow(test_psbt, sizeof(test_psbt));
#endif
        if (sel == (TEST_SEED ? 4 : 3) && confirm_clear_seed()) {
            core_unload();
            printf("seed cleared (key wiped)\n");
            return;
        }
        show(&menu.screen);
    }
}

int main(void) {
    stdio_init_all();
#if !NO_LCD
    st7789_init();
#endif
    buttons_init();
    printf("\nbitwasm-signer-pico (%s)\n", TESTNET ? "testnet/signet" : "mainnet");

    memcpy(parser_wasm_rw, parser_wasm, sizeof(parser_wasm_rw));
    /* A TESTNET=1 build targets signet and testnet: tb1 addresses, derived under m/84'/1'/... */
    if (!core_init(TESTNET ? CORE_TESTNET : CORE_MAINNET) ||
        !parser_host_init(parser_wasm_rw, sizeof(parser_wasm_rw), pool, sizeof(pool))) {
        message("Init failed", NULL, 1);
        return 1;
    }
    camera_bus_init(&camera_ov7670);
    camera_ready = camera_init(&camera_ov7670);
    printf("camera: %s\n", camera_ready ? "ready" : "not connected");
    for (;;) {
        seed_menu();
        main_menu();
    }
}
