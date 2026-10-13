#include "ui.h"
#include <string.h>
#include <stdio.h>
#include "font8x16.h"
#include "qrcodegen.h"

#define RGB565(r, g, b) (uint16_t)(((r) >> 3) << 11 | ((g) >> 2) << 5 | (b) >> 3)
#define C_TEXT RGB565(230, 230, 230)
#define C_TITLE RGB565(255, 170, 0)
#define C_SEND RGB565(255, 110, 80)
#define C_OURS RGB565(90, 220, 120)
#define C_HINT RGB565(140, 140, 140)
#define C_BG RGB565(0, 0, 0)
#define C_PANEL RGB565(17, 18, 22)
#define C_BORDER RGB565(48, 50, 56)
#define C_ACTIVE RGB565(33, 27, 16)
#define C_SEND_BG RGB565(27, 13, 11)
#define C_SEND_BORDER RGB565(87, 53, 45)

typedef struct {
    ui_screen_t *s;
    int row;
} writer_t;

/* The application name, right-aligned, on the first line of every screen */
static void put_header(writer_t *w);

static void put(writer_t *w, uint16_t color, const char *text) {
    if (w->row >= UI_ROWS) return;
    strncpy(w->s->text[w->row], text, UI_COLS);
    w->s->text[w->row][UI_COLS] = 0;
    w->s->color[w->row++] = color;
}

static void put_header(writer_t *w) {
    char line[UI_COLS + 1];
    size_t n = strlen(UI_APP_NAME);
    memset(line, ' ', UI_COLS);
    line[UI_COLS] = 0;
    if (n < UI_COLS) memcpy(line + (UI_COLS - 1 - n), UI_APP_NAME, n); /* one column of margin at the right */
    put(w, C_HINT, line);
}

/* A long string, an address or a script in hex, is wrapped in full rather than truncated */
static void put_wrapped(writer_t *w, uint16_t color, const char *text) {
    size_t n = strlen(text);
    char chunk[UI_COLS + 1];
    for (size_t i = 0; i < n; i += UI_COLS) {
        size_t k = n - i < UI_COLS ? n - i : UI_COLS;
        memcpy(chunk, text + i, k);
        chunk[k] = 0;
        put(w, color, chunk);
    }
}

static void put_amount(writer_t *w, uint16_t color, const char *label, uint64_t sats, int show_sats) {
    char btc[21], line[UI_COLS + 1];
    if (show_sats) {
        snprintf(line, sizeof(line), "%s%llu sat", label, (unsigned long long)sats);
        put(w, color, line);
        return;
    }
    size_t n = strlen(label);
    core_format_btc(sats, btc);
    memcpy(line, label, n);
    memcpy(line + n, btc, strlen(btc) + 1);
    strncat(line, " BTC", UI_COLS - strlen(line));
    put(w, color, line);
}

static void put_number(writer_t *w, uint16_t color, const char *label, unsigned int v) {
    char line[UI_COLS + 1];
    size_t n = strlen(label);
    memcpy(line, label, n);
    if (v >= 10) line[n++] = (char)('0' + v / 10);
    line[n++] = (char)('0' + v % 10);
    line[n] = 0;
    put(w, color, line);
}

static void put_counter(writer_t *w, uint16_t color, const char *label, unsigned int a, unsigned int b) {
    char line[UI_COLS + 1];
    size_t n = strlen(label), o = n;
    memcpy(line, label, n);
    if (a >= 10) line[o++] = (char)('0' + a / 10);
    line[o++] = (char)('0' + a % 10);
    line[o++] = '/';
    if (b >= 10) line[o++] = (char)('0' + b / 10);
    line[o++] = (char)('0' + b % 10);
    line[o] = 0;
    put(w, color, line);
}

void ui_review_init(ui_review_t *r, const core_display_t *d) {
    static const char *owner[] = {"Send to", "Change", "Self-transfer"};
    unsigned int n_send = 0;
    writer_t w;

    memset(r, 0, sizeof(*r));
    for (unsigned int i = 0; i < d->n_outputs; i++) n_send += d->outputs[i].owner == CORE_OUT_EXTERNAL;

    w = (writer_t){&r->screens[r->n++], 0};
    put_header(&w);
    put(&w, C_TITLE, "Review transaction");
    put(&w, C_TEXT, "");
    put_amount(&w, C_SEND, "Spend ", d->spend, 0);
    put_amount(&w, C_TEXT, "Fee ", d->fee, 1);
    put(&w, C_TEXT, "");
    put_number(&w, C_TEXT, "Outputs:  ", d->n_outputs);
    put_number(&w, C_TEXT, "External: ", n_send);
    w.row = UI_ROWS - 1;
    put(&w, C_HINT, "DOWN: review outputs");

    for (unsigned int i = 0; i < d->n_outputs; i++) {
        const core_display_output_t *o = &d->outputs[i];
        uint16_t color = o->owner == CORE_OUT_EXTERNAL ? C_SEND : C_OURS;
        w = (writer_t){&r->screens[r->n++], 0};
        put_header(&w);
        put_counter(&w, C_TITLE, "Output ", i + 1, d->n_outputs);
        put(&w, color, owner[o->owner]);
        put_amount(&w, color, "", o->amount, 0);
        put(&w, C_TEXT, "");
        put(&w, C_HINT,
            o->text_kind == CORE_TEXT_ADDRESS     ? "Address:"
            : o->text_kind == CORE_TEXT_OP_RETURN ? "OP_RETURN data (hex):"
                                                  : "Script (hex):");
        put_wrapped(&w, C_TEXT, o->text);
        w.row = UI_ROWS - 1;
        put(&w, C_HINT, "DOWN: next screen");
    }

    w = (writer_t){&r->screens[r->n++], 0};
    put_header(&w);
    put(&w, C_TITLE, "Sign this transaction?");
    put(&w, C_TEXT, "");
    put_amount(&w, C_SEND, "Spend ", d->spend, 0);
    put_amount(&w, C_TEXT, "Fee ", d->fee, 1);
    w.row = UI_ROWS - 2;
    put(&w, C_HINT, "PUSH: sign");
    put(&w, C_HINT, "DOWN: cancel option");

    /* The last screen is the cancel screen, so two buttons are enough to reject; it wraps to the first */
    w = (writer_t){&r->screens[r->n++], 0};
    put_header(&w);
    put(&w, C_TITLE, "Cancel transaction?");
    put(&w, C_TEXT, "");
    put(&w, C_TEXT, "Nothing is signed.");
    w.row = UI_ROWS - 2;
    put(&w, C_HINT, "PUSH: cancel");
    put(&w, C_HINT, "DOWN: return to start");
    r->seen = 1;
}

int ui_review_key(ui_review_t *r, int key) {
    /* Confirmation is second from last and cancel is last. Signing needs every screen to have been
     * seen, the cancel screen aside */
    unsigned int sign = r->n - 2, must_see = (1u << (r->n - 1)) - 1;
    if (key == UI_KEY_A) return UI_REJECTED;
    if (key == UI_KEY_RIGHT || key == UI_KEY_DOWN) r->cur = (r->cur + 1) % r->n;
    if (key == UI_KEY_LEFT || key == UI_KEY_UP) r->cur = (r->cur + r->n - 1) % r->n;
    r->seen |= 1u << r->cur;
    if (key == UI_KEY_PUSH && r->cur == r->n - 1) return UI_REJECTED;
    if (key == UI_KEY_PUSH && r->cur == sign && (r->seen & must_see) == must_see) return UI_APPROVED;
    return UI_PENDING;
}

static void menu_draw(ui_menu_t *m) {
    writer_t w = {&m->screen, 0};
    memset(&m->screen, 0, sizeof(m->screen));
    put_header(&w);
    put(&w, C_TITLE, m->title);
    put(&w, C_TEXT, "");
    for (unsigned int i = 0; i < m->n; i++) {
        char line[UI_COLS + 1];
        line[0] = i == m->cur ? '>' : ' ';
        line[1] = ' ';
        strncpy(line + 2, m->items[i], UI_COLS - 2);
        line[UI_COLS] = 0;
        put(&w, i == m->cur ? C_TITLE : C_TEXT, line);
    }
    w.row = UI_ROWS - 1;
    put(&w, C_HINT, "DOWN: move   PUSH: select");
}

void ui_menu_init(ui_menu_t *m, const char *title, const char *const *items, unsigned int n) {
    m->title = title;
    m->items = items;
    m->n = n > UI_MENU_MAX ? UI_MENU_MAX : n;
    m->cur = 0;
    menu_draw(m);
}

int ui_menu_key(ui_menu_t *m, int key) {
    if (key == UI_KEY_PUSH || key == UI_KEY_RIGHT) return (int)m->cur;
    if (key == UI_KEY_A || key == UI_KEY_LEFT) return UI_MENU_BACK;
    if (key == UI_KEY_DOWN) m->cur = (m->cur + 1) % m->n;
    if (key == UI_KEY_UP) m->cur = (m->cur + m->n - 1) % m->n;
    menu_draw(m);
    return UI_MENU_PENDING;
}

void ui_message(ui_screen_t *s, const char *title, const char *body, int warn) {
    writer_t w = {s, 0};
    memset(s, 0, sizeof(*s));
    put_header(&w);
    put(&w, warn ? C_SEND : C_TITLE, title);
    put(&w, C_TEXT, "");
    if (body) put_wrapped(&w, C_TEXT, body);
    w.row = UI_ROWS - 1;
    put(&w, C_HINT, "PUSH: ok");
}

static int text_starts(const char *text, const char *prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int is_action_line(const char *text) {
    static const char *const prefixes[] = {"PUSH:", "DOWN:", "UP:", "LEFT:", "RIGHT:", "ANY KEY", "A:", "B:", "PRESS"};
    for (unsigned int i = 0; i < sizeof(prefixes) / sizeof(*prefixes); i++)
        if (text_starts(text, prefixes[i])) return 1;
    return 0;
}

static void fill_rect_line(uint16_t line[UI_W], int y, int x, int top, int width, int height, uint16_t color) {
    if (y < top || y >= top + height) return;
    if (x < 0) width += x, x = 0;
    if (x + width > UI_W) width = UI_W - x;
    for (int i = 0; i < width; i++) line[x + i] = color;
}

static void draw_text_line(uint16_t line[UI_W], int y, int x, int top, int glyph_w, int glyph_h, const char *text,
                           uint16_t color) {
    if (y < top || y >= top + glyph_h) return;
    int gy = (y - top) * FONT_H / glyph_h;
    for (size_t c = 0; text[c]; c++) {
        unsigned int ch = (unsigned char)text[c];
        uint8_t bits = font8x16[(ch >= 0x20 && ch <= 0x7e ? ch : '?') - 0x20][gy];
        for (int b = 0; b < glyph_w; b++) {
            int sx = b * FONT_W / glyph_w, px = x + (int)c * glyph_w + b;
            if (px >= 0 && px < UI_W && (bits & (0x80 >> sx))) line[px] = color;
        }
    }
}

static void draw_small(uint16_t line[UI_W], int y, int x, int top, const char *text, uint16_t color) {
    draw_text_line(line, y, x, top, FONT_W, FONT_H, text, color);
}

static void draw_normal(uint16_t line[UI_W], int y, int x, int top, const char *text, uint16_t color) {
    draw_text_line(line, y, x, top, 8, 16, text, color);
}

static void draw_dot(uint16_t line[UI_W], int y, int cx, int cy, int radius, uint16_t color) {
    if (y < cy - radius || y > cy + radius) return;
    int dy = y - cy;
    for (int x = cx - radius; x <= cx + radius; x++) {
        int dx = x - cx;
        if (x >= 0 && x < UI_W && dx * dx + dy * dy <= radius * radius) line[x] = color;
    }
}

static const char *screen_section(const char *title, int ready, int setup, int summary, int output, int approval,
                                  int cancel) {
    if (setup) return "WALLET SETUP";
    if (ready) return "JITSU SIGNER";
    if (summary) return "SIGN PSBT / SUMMARY";
    if (output) return title;
    if (approval) return "FINAL REVIEW";
    if (cancel) return "SIGN PSBT / CANCEL";
    if (strstr(title, "xpub")) return "WALLET INFO";
    if (strstr(title, "hash")) return "DEVICE INFO";
    return "JITSU SIGNER";
}

static int is_menu_row(const char *text) {
    return text[0] == '>' || (text[0] == ' ' && text[1] == ' ' && text[2] != 0);
}

static void draw_menu(uint16_t line[UI_W], int y, const ui_screen_t *s, int top) {
    int index = 0;
    for (int r = 2; r < UI_ROWS - 1; r++) {
        const char *text = s->text[r];
        if (!is_menu_row(text)) continue;
        int active = text[0] == '>';
        const char *label = text + 2;
        int row_y = top + index * 19;
        fill_rect_line(line, y, 12, row_y, 216, 17, active ? C_ACTIVE : C_PANEL);
        fill_rect_line(line, y, 12, row_y, 2, 17, active ? C_TITLE : C_BORDER);
        draw_small(line, y, 21, row_y + 1, label, active ? C_TITLE : C_TEXT);
        index++;
    }
}

static void draw_footer(uint16_t line[UI_W], int y, const ui_screen_t *s) {
    const char *actions[2] = {NULL, NULL};
    int count = 0;
    for (int r = 0; r < UI_ROWS && count < 2; r++)
        if (s->text[r][0] && is_action_line(s->text[r])) actions[count++] = s->text[r];
    fill_rect_line(line, y, 0, 207, UI_W, 33, C_PANEL);
    fill_rect_line(line, y, 10, 207, UI_W - 20, 1, C_BORDER);
    if (count == 1) draw_small(line, y, 12, 215, actions[0], C_HINT);
    if (count == 2) {
        draw_small(line, y, 12, 208, actions[0], C_HINT);
        draw_small(line, y, 12, 224, actions[1], C_HINT);
    }
}

static const char *row_value(const ui_screen_t *s, const char *prefix, uint16_t color) {
    size_t n = strlen(prefix);
    for (int r = 2; r < UI_ROWS - 1; r++)
        if (s->color[r] == color && strncmp(s->text[r], prefix, n) == 0) return s->text[r] + n;
    return NULL;
}

static int row_with_text(const ui_screen_t *s, const char *text) {
    for (int r = 2; r < UI_ROWS - 1; r++)
        if (strcmp(s->text[r], text) == 0) return r;
    return -1;
}

static void draw_card(uint16_t line[UI_W], int y, int x, int top, int width, int height, uint16_t bg, uint16_t border) {
    fill_rect_line(line, y, x, top, width, height, bg);
    if (y >= top && y < top + height) {
        line[x] = border;
        line[x + width - 1] = border;
    }
    if (y == top || y == top + height - 1)
        for (int i = 0; i < width; i++) line[x + i] = border;
}

void ui_render_line(const ui_screen_t *s, int y, uint16_t line[UI_W]) {
    const char *title = s->text[1];
    int setup = strcmp(title, "No seed loaded") == 0;
    int ready = text_starts(title, "Signet fp ") || text_starts(title, "Signer fp ");
    int summary = strcmp(title, "Review transaction") == 0;
    int output = text_starts(title, "Output ");
    int approval = text_starts(title, "Sign this transaction");
    int cancel = text_starts(title, "Cancel transaction");
    int net_len;
    const char *network;
    char heading[UI_COLS + 1];

    for (int x = 0; x < UI_W; x++) line[x] = C_BG;
    fill_rect_line(line, y, 6, 6, UI_W - 12, 1, C_BORDER);
    fill_rect_line(line, y, 6, UI_H - 7, UI_W - 12, 1, C_BORDER);
    if (y >= 6 && y < UI_H - 6) line[6] = line[UI_W - 7] = C_BORDER;
#if defined(TESTNET) && TESTNET
    network = "SIGNET";
#else
    network = "MAINNET";
#endif
    net_len = (int)strlen(network) * FONT_W;
    draw_small(line, y, 13, 8, screen_section(title, ready, setup, summary, output, approval, cancel), C_HINT);
    draw_small(line, y, UI_W - 13 - net_len, 8, network, C_HINT);
    fill_rect_line(line, y, 12, 27, UI_W - 24, 1, C_BORDER);

    if (ready) strcpy(heading, "Signer ready");
    else if (output) strcpy(heading, "Check recipient");
    else strcpy(heading, title);
    draw_normal(line, y, 13, 32, heading, s->color[1] == C_SEND ? C_SEND : C_TITLE);

    if (setup || ready) {
        if (ready) {
            draw_small(line, y, 14, 56, "FINGERPRINT", C_HINT);
            draw_normal(line, y, 141, 52, title + 10, C_OURS);
            draw_dot(line, y, 17, 82, 3, C_OURS);
            draw_small(line, y, 26, 74, "Key is in RAM", C_OURS);
            draw_menu(line, y, s, 94);
        } else {
            draw_menu(line, y, s, 67);
        }
    } else if (summary || approval) {
        const char *spend = row_value(s, "Spend ", C_SEND);
        const char *fee = row_value(s, "Fee", C_TEXT);
        const char *outputs = row_value(s, "Outputs:", C_TEXT);
        const char *external = row_value(s, "External:", C_TEXT);
        draw_card(line, y, 12, 56, 216, 52, C_SEND_BG, C_SEND_BORDER);
        draw_small(line, y, 20, 60, "YOU SEND", C_HINT);
        if (spend) draw_normal(line, y, 20, 80, spend, C_SEND);
        draw_card(line, y, 12, 115, 102, 72, C_PANEL, C_BORDER);
        draw_card(line, y, 126, 115, 102, 72, C_PANEL, C_BORDER);
        draw_small(line, y, 20, 119, "NETWORK FEE", C_HINT);
        if (fee) draw_small(line, y, 20, 140, fee, C_TEXT);
        draw_small(line, y, 134, 119, "OUTPUTS", C_HINT);
        if (outputs) draw_small(line, y, 134, 140, outputs, C_TEXT);
        if (external) draw_small(line, y, 134, 157, external, C_TEXT);
    } else if (output) {
        int owner_row = -1, amount_row = -1, label_row = -1;
        const char *address_label = "ADDRESS";
        for (int r = 2; r < UI_ROWS - 1; r++) {
            const char *text = s->text[r];
            if (!strcmp(text, "Send to") || !strcmp(text, "Change") || !strcmp(text, "Self-transfer")) owner_row = r;
            else if (text[0] && (s->color[r] == C_SEND || s->color[r] == C_OURS)) amount_row = r;
            else if (s->color[r] == C_HINT && text[0] && !is_action_line(text)) {
                label_row = r;
                if (text_starts(text, "OP_RETURN")) address_label = "OP_RETURN DATA";
                else if (text_starts(text, "Script")) address_label = "SCRIPT";
            }
        }
        if (owner_row >= 0) {
            int external = s->color[owner_row] == C_SEND;
            draw_dot(line, y, 17, 63, 3, external ? C_SEND : C_OURS);
            draw_small(line, y, 26, 55, external ? "External / Send to" : s->text[owner_row],
                       external ? C_SEND : C_OURS);
        }
        draw_card(line, y, 12, 77, 216, 47, C_PANEL, C_BORDER);
        draw_small(line, y, 20, 79, "AMOUNT", C_HINT);
        if (amount_row >= 0) draw_normal(line, y, 20, 99, s->text[amount_row], s->color[amount_row]);
        draw_card(line, y, 12, 130, 216, 74, C_PANEL, C_BORDER);
        draw_small(line, y, 20, 132, address_label, C_HINT);
        if (label_row >= 0) {
            char part[UI_COLS + 1];
            int line_y = 151, used = 0;
            for (int r = label_row + 1; r < UI_ROWS - 1 && line_y < 195; r++) {
                if (!s->text[r][0] || is_action_line(s->text[r])) break;
                for (size_t c = 0; s->text[r][c] && line_y < 195; c++) {
                    part[used++] = s->text[r][c];
                    if (used == 26) {
                        part[used] = 0;
                        draw_small(line, y, 16, line_y, part, C_TEXT);
                        line_y += 17;
                        used = 0;
                    }
                }
            }
            if (used && line_y < 195) part[used] = 0, draw_small(line, y, 16, line_y, part, C_TEXT);
        }
    } else if (cancel) {
        int row = row_with_text(s, "Nothing is signed.");
        draw_card(line, y, 12, 64, 216, 53, C_SEND_BG, C_SEND_BORDER);
        if (row >= 0) draw_normal(line, y, 20, 82, s->text[row], C_SEND);
    } else {
        int line_y = 57;
        for (int r = 2; r < UI_ROWS - 1 && line_y < 198; r++) {
            if (!s->text[r][0] || is_action_line(s->text[r]) || is_menu_row(s->text[r])) continue;
            size_t length = strlen(s->text[r]);
            for (size_t start = 0; start < length && line_y < 198; start += 28) {
                char part[29];
                size_t count = length - start < 28 ? length - start : 28;
                memcpy(part, s->text[r] + start, count);
                part[count] = 0;
                draw_small(line, y, 8, line_y, part, s->color[r] ? s->color[r] : C_TEXT);
                line_y += 17;
            }
        }
    }
    draw_footer(line, y, s);
}

void ui_render_scan_line(const uint8_t *img, int camera_w, int camera_h, const char *status, int y,
                         uint16_t line[UI_W]) {
    int source_w = camera_h * UI_W / UI_H;
    int crop_x = (camera_w - source_w) / 2;
    int sy = y * camera_h / UI_H;
    int cy = 112, radius = 80;

    for (int x = 0; x < UI_W; x++) {
        int sx = crop_x + x * source_w / UI_W;
        int value = img[sy * camera_w + sx];
        int dx = x - UI_W / 2, dy = y - cy, distance = dx * dx + dy * dy;
        int guide = (x == 54 || x == 185 || y == 46 || y == 177) && x >= 54 && x <= 185 && y >= 46 && y <= 177;
        int ring = distance >= (radius - 1) * (radius - 1) && distance <= (radius + 1) * (radius + 1) &&
                   (((x + y) / 5) & 1) == 0;
        if (guide || ring) {
            int red = value * 3 / 5;
            int green = value * 3 / 5 + 102;
            int blue = value * 3 / 5 + 40;
            line[x] = RGB565(red, green, blue);
        } else {
            line[x] = RGB565(value, value, value);
        }
    }
    fill_rect_line(line, y, 0, 214, UI_W, 26, C_PANEL);
    fill_rect_line(line, y, 0, 214, UI_W, 1, C_BORDER);
    draw_dot(line, y, 10, 227, 3, C_TITLE);
    draw_small(line, y, 21, 222, status ? status : "Waiting for a valid QR", C_TEXT);
}

static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(UI_QR_MAX_VERSION)];

int ui_qr_set(const char *text) {
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(UI_QR_MAX_VERSION)];
    return qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, 1, UI_QR_MAX_VERSION, qrcodegen_Mask_AUTO, true);
}

int ui_qr_modules(void) {
    return qrcodegen_getSize(qr);
}

/* The backlight is wired straight to 3V3, so brightness is changed through the white level instead.
 * Too bright saturates the camera's exposure and cannot be read */
static const uint16_t qr_white[] = {RGB565(255, 255, 255), RGB565(190, 190, 190), RGB565(130, 130, 130),
                                    RGB565(80, 80, 80)};
static unsigned int qr_level = 1;

void ui_qr_level(int delta) {
    unsigned int n = sizeof(qr_white) / sizeof(*qr_white);
    qr_level = (qr_level + (unsigned int)(delta > 0 ? 1 : n - 1)) % n;
}

int ui_qr_level_get(void) {
    return (int)qr_level;
}

void ui_qr_render_line(int y, uint16_t line[UI_W]) {
    int size = qrcodegen_getSize(qr), scale = UI_W / (size + 8), off = (UI_W - size * scale) / 2;
    int my = (y - off) / scale;
    for (int x = 0; x < UI_W; x++) {
        int mx = (x - off) / scale;
        int dark = y >= off && x >= off && my < size && mx < size && qrcodegen_getModule(qr, mx, my);
        line[x] = dark ? 0x0000 : qr_white[qr_level];
    }
}

/* The account xpub, shown in full with nothing trimmed from either end, so it can be compared by eye */
void ui_xpub(ui_screen_t *s, const char *xpub, uint32_t fp, int testnet, unsigned int purpose) {
    writer_t w = {s, 0};
    char line[UI_COLS + 1];
    size_t n = strlen(xpub);

    memset(s, 0, sizeof(*s));
    put_header(&w);
    put(&w, C_TITLE, purpose == 48 ? "Multisig xpub (P2WSH)" : "Account xpub (watch-only)");
    snprintf(line, sizeof(line), "m/%uh/%dh/0h%s  fp %08lx", purpose, testnet ? 1 : 0, purpose == 48 ? "/2h" : "",
             (unsigned long)fp);
    put(&w, C_HINT, line);
    put(&w, C_TEXT, "");
    for (size_t i = 0; i < n; i += UI_COLS) {
        snprintf(line, sizeof(line), "%.*s", (int)UI_COLS, xpub + i);
        put(&w, C_OURS, line);
    }
    w.row = UI_ROWS - 1;
    put(&w, C_HINT, "PUSH: show QR");
}

/* The hash of the loaded wasm, grouped in eights because it is compared by eye with the browser's */
void ui_hash(ui_screen_t *s, const char *name, unsigned int len, const uint8_t h[32]) {
    writer_t w = {s, 0};
    char line[UI_COLS + 1];

    memset(s, 0, sizeof(*s));
    put_header(&w);
    put(&w, C_TITLE, "Parser hash");
    snprintf(line, sizeof(line), "%s  %u B", name, len);
    put(&w, C_HINT, line);
    put(&w, C_TEXT, "");
    for (int r = 0; r < 4; r++) {
        const uint8_t *p = h + r * 8;
        snprintf(line, sizeof(line), "%02x%02x%02x%02x %02x%02x%02x%02x", p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                 p[7]);
        put(&w, C_OURS, line);
    }
    put(&w, C_TEXT, "");
    put_wrapped(&w, C_HINT, "Must match the viewer page");
    w.row = UI_ROWS - 1;
    put(&w, C_HINT, "PUSH: ok");
}
