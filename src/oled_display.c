#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/neutrino.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>
#include <hw/i2c.h>

/* ── OLED ── */
#define OLED_ADDR        0x3C
#define I2C_DEV          "/dev/i2c1"
#define OLED_W           128
#define OLED_PAGES       8
#define OLED_CHANNEL_NAME "/dev/oled_channel"

/* ── THRESHOLDS ── */
#define CPU_WARN    55
#define CPU_CRIT    75
#define MEM_WARN    55
#define MEM_CRIT    75
#define TEMP_WARN   42
#define TEMP_CRIT   50

/* message struct - must match cpu.c */
typedef struct {
    uint16_t type;
    int cpu;
    int mem;
    int temp;
    char uptime[16];
    int fan;
    int alert;
    int games_running;
} OledMsg;

int oled_fd = -1;

/* ════════════════════════════════
   FONT - copied from cpu.c
   ════════════════════════════════ */
const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* SPACE */
    {0x7E,0x11,0x11,0x11,0x7E}, /* A */
    {0x7F,0x49,0x49,0x49,0x36}, /* B */
    {0x3E,0x41,0x41,0x41,0x22}, /* C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* D */
    {0x7F,0x49,0x49,0x49,0x41}, /* E */
    {0x7F,0x09,0x09,0x09,0x01}, /* F */
    {0x3E,0x41,0x49,0x49,0x7A}, /* G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* H */
    {0x00,0x41,0x7F,0x41,0x00}, /* I */
    {0x20,0x40,0x41,0x3F,0x01}, /* J */
    {0x7F,0x08,0x14,0x22,0x41}, /* K */
    {0x7F,0x40,0x40,0x40,0x40}, /* L */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* O */
    {0x7F,0x09,0x09,0x09,0x06}, /* P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* R */
    {0x46,0x49,0x49,0x49,0x31}, /* S */
    {0x01,0x01,0x7F,0x01,0x01}, /* T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* V */
    {0x3F,0x40,0x38,0x40,0x3F}, /* W */
    {0x63,0x14,0x08,0x14,0x63}, /* X */
    {0x07,0x08,0x70,0x08,0x07}, /* Y */
    {0x61,0x51,0x49,0x45,0x43}, /* Z */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 9 */
    {0x36,0x36,0x00,0x00,0x00}, /* : */
    {0x23,0x13,0x08,0x64,0x62}, /* % */
    {0x00,0x00,0x5F,0x00,0x00}, /* ! */
    {0x01,0x01,0x01,0x01,0x01}, /* - */
    {0x06,0x09,0x09,0x06,0x00}, /* @ = degree */
};

int char_to_index(char c) {
    if (c == ' ')             return 0;
    if (c >= 'A' && c <= 'Z') return 1  + (c - 'A');
    if (c >= 'a' && c <= 'z') return 1  + (c - 'a');
    if (c >= '0' && c <= '9') return 27 + (c - '0');
    if (c == ':') return 37;
    if (c == '%') return 38;
    if (c == '!') return 39;
    if (c == '-') return 40;
    if (c == '@') return 41;
    return 0;
}

/* ════════════════════════════════
   OLED DRIVER
   ════════════════════════════════ */
void oled_send(uint8_t control, uint8_t value) {
    uint8_t buf[2] = {control, value};
    iov_t siov[2];
    i2c_send_t hdr;
    hdr.slave.addr = OLED_ADDR;
    hdr.slave.fmt  = I2C_ADDRFMT_7BIT;
    hdr.len = 2; hdr.stop = 1;
    SETIOV(&siov[0], &hdr, sizeof(hdr));
    SETIOV(&siov[1], buf,  sizeof(buf));
    devctlv(oled_fd, DCMD_I2C_SEND, 2, 0, siov, NULL, NULL);
}
void oled_cmd(uint8_t cmd)   { oled_send(0x00, cmd); }
void oled_data(uint8_t data) { oled_send(0x40, data); }

uint8_t oled_buf[OLED_PAGES][OLED_W];

void oled_buf_clear() { memset(oled_buf, 0, sizeof(oled_buf)); }

void oled_buf_flush() {
    for (int p = 0; p < OLED_PAGES; p++) {
        oled_cmd(0xB0 + p);
        oled_cmd(0x00); oled_cmd(0x10);
        for (int i = 0; i < OLED_W; i++) oled_data(oled_buf[p][i]);
    }
}

void oled_buf_pixel(int x, int y, int on) {
    if (x < 0 || x >= OLED_W || y < 0 || y >= 64) return;
    int page = y / 8, bit = y % 8;
    if (on) oled_buf[page][x] |=  (1 << bit);
    else    oled_buf[page][x] &= ~(1 << bit);
}

void oled_buf_rect(int x, int y, int w, int h, int fill) {
    for (int i = x; i < x+w; i++)
        for (int j = y; j < y+h; j++)
            oled_buf_pixel(i, j, fill);
}

void oled_buf_hline(int x, int y, int w) {
    for (int i = x; i < x+w; i++) oled_buf_pixel(i, y, 1);
}

void oled_buf_text(const char *text, int page, int col) {
    while (*text) {
        if (col + 6 > OLED_W) break;
        int idx = char_to_index(*text);
        for (int i = 0; i < 5; i++)
            oled_buf[page][col++] = font5x7[idx][i];
        oled_buf[page][col++] = 0x00;
        text++;
    }
}

void oled_init() {
    oled_cmd(0xAE);
    oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x3F);
    oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40);
    oled_cmd(0x8D); oled_cmd(0x14);
    oled_cmd(0x20); oled_cmd(0x00);
    oled_cmd(0xA1); oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x12);
    oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0xF1);
    oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0xA4); oled_cmd(0xA6);
    oled_cmd(0xAF);
}

/* ════════════════════════════════
   STATUS
   ════════════════════════════════ */
typedef enum { STATUS_OK, STATUS_WARN, STATUS_CRIT } Status;
Status get_status(int v, int w, int c) {
    if (v >= c) return STATUS_CRIT;
    if (v >= w) return STATUS_WARN;
    return STATUS_OK;
}
const char* status_str(Status s) {
    if (s == STATUS_CRIT) return "CRIT";
    if (s == STATUS_WARN) return "WARN";
    return "OK  ";
}

/* ════════════════════════════════
   DRAW BAR
   ════════════════════════════════ */
void draw_bar_buf(int val, int page, int col, int width) {
    int filled = (val * width) / 100;
    oled_buf[page][col] = 0x7E;
    for (int i = 0; i < width; i++)
        oled_buf[page][col+1+i] = (i < filled) ? 0x3C : 0x04;
    oled_buf[page][col+width+1] = 0x7E;
}

/* ════════════════════════════════
   DRAW DIAGNOSTICS
   ════════════════════════════════ */
void update_oled_diagnostics(OledMsg *msg) {
    char buf[32];
    Status cs = get_status(msg->cpu,  CPU_WARN, CPU_CRIT);
    Status ms = get_status(msg->mem,  MEM_WARN, MEM_CRIT);
    Status ts = get_status(msg->temp, TEMP_WARN, TEMP_CRIT);

    oled_buf_clear();

    if (ts == STATUS_CRIT) {
        oled_buf_text("!! CRITICAL !!", 0, 5);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "TEMP: %d@C", msg->temp);
        oled_buf_text(buf, 2, 20);
        draw_bar_buf(msg->temp, 3, 0, 110);
        oled_buf_text("FAN ON DANGER", 4, 8);
        oled_buf_hline(0, 41, 128);
        sprintf(buf, "CPU:%d%% MEM:%d%%", msg->cpu, msg->mem);
        oled_buf_text(buf, 6, 0);
        sprintf(buf, "UP:%s", msg->uptime);
        oled_buf_text(buf, 7, 0);
    } else if (ts == STATUS_WARN) {
        oled_buf_text("FAN ON", 0, 32);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "TEMP: %d@C", msg->temp);
        oled_buf_text(buf, 2, 20);
        draw_bar_buf(msg->temp, 3, 0, 110);
        oled_buf_text("ABOVE 42@C", 4, 18);
        oled_buf_hline(0, 41, 128);
        sprintf(buf, "CPU:%d%% MEM:%d%%", msg->cpu, msg->mem);
        oled_buf_text(buf, 6, 0);
        sprintf(buf, "UP:%s", msg->uptime);
        oled_buf_text(buf, 7, 0);
    } else {
        oled_buf_text("QNX DIAGNOSTICS", 0, 0);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "CPU:%3d%% %s", msg->cpu, status_str(cs));
        oled_buf_text(buf, 2, 0);
        draw_bar_buf(msg->cpu, 3, 0, 110);
        sprintf(buf, "MEM:%3d%% %s", msg->mem, status_str(ms));
        oled_buf_text(buf, 4, 0);
        draw_bar_buf(msg->mem, 5, 0, 110);
        sprintf(buf, "GAMES:%d/2", msg->games_running);
        oled_buf_text(buf, 6, 0);
        sprintf(buf, "UP:%s T:%d@", msg->uptime, msg->temp);
        oled_buf_text(buf, 7, 0);
    }

    oled_buf_flush();
}

/* ════════════════════════════════
   MAIN
   ════════════════════════════════ */
int main() {
    printf("=== OLED DISPLAY PROCESS ===\n");

    /* open I2C */
    oled_fd = open(I2C_DEV, O_RDWR);
    if (oled_fd < 0) {
        printf("[ERROR] Cannot open %s\n", I2C_DEV);
        return -1;
    }
    printf("[OK] I2C opened\n");

    oled_init();
    oled_buf_clear();
    oled_buf_flush();

    /* boot screen */
    oled_buf_text("OLED PROCESS", 2, 10);
    oled_buf_text("READY...", 4, 30);
    oled_buf_flush();
    sleep(1);

    /* create QNX channel */
    int chid = ChannelCreate(0);
    if (chid < 0) {
        printf("[ERROR] ChannelCreate failed\n");
        return -1;
    }

    /* register name so cpu can find us */
    name_attach_t *attach = name_attach(NULL, OLED_CHANNEL_NAME, 0);
    if (attach == NULL) {
        printf("[ERROR] name_attach failed\n");
        return -1;
    }

    printf("[OK] Channel ready, waiting for messages...\n");

    /* message loop */
    while (1) {
        OledMsg msg;
        OledMsg reply;
        int rcvid;

        rcvid = MsgReceive(attach->chid, &msg, sizeof(msg), NULL);
        if (rcvid < 0) continue;

        /* draw on OLED */
        update_oled_diagnostics(&msg);

        /* reply to cpu so it doesn't block */
        reply.type = 2;
        MsgReply(rcvid, 0, &reply, sizeof(reply));
    }

    name_detach(attach, 0);
    ChannelDestroy(chid);
    close(oled_fd);
    return 0;
}
