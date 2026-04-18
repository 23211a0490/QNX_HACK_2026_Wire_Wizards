#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <hw/i2c.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/iofunc.h>
#include <sys/dispatch.h>

/* ── OLED ── */
#define OLED_ADDR   0x3C
#define I2C_DEV     "/dev/i2c1"
#define OLED_W      128
#define OLED_PAGES  8

/* ── THRESHOLDS ── */
#define CPU_WARN    55
#define CPU_CRIT    75
#define MEM_WARN    55
#define MEM_CRIT    75
#define TEMP_WARN   42
#define TEMP_CRIT   50

/* ── GPIO: OUTPUT ── */
#define FAN_GPIO    17
#define GREEN_GPIO  22
#define YELLOW_GPIO 23
#define RED_GPIO    24
#define BUZZ_GPIO   25

/* ── GPIO: INPUT BUTTONS ── */
#define BTN_UP      5
#define BTN_DOWN    6
#define BTN_LEFT    13
#define BTN_RIGHT   19
#define BTN_SELECT  26
#define BTN_RESET   21

/* ── HTTP ── */
#define HTTP_PORT   8080

/* ── GAME LOAD ── */
#define GAME1_EXTRA_CPU   20
#define GAME1_EXTRA_MEM   15
#define GAME2_EXTRA_CPU   40
#define GAME2_EXTRA_MEM   30

/* ════════════════════════════════
   GLOBAL STATE
   ════════════════════════════════ */
int oled_fd   = -1;
int fan_is_on =  0;
int gpio_ok   =  0;
time_t start_time;

typedef enum { ALERT_NONE, ALERT_WARN, ALERT_CRIT } AlertLevel;
volatile AlertLevel alert_level = ALERT_NONE;

/* auto kill mode */
volatile int auto_kill_enabled = 0;

/* game state */
typedef enum {
    GAME_NONE = 0,
    GAME_SNAKE,
    GAME_PONG,
    GAME_TETRIS,
    GAME_BREAKOUT
} GameID;

typedef struct {
    GameID  id;
    char    name[16];
    int     active;
    int     score;
    int     level;
    pthread_t thread;
} GameSlot;

#define MAX_GAMES 2
GameSlot game_slots[MAX_GAMES];
volatile int games_running = 0;
pthread_mutex_t game_mutex = PTHREAD_MUTEX_INITIALIZER;

/* last killed game info for dashboard */
volatile int   last_kill_cpu_drop = 0;
volatile int   last_kill_mem_drop = 0;
volatile char  last_killed_name[16] = "";

/* HTTP latest stats */
volatile int latest_cpu  = 0;
volatile int latest_mem  = 0;
volatile int latest_temp = 0;

/* sim values */
int sim_cpu  = 30;
int sim_mem  = 45;
int sim_temp = 30;
#define SHM_NAME "/qnx_temp_shm"
int *shared_temp_ptr = NULL;
#define OLED_CHANNEL_NAME "/dev/oled_channel"
int oled_coid = -1;

typedef struct {
    uint16_t type;
    int cpu, mem, temp;
    char uptime[16];
    int fan, alert, games_running;
} OledMsg;

/* menu */
typedef enum {
    SCREEN_DIAGNOSTICS,
    SCREEN_MENU,
    SCREEN_GAME,
    SCREEN_RESET_MSG,
    SCREEN_KILL_MSG
} ScreenMode;

volatile ScreenMode current_screen = SCREEN_DIAGNOSTICS;
volatile int menu_index = 0;

#define MENU_ITEMS 7
const char *menu_labels[MENU_ITEMS] = {
    "Diagnostics",
    "Snake",
    "Pong",
    "Tetris",
    "Breakout",
    "Kill Process",
    "Kill All"
};

/* threads */
pthread_t led_thread, buzz_thread, http_thread, btn_thread;

/* ════════════════════════════════
   GPIO HELPERS
   ════════════════════════════════ */
void gpio_set(int pin, int val) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "gpio-bcm2711 set %d %s", pin, val ? "dh" : "dl");
    system(cmd);
}
void gpio_setup_output(int pin) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "gpio-bcm2711 set %d op", pin);
    system(cmd);
}
void gpio_setup_input_pullup(int pin) {
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "gpio-bcm2711 set %d ip pu", pin);
    system(cmd);
}
int gpio_read(int pin) {
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
        "gpio-bcm2711 get %d | grep -o 'level=[01]' | cut -d= -f2", pin);
    FILE *f = popen(cmd, "r");
    if (!f) return 1;
    int val = 1;
    fscanf(f, "%d", &val);
    pclose(f);
    return val;
}

/* ════════════════════════════════
   GPIO INIT
   ════════════════════════════════ */
int gpio_init() {
    printf("[GPIO] Configuring outputs...\n");
    gpio_setup_output(FAN_GPIO);    gpio_set(FAN_GPIO,    0);
    gpio_setup_output(GREEN_GPIO);  gpio_set(GREEN_GPIO,  0);
    gpio_setup_output(YELLOW_GPIO); gpio_set(YELLOW_GPIO, 0);
    gpio_setup_output(RED_GPIO);    gpio_set(RED_GPIO,    0);
    gpio_setup_output(BUZZ_GPIO);   gpio_set(BUZZ_GPIO,   0);

    printf("[GPIO] Configuring button inputs...\n");
    gpio_setup_input_pullup(BTN_UP);
    gpio_setup_input_pullup(BTN_DOWN);
    gpio_setup_input_pullup(BTN_LEFT);
    gpio_setup_input_pullup(BTN_RIGHT);
    gpio_setup_input_pullup(BTN_SELECT);
    gpio_setup_input_pullup(BTN_RESET);

    printf("[GPIO] Startup test...\n");
    gpio_set(GREEN_GPIO, 1); usleep(300000);
    gpio_set(GREEN_GPIO, 0);
    gpio_set(YELLOW_GPIO,1); usleep(300000);
    gpio_set(YELLOW_GPIO,0);
    gpio_set(RED_GPIO,   1); usleep(300000);
    gpio_set(RED_GPIO,   0);
    gpio_set(BUZZ_GPIO,  1); usleep(150000);
    gpio_set(BUZZ_GPIO,  0);

    gpio_ok   = 1;
    fan_is_on = 0;
    printf("[OK] GPIO ready\n");
    return 0;
}

/* ════════════════════════════════
   FAN
   ════════════════════════════════ */
void fan_on()  { if (!gpio_ok) return; gpio_set(FAN_GPIO, 1); fan_is_on=1; printf("[FAN] ON\n");  }
void fan_off() { if (!gpio_ok) return; gpio_set(FAN_GPIO, 0); fan_is_on=0; printf("[FAN] OFF\n"); }
void handle_fan(int temp) {
    if (!gpio_ok) return;
    if (temp >= TEMP_WARN) { if (!fan_is_on) fan_on();  }
    else                   { if (fan_is_on)  fan_off(); }
}

/* ════════════════════════════════
   LOAD SIMULATION
   ════════════════════════════════ */
int smooth_value(int cur, int mn, int mx) {
    int next = cur + ((rand() % 11) - 5);
    if (next < mn) next = mn;
    if (next > mx) next = mx;
    return next;
}

int get_extra_cpu() {
    int extra = 0;
    pthread_mutex_lock(&game_mutex);
    for (int i = 0; i < MAX_GAMES; i++)
        if (game_slots[i].active)
            extra += (i == 0) ? GAME1_EXTRA_CPU : GAME2_EXTRA_CPU;
    pthread_mutex_unlock(&game_mutex);
    return extra;
}
int get_extra_mem() {
    int extra = 0;
    pthread_mutex_lock(&game_mutex);
    for (int i = 0; i < MAX_GAMES; i++)
        if (game_slots[i].active)
            extra += (i == 0) ? GAME1_EXTRA_MEM : GAME2_EXTRA_MEM;
    pthread_mutex_unlock(&game_mutex);
    return extra;
}

int get_cpu() {
    sim_cpu = smooth_value(sim_cpu, 5, 40);
    int total = sim_cpu + get_extra_cpu();
    if (total > 99) total = 99;
    return total;
}
int get_mem() {
    sim_mem = smooth_value(sim_mem, 20, 50);
    int total = sim_mem + get_extra_mem();
    if (total > 99) total = 99;
    return total;
}
int get_temp(int cpu) {
    if (shared_temp_ptr != NULL)
        return *shared_temp_ptr;
    int target = 28 + (cpu / 2);
    if (sim_temp < target) sim_temp += 2;
    else                   sim_temp -= 1;
    if (sim_temp < 28) sim_temp = 28;
    if (sim_temp > 90) sim_temp = 90;
    return sim_temp;
}

/* ════════════════════════════════
   TASK LIST
   ════════════════════════════════ */
typedef struct { char name[20]; int alive; } Task;
#define TASK_COUNT 3
Task tasks[TASK_COUNT] = {
    {"i2c-bcm2711",  1},
    {"oled_display", 1},
    {"devb-umass",   1}
};
void check_tasks() {
    for (int i = 0; i < TASK_COUNT; i++)
        tasks[i].alive = ((rand() % 20) != 0) ? 1 : 0;
}
int all_tasks_healthy() {
    for (int i = 0; i < TASK_COUNT; i++) if (!tasks[i].alive) return 0;
    return 1;
}

/* ════════════════════════════════
   ALERTS
   ════════════════════════════════ */
void handle_alerts(int cpu, int mem, int temp) {
    if (!gpio_ok) return;
    int crit = (cpu >= CPU_CRIT || mem >= MEM_CRIT || temp >= TEMP_CRIT);
    int warn = (cpu >= CPU_WARN || mem >= MEM_WARN || temp >= TEMP_WARN);
    if (crit)      alert_level = ALERT_CRIT;
    else if (warn) alert_level = ALERT_WARN;
    else           alert_level = ALERT_NONE;
}

/* ════════════════════════════════
   FONT
   ════════════════════════════════ */
const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x36,0x36,0x00,0x00,0x00},
    {0x23,0x13,0x08,0x64,0x62},
    {0x00,0x00,0x5F,0x00,0x00},
    {0x01,0x01,0x01,0x01,0x01},
    {0x06,0x09,0x09,0x06,0x00},
    {0x08,0x1C,0x3E,0x1C,0x08},
    {0x7F,0x41,0x41,0x41,0x7F},
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
   UPTIME
   ════════════════════════════════ */
void get_uptime(char *buf) {
    long s = (long)(time(NULL) - start_time);
    sprintf(buf, "%02d:%02d:%02d", (int)(s/3600), (int)((s%3600)/60), (int)(s%60));
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
   LED THREAD
   ════════════════════════════════ */
void *led_task(void *arg) {
    while (1) {
        switch (alert_level) {
            case ALERT_NONE:
                gpio_set(GREEN_GPIO,  1);
                gpio_set(YELLOW_GPIO, 0);
                gpio_set(RED_GPIO,    0);
                usleep(100000);
                break;
            case ALERT_WARN:
                gpio_set(GREEN_GPIO,  0);
                gpio_set(YELLOW_GPIO, 1);
                gpio_set(RED_GPIO,    0);
                usleep(500000);
                gpio_set(YELLOW_GPIO, 0);
                usleep(500000);
                break;
            case ALERT_CRIT:
                gpio_set(GREEN_GPIO,  0);
                gpio_set(YELLOW_GPIO, 0);
                gpio_set(RED_GPIO,    1);
                usleep(150000);
                gpio_set(RED_GPIO,    0);
                usleep(150000);
                break;
        }
    }
    return NULL;
}

/* ════════════════════════════════
   BUZZER THREAD
   ════════════════════════════════ */
void *buzz_task(void *arg) {
    while (1) {
        switch (alert_level) {
            case ALERT_NONE:
                gpio_set(BUZZ_GPIO, 0);
                usleep(500000);
                break;
            case ALERT_WARN:
                gpio_set(BUZZ_GPIO, 1); usleep(200000);
                gpio_set(BUZZ_GPIO, 0); usleep(2800000);
                break;
            case ALERT_CRIT:
                gpio_set(BUZZ_GPIO, 1); usleep(100000);
                gpio_set(BUZZ_GPIO, 0); usleep(100000);
                gpio_set(BUZZ_GPIO, 1); usleep(100000);
                gpio_set(BUZZ_GPIO, 0); usleep(500000);
                break;
        }
    }
    return NULL;
}

/* ════════════════════════════════
   HTTP SERVER — with command handling
   ════════════════════════════════ */
static const char *ok_response =
    "HTTP/1.1 200 OK\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\nOK";

void *http_server(void *arg) {
    int server_fd, client_fd;
    struct sockaddr_in addr;
    char buf[512];

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { printf("[HTTP] Socket failed\n"); return NULL; }

    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[HTTP] Bind failed\n"); return NULL;
    }
    listen(server_fd, 5);
    printf("[HTTP] Server on port %d\n", HTTP_PORT);

    while (1) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) continue;
        memset(buf, 0, sizeof(buf));
        read(client_fd, buf, sizeof(buf)-1);

        /* ── CORS preflight ── */
        if (strncmp(buf, "OPTIONS", 7) == 0) {
            const char *cors =
                "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Connection: close\r\n\r\n";
            write(client_fd, cors, strlen(cors));
            close(client_fd);
            continue;
        }

        /* ── FAN ON ── */
        if (strstr(buf, "POST /fan/on")) {
            fan_on();
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── FAN OFF ── */
        if (strstr(buf, "POST /fan/off")) {
            fan_off();
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── KILL ONE GAME ── */
        if (strstr(buf, "POST /kill/one")) {
            kill_one_game();
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── KILL ALL GAMES ── */
        if (strstr(buf, "POST /kill/all")) {
            kill_all_games();
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── AUTO KILL ON ── */
        if (strstr(buf, "POST /autokill/on")) {
            auto_kill_enabled = 1;
            printf("[AUTO] Auto-kill ENABLED\n");
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── AUTO KILL OFF ── */
        if (strstr(buf, "POST /autokill/off")) {
            auto_kill_enabled = 0;
            printf("[AUTO] Auto-kill DISABLED\n");
            write(client_fd, ok_response, strlen(ok_response));
            close(client_fd); continue;
        }

        /* ── JSON STATUS ── */
        char game_json[512] = "";
        pthread_mutex_lock(&game_mutex);
        char slot1[128]="null", slot2[128]="null";
        if (game_slots[0].active)
            snprintf(slot1,sizeof(slot1),
                "{\"name\":\"%s\",\"score\":%d,\"level\":%d}",
                game_slots[0].name, game_slots[0].score, game_slots[0].level);
        if (game_slots[1].active)
            snprintf(slot2,sizeof(slot2),
                "{\"name\":\"%s\",\"score\":%d,\"level\":%d}",
                game_slots[1].name, game_slots[1].score, game_slots[1].level);
        int gc = games_running;
        pthread_mutex_unlock(&game_mutex);

        snprintf(game_json, sizeof(game_json),
            "\"games_running\":%d,\"game_slot1\":%s,\"game_slot2\":%s,"
            "\"last_killed\":\"%s\",\"kill_cpu_drop\":%d,\"kill_mem_drop\":%d",
            gc, slot1, slot2,
            (char*)last_killed_name, last_kill_cpu_drop, last_kill_mem_drop);

        char uptime[16]; get_uptime(uptime);

        char body[1024];
        snprintf(body, sizeof(body),
            "{\n"
            "  \"cpu\": %d,\n"
            "  \"memory\": %d,\n"
            "  \"temperature\": %d,\n"
            "  \"fan\": %d,\n"
            "  \"alert\": \"%s\",\n"
            "  \"uptime\": \"%s\",\n"
            "  \"auto_kill\": %d,\n"
            "  %s\n"
            "}",
            (int)latest_cpu, (int)latest_mem, (int)latest_temp,
            fan_is_on,
            alert_level==ALERT_CRIT?"CRITICAL":alert_level==ALERT_WARN?"WARNING":"OK",
            uptime,
            auto_kill_enabled,
            game_json);

        char response[2048];
        snprintf(response, sizeof(response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Content-Length: %d\r\n"
            "Connection: close\r\n"
            "\r\n%s",
            (int)strlen(body), body);

        write(client_fd, response, strlen(response));
        close(client_fd);
    }
    close(server_fd);
    return NULL;
}

/* ════════════════════════════════
   OLED DIAGNOSTICS SCREEN
   ════════════════════════════════ */
void draw_bar_buf(int val, int page, int col, int width) {
    int filled = (val * width) / 100;
    oled_buf[page][col] = 0x7E;
    for (int i = 0; i < width; i++)
        oled_buf[page][col+1+i] = (i < filled) ? 0x3C : 0x04;
    oled_buf[page][col+width+1] = 0x7E;
}

void update_oled_diagnostics(int cpu, int mem, int temp) {
    char buf[32], uptime[16];
    get_uptime(uptime);
    Status cs = get_status(cpu,  CPU_WARN, CPU_CRIT);
    Status ms = get_status(mem,  MEM_WARN, MEM_CRIT);
    Status ts = get_status(temp, TEMP_WARN, TEMP_CRIT);

    oled_buf_clear();

    if (ts == STATUS_CRIT) {
        oled_buf_text("!! CRITICAL !!", 0, 5);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "TEMP: %d@C", temp);
        oled_buf_text(buf, 2, 20);
        draw_bar_buf(temp, 3, 0, 110);
        oled_buf_text("FAN ON DANGER", 4, 8);
        oled_buf_hline(0, 41, 128);
        sprintf(buf, "CPU:%d%% MEM:%d%%", cpu, mem);
        oled_buf_text(buf, 6, 0);
        sprintf(buf, "UP:%s", uptime);
        oled_buf_text(buf, 7, 0);
    } else if (ts == STATUS_WARN) {
        oled_buf_text("FAN ON", 0, 32);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "TEMP: %d@C", temp);
        oled_buf_text(buf, 2, 20);
        draw_bar_buf(temp, 3, 0, 110);
        oled_buf_text("ABOVE 42@C", 4, 18);
        oled_buf_hline(0, 41, 128);
        sprintf(buf, "CPU:%d%% MEM:%d%%", cpu, mem);
        oled_buf_text(buf, 6, 0);
        sprintf(buf, "UP:%s", uptime);
        oled_buf_text(buf, 7, 0);
    } else {
        oled_buf_text("QNX DIAGNOSTICS", 0, 0);
        oled_buf_hline(0, 9, 128);
        sprintf(buf, "CPU:%3d%% %s", cpu, status_str(cs));
        oled_buf_text(buf, 2, 0);
        draw_bar_buf(cpu, 3, 0, 110);
        sprintf(buf, "MEM:%3d%% %s", mem, status_str(ms));
        oled_buf_text(buf, 4, 0);
        draw_bar_buf(mem, 5, 0, 110);
        oled_buf_text(all_tasks_healthy() ? "ALL SYSTEMS OK" : "TASK DOWN", 6, 0);
        sprintf(buf, "UP:%s T:%d@", uptime, temp);
        oled_buf_text(buf, 7, 0);
    }

    oled_buf_flush();
}

/* ════════════════════════════════
   OLED MENU SCREEN
   ════════════════════════════════ */
void update_oled_menu() {
    oled_buf_clear();
    oled_buf_text("-- MAIN MENU --", 0, 5);
    oled_buf_hline(0, 9, 128);

    int start = (menu_index > 5) ? menu_index - 5 : 0;
    for (int i = 0; i < 6 && (start+i) < MENU_ITEMS; i++) {
        int idx = start + i;
        int page = i + 1;
        char label[32];

        int running = 0;
        pthread_mutex_lock(&game_mutex);
        for (int s = 0; s < MAX_GAMES; s++) {
            if (game_slots[s].active) {
                GameID gid = GAME_NONE;
                if (strcmp(menu_labels[idx],"Snake")    == 0) gid = GAME_SNAKE;
                if (strcmp(menu_labels[idx],"Pong")     == 0) gid = GAME_PONG;
                if (strcmp(menu_labels[idx],"Tetris")   == 0) gid = GAME_TETRIS;
                if (strcmp(menu_labels[idx],"Breakout") == 0) gid = GAME_BREAKOUT;
                if (gid != GAME_NONE && game_slots[s].id == gid) running = 1;
            }
        }
        pthread_mutex_unlock(&game_mutex);

        if (running)
            snprintf(label, sizeof(label), "%s [ON]", menu_labels[idx]);
        else
            snprintf(label, sizeof(label), "%s", menu_labels[idx]);

        if (idx == menu_index) {
            oled_buf_rect(0, page*8, 128, 8, 1);
            oled_buf_text("> ", page, 0);
            oled_buf_text(label, page, 12);
        } else {
            oled_buf_text(label, page, 6);
        }
    }
    oled_buf_flush();
}

/* ════════════════════════════════
   GAME: SNAKE
   ════════════════════════════════ */
#define SNAKE_W  21
#define SNAKE_H  14
#define SNAKE_MAX 60

typedef struct { int x, y; } Pt;

typedef struct {
    Pt body[SNAKE_MAX];
    int len;
    int dx, dy;
    Pt food;
    int score;
    int alive;
} SnakeGame;

void snake_place_food(SnakeGame *g) {
    g->food.x = rand() % SNAKE_W;
    g->food.y = rand() % SNAKE_H;
}

void snake_draw(SnakeGame *g, int slot) {
    oled_buf_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "SNAKE S%d SC:%d", slot+1, g->score);
    oled_buf_text(hdr, 0, 0);
    oled_buf_hline(0, 9, 128);
    oled_buf_hline(0, 10, 128);
    oled_buf_hline(0, 63, 128);
    for (int y = 10; y < 64; y++) {
        oled_buf_pixel(0, y, 1);
        oled_buf_pixel(127, y, 1);
    }
    int fx = 2 + g->food.x * 6, fy = 12 + g->food.y * 3;
    oled_buf_rect(fx, fy, 3, 3, 1);
    for (int i = 0; i < g->len; i++) {
        int px = 2 + g->body[i].x * 6;
        int py = 12 + g->body[i].y * 3;
        oled_buf_rect(px, py, (i==0)?5:4, (i==0)?4:3, 1);
    }
    oled_buf_flush();
}

void *snake_thread(void *arg) {
    int slot = *(int*)arg;
    free(arg);
    SnakeGame g;
    memset(&g, 0, sizeof(g));
    g.body[0] = (Pt){SNAKE_W/2, SNAKE_H/2};
    g.len = 3; g.dx = 1; g.dy = 0; g.alive = 1;
    snake_place_food(&g);

    pthread_mutex_lock(&game_mutex);
    game_slots[slot].score = 0;
    game_slots[slot].level = 1;
    pthread_mutex_unlock(&game_mutex);

    while (1) {
        pthread_mutex_lock(&game_mutex);
        int still_active = game_slots[slot].active;
        pthread_mutex_unlock(&game_mutex);
        if (!still_active) break;

        if (!gpio_read(BTN_UP))    { if (g.dy != 1)  { g.dx= 0; g.dy=-1; } }
        if (!gpio_read(BTN_DOWN))  { if (g.dy != -1) { g.dx= 0; g.dy= 1; } }
        if (!gpio_read(BTN_LEFT))  { if (g.dx != 1)  { g.dx=-1; g.dy= 0; } }
        if (!gpio_read(BTN_RIGHT)) { if (g.dx != -1) { g.dx= 1; g.dy= 0; } }

        for (int i = g.len-1; i > 0; i--) g.body[i] = g.body[i-1];
        g.body[0].x += g.dx;
        g.body[0].y += g.dy;

        if (g.body[0].x < 0)        g.body[0].x = SNAKE_W-1;
        if (g.body[0].x >= SNAKE_W) g.body[0].x = 0;
        if (g.body[0].y < 0)        g.body[0].y = SNAKE_H-1;
        if (g.body[0].y >= SNAKE_H) g.body[0].y = 0;

        if (g.body[0].x == g.food.x && g.body[0].y == g.food.y) {
            if (g.len < SNAKE_MAX) g.len++;
            g.score++;
            snake_place_food(&g);
        }

        for (int i = 1; i < g.len; i++) {
            if (g.body[0].x == g.body[i].x && g.body[0].y == g.body[i].y) {
                g.alive = 0; break;
            }
        }

        pthread_mutex_lock(&game_mutex);
        game_slots[slot].score = g.score;
        game_slots[slot].level = 1 + g.score / 5;
        pthread_mutex_unlock(&game_mutex);

        if (current_screen == SCREEN_GAME) snake_draw(&g, slot);

        if (!g.alive) {
            oled_buf_clear();
            oled_buf_text("GAME OVER", 2, 28);
            char sc[24]; snprintf(sc, sizeof(sc), "SCORE: %d", g.score);
            oled_buf_text(sc, 4, 30);
            oled_buf_flush();
            sleep(2);
            memset(&g, 0, sizeof(g));
            g.body[0] = (Pt){SNAKE_W/2, SNAKE_H/2};
            g.len = 3; g.dx = 1; g.dy = 0; g.alive = 1;
            snake_place_food(&g);
        }
        usleep(200000);
    }
    return NULL;
}

/* ════════════════════════════════
   GAME: PONG
   ════════════════════════════════ */
typedef struct {
    int bx, by, bdx, bdy;
    int py, ey;
    int pscore, escore;
} PongGame;

void pong_draw(PongGame *g, int slot) {
    oled_buf_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "PONG S%d %d-%d", slot+1, g->pscore, g->escore);
    oled_buf_text(hdr, 0, 0);
    oled_buf_hline(0, 9, 128);
    oled_buf_rect(2,  g->py, 3, 12, 1);
    oled_buf_rect(123,g->ey, 3, 12, 1);
    oled_buf_rect(g->bx, g->by, 3, 3, 1);
    for (int y = 10; y < 64; y += 6) oled_buf_pixel(64, y, 1);
    oled_buf_flush();
}

void *pong_thread(void *arg) {
    int slot = *(int*)arg; free(arg);
    PongGame g;
    g.bx=64; g.by=36; g.bdx=2; g.bdy=1;
    g.py=30; g.ey=30; g.pscore=0; g.escore=0;

    pthread_mutex_lock(&game_mutex);
    game_slots[slot].score = 0;
    game_slots[slot].level = 1;
    pthread_mutex_unlock(&game_mutex);

    while (1) {
        pthread_mutex_lock(&game_mutex);
        int still_active = game_slots[slot].active;
        pthread_mutex_unlock(&game_mutex);
        if (!still_active) break;

        if (!gpio_read(BTN_UP)   && g.py > 10) g.py -= 3;
        if (!gpio_read(BTN_DOWN) && g.py < 52) g.py += 3;

        if (g.ey + 6 < g.by) g.ey += 2;
        else if (g.ey + 6 > g.by) g.ey -= 2;
        if (g.ey < 10) g.ey = 10;
        if (g.ey > 52) g.ey = 52;

        g.bx += g.bdx; g.by += g.bdy;
        if (g.by <= 10 || g.by >= 61) g.bdy = -g.bdy;

        if (g.bx <= 5 && g.by >= g.py && g.by <= g.py+12) {
            g.bdx = abs(g.bdx); g.pscore++;
        }
        if (g.bx >= 120 && g.by >= g.ey && g.by <= g.ey+12) {
            g.bdx = -abs(g.bdx); g.escore++;
        }
        if (g.bx < 0 || g.bx > 128) {
            g.bx=64; g.by=36; g.bdx=(rand()%2)?2:-2; g.bdy=(rand()%2)?1:-1;
        }

        pthread_mutex_lock(&game_mutex);
        game_slots[slot].score = g.pscore;
        game_slots[slot].level = 1 + (g.pscore+g.escore)/10;
        pthread_mutex_unlock(&game_mutex);

        if (current_screen == SCREEN_GAME) pong_draw(&g, slot);
        usleep(50000);
    }
    return NULL;
}

/* ════════════════════════════════
   GAME: TETRIS
   ════════════════════════════════ */
#define TET_W  10
#define TET_H  14

typedef struct {
    int board[TET_H][TET_W];
    int px, py;
    int piece[4][2];
    int score, lines, alive;
} TetGame;

int tet_pieces[7][4][2] = {
    {{0,0},{1,0},{2,0},{3,0}},
    {{0,0},{1,0},{0,1},{1,1}},
    {{1,0},{0,1},{1,1},{2,1}},
    {{0,0},{0,1},{1,1},{2,1}},
    {{2,0},{0,1},{1,1},{2,1}},
    {{1,0},{2,0},{0,1},{1,1}},
    {{0,0},{1,0},{1,1},{2,1}},
};

void tet_new_piece(TetGame *g) {
    int t = rand() % 7;
    for (int i = 0; i < 4; i++) {
        g->piece[i][0] = tet_pieces[t][i][0];
        g->piece[i][1] = tet_pieces[t][i][1];
    }
    g->px = TET_W/2 - 1; g->py = 0;
}

int tet_valid(TetGame *g, int dx, int dy) {
    for (int i = 0; i < 4; i++) {
        int nx = g->px + g->piece[i][0] + dx;
        int ny = g->py + g->piece[i][1] + dy;
        if (nx<0||nx>=TET_W||ny>=TET_H) return 0;
        if (ny>=0 && g->board[ny][nx]) return 0;
    }
    return 1;
}

void tet_lock(TetGame *g) {
    for (int i = 0; i < 4; i++) {
        int x = g->px + g->piece[i][0];
        int y = g->py + g->piece[i][1];
        if (y>=0) g->board[y][x] = 1;
    }
    for (int r = TET_H-1; r >= 0; r--) {
        int full = 1;
        for (int c = 0; c < TET_W; c++) if (!g->board[r][c]) { full=0; break; }
        if (full) {
            for (int rr = r; rr > 0; rr--)
                memcpy(g->board[rr], g->board[rr-1], TET_W*sizeof(int));
            memset(g->board[0], 0, TET_W*sizeof(int));
            g->lines++; g->score += 10; r++;
        }
    }
    tet_new_piece(g);
    if (!tet_valid(g, 0, 0)) g->alive = 0;
}

void tet_draw(TetGame *g, int slot) {
    oled_buf_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr), "TET S%d SC:%d", slot+1, g->score);
    oled_buf_text(hdr, 0, 0);
    int ox=24, oy=10, cw=5, ch=5;
    for (int r=0;r<TET_H;r++)
        for (int c=0;c<TET_W;c++)
            if (g->board[r][c])
                oled_buf_rect(ox+c*cw, oy+r*ch, cw-1, ch-1, 1);
    for (int i=0;i<4;i++) {
        int px=ox+(g->px+g->piece[i][0])*cw;
        int py=oy+(g->py+g->piece[i][1])*ch;
        oled_buf_rect(px, py, cw-1, ch-1, 1);
    }
    oled_buf_flush();
}

void *tetris_thread(void *arg) {
    int slot = *(int*)arg; free(arg);
    TetGame g;
    memset(&g, 0, sizeof(g));
    g.alive = 1;
    tet_new_piece(&g);
    int drop_timer = 0;

    while (1) {
        pthread_mutex_lock(&game_mutex);
        int still_active = game_slots[slot].active;
        pthread_mutex_unlock(&game_mutex);
        if (!still_active) break;

        if (!gpio_read(BTN_LEFT)  && tet_valid(&g,-1,0)) g.px--;
        if (!gpio_read(BTN_RIGHT) && tet_valid(&g, 1,0)) g.px++;
        if (!gpio_read(BTN_DOWN)  && tet_valid(&g, 0,1)) g.py++;

        drop_timer++;
        if (drop_timer >= 10) {
            drop_timer = 0;
            if (tet_valid(&g, 0, 1)) g.py++;
            else tet_lock(&g);
        }

        pthread_mutex_lock(&game_mutex);
        game_slots[slot].score = g.score;
        game_slots[slot].level = 1 + g.lines/5;
        pthread_mutex_unlock(&game_mutex);

        if (current_screen == SCREEN_GAME) tet_draw(&g, slot);

        if (!g.alive) {
            oled_buf_clear();
            oled_buf_text("GAME OVER", 2, 28);
            char sc[24]; snprintf(sc, sizeof(sc), "SCORE: %d", g.score);
            oled_buf_text(sc, 4, 28);
            oled_buf_flush(); sleep(2);
            memset(&g, 0, sizeof(g)); g.alive=1; tet_new_piece(&g);
        }
        usleep(100000);
    }
    return NULL;
}

/* ════════════════════════════════
   GAME: BREAKOUT
   ════════════════════════════════ */
#define BRK_COLS  8
#define BRK_ROWS  3

typedef struct {
    int bx, by, bdx, bdy, px;
    int bricks[BRK_ROWS][BRK_COLS];
    int score, alive, remaining;
} BrkGame;

void brk_draw(BrkGame *g, int slot) {
    oled_buf_clear();
    char hdr[24];
    snprintf(hdr, sizeof(hdr),"BRK S%d SC:%d", slot+1, g->score);
    oled_buf_text(hdr, 0, 0);
    oled_buf_hline(0, 9, 128);
    for (int r=0;r<BRK_ROWS;r++)
        for (int c=0;c<BRK_COLS;c++)
            if (g->bricks[r][c])
                oled_buf_rect(c*16, 10+r*6, 14, 4, 1);
    oled_buf_rect(g->px-10, 60, 20, 3, 1);
    oled_buf_rect(g->bx, g->by, 3, 3, 1);
    oled_buf_flush();
}

void *breakout_thread(void *arg) {
    int slot = *(int*)arg; free(arg);
    BrkGame g;
    memset(&g, 0, sizeof(g));
    g.bx=64; g.by=50; g.bdx=2; g.bdy=-2;
    g.px=64; g.alive=1; g.score=0;
    for (int r=0;r<BRK_ROWS;r++)
        for (int c=0;c<BRK_COLS;c++)
            g.bricks[r][c]=1;
    g.remaining = BRK_ROWS*BRK_COLS;

    while (1) {
        pthread_mutex_lock(&game_mutex);
        int still_active = game_slots[slot].active;
        pthread_mutex_unlock(&game_mutex);
        if (!still_active) break;

        if (!gpio_read(BTN_LEFT)  && g.px > 14)  g.px -= 4;
        if (!gpio_read(BTN_RIGHT) && g.px < 114) g.px += 4;

        g.bx += g.bdx; g.by += g.bdy;
        if (g.bx<=0||g.bx>=125) g.bdx=-g.bdx;
        if (g.by<=10) g.bdy=abs(g.bdy);
        if (g.by>=57 && g.bx>=g.px-10 && g.bx<=g.px+10) g.bdy=-abs(g.bdy);

        if (g.by>64) {
            oled_buf_clear();
            oled_buf_text("GAME OVER", 2, 28);
            char sc[24]; snprintf(sc,sizeof(sc),"SCORE: %d",g.score);
            oled_buf_text(sc,4,28);
            oled_buf_flush(); sleep(2);
            g.bx=64;g.by=50;g.bdx=2;g.bdy=-2;g.px=64;g.score=0;
            for(int r=0;r<BRK_ROWS;r++) for(int c=0;c<BRK_COLS;c++) g.bricks[r][c]=1;
            g.remaining=BRK_ROWS*BRK_COLS;
        }

        for (int r=0;r<BRK_ROWS;r++) {
            for (int c=0;c<BRK_COLS;c++) {
                if (!g.bricks[r][c]) continue;
                int bx0=c*16, by0=10+r*6;
                if (g.bx+3>bx0 && g.bx<bx0+14 && g.by+3>by0 && g.by<by0+4) {
                    g.bricks[r][c]=0; g.bdy=-g.bdy;
                    g.score++; g.remaining--;
                }
            }
        }

        if (g.remaining <= 0) {
            for(int r=0;r<BRK_ROWS;r++) for(int c=0;c<BRK_COLS;c++) g.bricks[r][c]=1;
            g.remaining=BRK_ROWS*BRK_COLS;
        }

        pthread_mutex_lock(&game_mutex);
        game_slots[slot].score = g.score;
        game_slots[slot].level = 1 + g.score/10;
        pthread_mutex_unlock(&game_mutex);

        if (current_screen == SCREEN_GAME) brk_draw(&g, slot);
        usleep(50000);
    }
    return NULL;
}

/* ════════════════════════════════
   LAUNCH GAME
   ════════════════════════════════ */
int launch_game(GameID gid, const char *name) {
    pthread_mutex_lock(&game_mutex);
    int slot = -1;
    for (int i = 0; i < MAX_GAMES; i++)
        if (!game_slots[i].active) { slot = i; break; }

    if (slot < 0) {
        pthread_mutex_unlock(&game_mutex);
        printf("[GAME] No free slot\n");
        return -1;
    }

    game_slots[slot].id     = gid;
    game_slots[slot].active = 1;
    game_slots[slot].score  = 0;
    game_slots[slot].level  = 1;
    strncpy(game_slots[slot].name, name, 15);
    games_running++;

    int *s = malloc(sizeof(int)); *s = slot;
    switch (gid) {
        case GAME_SNAKE:    pthread_create(&game_slots[slot].thread, NULL, snake_thread,    s); break;
        case GAME_PONG:     pthread_create(&game_slots[slot].thread, NULL, pong_thread,     s); break;
        case GAME_TETRIS:   pthread_create(&game_slots[slot].thread, NULL, tetris_thread,   s); break;
        case GAME_BREAKOUT: pthread_create(&game_slots[slot].thread, NULL, breakout_thread, s); break;
        default: break;
    }
    pthread_mutex_unlock(&game_mutex);
    current_screen = SCREEN_GAME;
    printf("[GAME] Launched %s in slot %d\n", name, slot);
    return slot;
}

/* ════════════════════════════════
   KILL ONE GAME
   ════════════════════════════════ */
void kill_one_game() {
    pthread_mutex_lock(&game_mutex);
    for (int i = 0; i < MAX_GAMES; i++) {
        if (game_slots[i].active) {
            printf("[KILL] Stopping %s\n", game_slots[i].name);
            strncpy((char*)last_killed_name, game_slots[i].name, 15);
            last_kill_cpu_drop = (i == 0) ? GAME1_EXTRA_CPU : GAME2_EXTRA_CPU;
            last_kill_mem_drop = (i == 0) ? GAME1_EXTRA_MEM : GAME2_EXTRA_MEM;
            game_slots[i].active = 0;
            games_running--;
            pthread_join(game_slots[i].thread, NULL);
            memset(&game_slots[i], 0, sizeof(GameSlot));
            break;
        }
    }
    pthread_mutex_unlock(&game_mutex);
    current_screen = SCREEN_KILL_MSG;
}

/* ════════════════════════════════
   KILL ALL
   ════════════════════════════════ */
void kill_all_games() {
    pthread_mutex_lock(&game_mutex);
    for (int i = 0; i < MAX_GAMES; i++) {
        if (game_slots[i].active) {
            game_slots[i].active = 0;
            games_running--;
            pthread_join(game_slots[i].thread, NULL);
            memset(&game_slots[i], 0, sizeof(GameSlot));
        }
    }
    strcpy((char*)last_killed_name, "ALL");
    last_kill_cpu_drop = GAME1_EXTRA_CPU + GAME2_EXTRA_CPU;
    last_kill_mem_drop = GAME1_EXTRA_MEM + GAME2_EXTRA_MEM;
    pthread_mutex_unlock(&game_mutex);
    current_screen = SCREEN_RESET_MSG;
}

/* ════════════════════════════════
   RESET MESSAGE SCREEN
   ════════════════════════════════ */
void show_reset_screen(const char *line1, const char *line2) {
    oled_buf_clear();
    oled_buf_text("!! RESET !!", 1, 20);
    oled_buf_hline(0, 17, 128);
    oled_buf_text(line1, 3, 5);
    oled_buf_text(line2, 5, 5);
    oled_buf_text("RETURNING...", 7, 10);
    oled_buf_flush();
    sleep(2);
    current_screen = SCREEN_MENU;
}

/* ════════════════════════════════
   BUTTON THREAD
   ════════════════════════════════ */
void *button_task(void *arg) {
    int prev_up=1,prev_dn=1,prev_sel=1,prev_rst=1;
    while (1) {
        int up  = gpio_read(BTN_UP);
        int dn  = gpio_read(BTN_DOWN);
        int sel = gpio_read(BTN_SELECT);
        int rst = gpio_read(BTN_RESET);

        if (!rst && prev_rst) {
            printf("[BTN] RESET pressed\n");
            kill_all_games();
            show_reset_screen("All processes", "terminated");
        }

        if (current_screen == SCREEN_MENU || current_screen == SCREEN_DIAGNOSTICS) {
            if (!up && prev_up) {
                current_screen = SCREEN_MENU;
                menu_index = (menu_index - 1 + MENU_ITEMS) % MENU_ITEMS;
            }
            if (!dn && prev_dn) {
                current_screen = SCREEN_MENU;
                menu_index = (menu_index + 1) % MENU_ITEMS;
            }
            if (!sel && prev_sel) {
                const char *label = menu_labels[menu_index];
                if (strcmp(label, "Diagnostics") == 0)      current_screen = SCREEN_DIAGNOSTICS;
                else if (strcmp(label, "Snake")    == 0)    launch_game(GAME_SNAKE,    "SNAKE");
                else if (strcmp(label, "Pong")     == 0)    launch_game(GAME_PONG,     "PONG");
                else if (strcmp(label, "Tetris")   == 0)    launch_game(GAME_TETRIS,   "TETRIS");
                else if (strcmp(label, "Breakout") == 0)    launch_game(GAME_BREAKOUT, "BREAKOUT");
                else if (strcmp(label, "Kill Process") == 0) {
                    kill_one_game();
                    show_reset_screen("Process killed", "System calming");
                } else if (strcmp(label, "Kill All") == 0) {
                    kill_all_games();
                    show_reset_screen("All killed", "System normal");
                }
            }
        } else if (current_screen == SCREEN_GAME) {
            if (!sel && prev_sel) current_screen = SCREEN_MENU;
        }

        prev_up=up; prev_dn=dn; prev_sel=sel; prev_rst=rst;
        usleep(50000);
    }
    return NULL;
}

/* ════════════════════════════════
   TERMINAL REPORT
   ════════════════════════════════ */
void print_report(int cpu, int mem, int temp) {
    char uptime[16]; get_uptime(uptime);
    Status cs = get_status(cpu,  CPU_WARN, CPU_CRIT);
    Status ms = get_status(mem,  MEM_WARN, MEM_CRIT);
    Status ts = get_status(temp, TEMP_WARN, TEMP_CRIT);
    const char *alert_str =
        alert_level==ALERT_CRIT ? "RED FAST BLINK + RAPID BEEP" :
        alert_level==ALERT_WARN ? "YELLOW SLOW BLINK + BEEP"    :
                                  "GREEN ON  all OK";

    printf("\033[2J\033[H");
    printf("╔══════════════════════════════════════════╗\n");
    printf("║       QNX DIAGNOSTICS SYSTEM v2.0        ║\n");
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  CPU    : %3d%%   [%s]                ║\n", cpu,  status_str(cs));
    printf("║  MEMORY : %3d%%   [%s]                ║\n", mem,  status_str(ms));
    printf("║  TEMP   : %3d C   [%s]                ║\n", temp, status_str(ts));
    printf("╠══════════════════════════════════════════╣\n");
    pthread_mutex_lock(&game_mutex);
    printf("║  GAMES RUNNING: %d/2                      ║\n", games_running);
    for (int i = 0; i < MAX_GAMES; i++) {
        if (game_slots[i].active)
            printf("║  [SLOT%d] %-10s SC:%-4d LV:%-2d        ║\n",
                i+1, game_slots[i].name,
                game_slots[i].score, game_slots[i].level);
    }
    pthread_mutex_unlock(&game_mutex);
    printf("╠══════════════════════════════════════════╣\n");
    printf("║  UPTIME    : %-12s                 ║\n", uptime);
    printf("║  FAN       : %-30s ║\n", fan_is_on?"ON  (above 42C)":"OFF (normal)");
    printf("║  AUTO KILL : %-30s ║\n", auto_kill_enabled?"ENABLED":"DISABLED");
    printf("║  ALERT     : %-30s ║\n", alert_str);
    printf("║  HTTP      : port 8080 serving JSON       ║\n");
    printf("╚══════════════════════════════════════════╝\n");
    fflush(stdout);
}

/* ════════════════════════════════
   SEND OLED DATA
   ════════════════════════════════ */
void send_oled_data(int cpu, int mem, int temp) {
    if (oled_coid < 0) {
        update_oled_diagnostics(cpu, mem, temp);
        return;
    }
    OledMsg msg;
    msg.type          = 1;
    msg.cpu           = cpu;
    msg.mem           = mem;
    msg.temp          = temp;
    msg.fan           = fan_is_on;
    msg.alert         = (int)alert_level;
    msg.games_running = games_running;
    get_uptime(msg.uptime);
    OledMsg reply;
    MsgSend(oled_coid, &msg, sizeof(msg), &reply, sizeof(reply));
}

/* ════════════════════════════════
   MAIN
   ════════════════════════════════ */
int main() {
    start_time = time(NULL);
    srand((unsigned int)time(NULL));

    oled_coid = name_open(OLED_CHANNEL_NAME, 0);
    if (oled_coid >= 0)
        printf("[OK] Connected to oled_display process\n");
    else
        printf("[WARN] oled_display not running, drawing locally\n");

    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (shm_fd >= 0) {
        shared_temp_ptr = mmap(NULL, sizeof(int), PROT_READ, MAP_SHARED, shm_fd, 0);
        if (shared_temp_ptr == MAP_FAILED) shared_temp_ptr = NULL;
        else printf("[OK] Connected to temp_sim\n");
    } else {
        printf("[WARN] temp_sim not running, using local sim\n");
    }

    printf("=== QNX DIAGNOSTICS + GAME SYSTEM v2.0 ===\n");
    memset(game_slots, 0, sizeof(game_slots));
    games_running = 0;

    printf("Initializing GPIO...\n");
    gpio_init();

    pthread_create(&led_thread,  NULL, led_task,    NULL);
    pthread_create(&buzz_thread, NULL, buzz_task,   NULL);
    pthread_create(&http_thread, NULL, http_server, NULL);
    pthread_create(&btn_thread,  NULL, button_task, NULL);
    printf("[OK] All threads started\n");

    printf("Opening I2C...\n");
    oled_fd = open(I2C_DEV, O_RDWR);
    if (oled_fd < 0) { printf("[ERROR] Cannot open %s\n", I2C_DEV); return -1; }
    printf("[OK] I2C opened\n");

    oled_init();
    oled_buf_clear();
    oled_buf_flush();

    oled_buf_text("QNX SYSTEM v2.0", 2, 5);
    oled_buf_text("GAME CONSOLE", 3, 20);
    oled_buf_text("INITIALIZING...", 5, 5);
    oled_buf_flush();
    sleep(2);

    current_screen = SCREEN_MENU;
    printf("[OK] System ready\n");

    while (1) {
        int cpu  = get_cpu();
        int mem  = get_mem();
        int temp = get_temp(cpu);
        check_tasks();

        latest_cpu  = cpu;
        latest_mem  = mem;
        latest_temp = temp;

        print_report(cpu, mem, temp);

        if (current_screen == SCREEN_DIAGNOSTICS)
            send_oled_data(cpu, mem, temp);
        else if (current_screen == SCREEN_MENU)
            update_oled_menu();

        handle_fan(temp);
        handle_alerts(cpu, mem, temp);

        /* ── AUTO KILL ── if enabled and CPU or temp is critical */
        if (auto_kill_enabled) {
            if ((cpu >= CPU_CRIT || temp >= TEMP_CRIT) && games_running > 0) {
                printf("[AUTO] Critical threshold! Killing a process...\n");
                kill_one_game();
            }
        }

        sleep(2);
    }

    kill_all_games();
    fan_off();
    alert_level = ALERT_NONE;
    gpio_set(GREEN_GPIO,  0);
    gpio_set(YELLOW_GPIO, 0);
    gpio_set(RED_GPIO,    0);
    gpio_set(BUZZ_GPIO,   0);
    close(oled_fd);
    return 0;
}
