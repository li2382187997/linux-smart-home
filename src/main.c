#include "common.h"
#include <poll.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <linux/input.h>

#define DEV_PATH2    "/dev/ttySAC2"
#define REC_CMD      "arecord -q -d3 -c1 -r16000 -traw -fS16_LE ./cmd.pcm"
#define REC_WAV_CMD  "arecord -q -d5 -c1 -r16000 -twav -fS16_LE ./record.wav"
#define PLAY_WAV_CMD "aplay -q ./record.wav"
#define LCD_W 800
#define LCD_H 480

/* ================= 彩色 BMP（正确 BGR->ARGB, Alpha=0xFF） ================= */
extern unsigned char *lcd;

static void show_24bmp(const char *file, int x0, int y0) {
    int fd = open(file, O_RDONLY);
    if (fd < 0) return;
    unsigned char header[54];
    if (read(fd, header, 54) != 54) { close(fd); return; }
    int w = *(int *)&header[18];
    int h = *(int *)&header[22];
    if (h < 0) h = -h;
    int line_bytes = ((w * 3 + 3) / 4) * 4;
    unsigned char *line = malloc(line_bytes);
    if (!line) { close(fd); return; }
    for (int y = 0; y < h; y++) {
        if (read(fd, line, line_bytes) <= 0) break;
        int fb_y = y0 + (h - 1 - y);
        if (fb_y < 0 || fb_y >= LCD_H) continue;
        for (int x = 0; x < w; x++) {
            int fb_x = x0 + x;
            if (fb_x < 0 || fb_x >= LCD_W) continue;
            int idx = (fb_y * LCD_W + fb_x) * 4;
            lcd[idx + 0] = line[x * 3 + 0];
            lcd[idx + 1] = line[x * 3 + 1];
            lcd[idx + 2] = line[x * 3 + 2];
            lcd[idx + 3] = 0xFF;
        }
    }
    free(line);
    close(fd);
}

/* ================= 全局变量 ================= */
static char *pics[] = {"0720.bmp", "07.bmp", "06.bmp", "10.bmp"};
#define PIC_N (sizeof(pics) / sizeof(pics[0]))

int tty2_fd = -1;
static int led[3];
static int touch_nbio = -1, lock_x = -1, lock_y = -1;
static font *g_font;

void showbitmap(bitmap *bm, int x, int y);
void font_show(char *s, int size, int w, int h, int bg, int fx, int fy,
               int fg, int lx, int ly);
void fontUnload(font *f);
void serial_init(void);
int get_stm32_data(char *cmd);
int get_voice_info(int sockfd);
void page_main(int sockfd);
void gallery_run(void);
void recorder_run(void);
void light_run(void);
void ai_voice(int sockfd);

/* ================= 工具函数 ================= */
static int in(int x, int y, int l, int t, int r, int b) {
    return x >= l && x <= r && y >= t && y <= b;
}

static void lock_touch(int x, int y) { lock_x = x; lock_y = y; }

static void read_touch(int *x, int *y) {
    struct timeval a, b;
    int tx, ty, ms;
    for (;;) {
        gettimeofday(&a, NULL);
        get_xy(&tx, &ty);
        gettimeofday(&b, NULL);
        if (touch_nbio < 0) {
            ms = (b.tv_sec - a.tv_sec) * 1000 + (b.tv_usec - a.tv_usec) / 1000;
            touch_nbio = ms < 20;
        }
        if (tx < 0 || tx >= LCD_W || ty < 0 || ty >= LCD_H) { poll(NULL, 0, 20); continue; }
        if (touch_nbio && lock_x >= 0 &&
            tx >= lock_x - 24 && tx <= lock_x + 24 &&
            ty >= lock_y - 24 && ty <= lock_y + 24) { poll(NULL, 0, 20); continue; }
        lock_x = lock_y = -1;
        *x = tx; *y = ty;
        return;
    }
}

static int back(int x, int y) { return in(x, y, 660, 10, 760, 70); }

void draw_back(void) {
    font_show("返回", 28, 100, 60, 0xFF888888, 10, 10, 0xFFFFFFFF, 660, 10);
}

static int wait_tap(int n, const int *rects, int *tx, int *ty) {
    int x, y;
    read_touch(&x, &y);
    *tx = x; *ty = y;
    if (back(x, y)) return -1;
    for (int i = 0; i < n; ++i)
        if (in(x, y, rects[4*i], rects[4*i+1], rects[4*i+2], rects[4*i+3]))
            return i;
    return -2;
}

static unsigned char *fb_mmap(int *fd_lcd) {
    *fd_lcd = open("/dev/fb0", O_RDWR);
    if (*fd_lcd < 0) return NULL;
    unsigned char *p = mmap(NULL, LCD_W * LCD_H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, *fd_lcd, 0);
    if (p == MAP_FAILED) { close(*fd_lcd); return NULL; }
    return p;
}

static void fb_unmap(unsigned char *fb, int fd_lcd) {
    munmap(fb, LCD_W * LCD_H * 4);
    close(fd_lcd);
}

static void fill_rect(unsigned char *fb, int x, int y, int w, int h, unsigned int color) {
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i) {
            if (i < 0 || i >= LCD_W || j < 0 || j >= LCD_H) continue;
            int idx = (j * LCD_W + i) * 4;
            fb[idx + 0] = (color >>  0) & 0xFF;
            fb[idx + 1] = (color >>  8) & 0xFF;
            fb[idx + 2] = (color >> 16) & 0xFF;
            fb[idx + 3] = (color >> 24) & 0xFF;
        }
}

/* ================= 组名信息（首页左上方） ================= */
static void draw_group_info(void) {
    font_show("李雅洁", 36, 200, 50, 0x00000000, 10, 5, 0xFFFFFFFF, 20, 20);
   
}

/* ================= LED / 串口 ================= */
static int send_led(int i, int on) {
    char cmd[16];
    snprintf(cmd, sizeof(cmd), "led%d-%s\n", i + 1, on ? "on" : "off");
    if (write(tty2_fd, cmd, strlen(cmd)) < 0) return 0;
    led[i] = on;
    return 1;
}

static int stm_query(char *cmd, int min) {
    char buf[128] = {0};
    int i, n;
    if (write(tty2_fd, cmd, strlen(cmd)) < 0) return 0;
    for (i = 0; i < 30; ++i) {
        struct pollfd p = {tty2_fd, POLLIN, 0};
        if (poll(&p, 1, 200) > 0 && (n = read(tty2_fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            if (atoi(buf) > min) return atoi(buf);
        }
    }
    return 0;
}

/* ================= bitmap / font ================= */
void showbitmap(bitmap *bm, int x0, int y0) {
    int xs = x0 < 0 ? -x0 : 0, ys = y0 < 0 ? -y0 : 0;
    int xe = bm->width, ye = bm->height;
    if (x0 + xe > LCD_W) xe = LCD_W - x0;
    if (y0 + ye > LCD_H) ye = LCD_H - y0;
    for (int y = ys; y < ye; ++y)
        for (int x = xs; x < xe; ++x) {
            int s = (y * bm->width + x) * 4;
            int d = ((y + y0) * LCD_W + x + x0) * 4;
            memcpy(lcd + d, bm->map + s, 4);
        }
}

void font_show(char *s, int size, int w, int h, int bg, int fx, int fy,
               int fg, int lx, int ly) {
    bitmap *bm;
    if (!g_font) g_font = fontLoad("/simfang.ttf");
    if (!g_font) return;
    fontSetSize(g_font, size);
    bm = createBitmapWithInit(w, h, 4, bg);
    if (!bm) return;
    fontPrint(g_font, bm, fx, fy, s, fg, 0);
    showbitmap(bm, lx, ly);
    destroyBitmap(bm);
}

/* ================= 总首页 ================= */
void start(void) {
    int x, y;
    show_24bmp("bj.bmp", 0, 0);
    draw_group_info();
    usleep(4000000);
    get_xy(&x, &y);
    while (1) { get_xy(&x, &y); if (x > 20 && x < 780 && y > 20 && y < 460) break; }
}

/* ================= 启动进度条 ================= */
static void loading(void) {
    int fd_lcd;
    show_24bmp("1010.bmp", 0, 0);
    unsigned char *fb = fb_mmap(&fd_lcd);
    if (!fb) return;
    fill_rect(fb, 100, 420, 600, 24, 0xFF323232u);
    for (int i = 0; i <= 600; i += 4) {
        fill_rect(fb, 100, 420, i, 24, 0xFF00C800u);
        usleep(15000);
    }
    fb_unmap(fb, fd_lcd);
}

/* ================= 主界面（四个图标 + 文字） ================= */
void page_main(int sockfd) {
    int x, y;
    while (1) {
        show_24bmp("xbj.bmp", 0, 0);

        /* 四个模块图标 225x135，严格对齐原始坐标 */
        show_24bmp("xc.bmp",  80, 130);
        show_24bmp("yy.bmp", 420, 130);
        show_24bmp("lyb.bmp", 80, 330);
        show_24bmp("dg.bmp", 420, 330);

        /* 图标上叠加文字标签 */
        font_show("相册",     32, 120, 40, 0x00000000, 10, 5, 0xFFFFFFFF, 130, 140);
        font_show("语音控制", 32, 120, 40, 0x00000000, 10, 5, 0xFFFFFFFF, 470, 140);
        font_show("留言器",   32, 120, 40, 0x00000000, 10, 5, 0xFFFFFFFF, 130, 340);
        font_show("灯光控制", 32, 120, 40, 0x00000000, 10, 5, 0xFFFFFFFF, 470, 340);

        /* 左上角首页按钮 */
        fill_rect(lcd, 10, 10, 80, 40, 0xFF555555u);
        font_show("首页", 24, 60, 30, 0x00000000, 5, 5, 0xFFFFFFFF, 15, 15);

        read_touch(&x, &y);

        if (in(x, y, 80, 130, 305, 265)) {
            lock_touch(x, y); gallery_run();
        } else if (in(x, y, 420, 130, 645, 265)) {
            lock_touch(x, y); ai_voice(sockfd);
        } else if (in(x, y, 80, 330, 305, 465)) {
            lock_touch(x, y); recorder_run();
        } else if (in(x, y, 420, 330, 645, 465)) {
            lock_touch(x, y); light_run();
        } else if (in(x, y, 10, 10, 90, 50)) {
            return;
        }
    }
}

/* ================= 相册 ================= */
void gallery_run(void) {
    int i = 0, x, y;
    static const int next_rect[4] = {680, 410, 780, 460};
    for (;;) {
        show_24bmp("ybj.bmp", 0, 0);
        show_24bmp(pics[i], 0, 0);
        draw_back();
        font_show("下一张", 24, 100, 40, 0xFF00AA00, 10, 8, 0xFFFFFFFF, 680, 420);
        int r = wait_tap(1, next_rect, &x, &y);
        if (r == -1) return;
        if (r == 0) {
            lock_touch(x, y);
            i = (i + 1) % PIC_N;
        }
    }
}

/* ================= 留言器 ================= */
void recorder_run(void) {
    static const int mic[4] = {250, 330, 550, 460};
    int ret, x, y;
    (void)mic;
    show_24bmp("ybj.bmp", 0, 0);
    font_show("留言器", 32, 200, 50, 0xFF444444, 10, 10, 0xFFFFFFFF, 320, 60);
    draw_back();
    font_show("点击麦克风录制/播放", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 130);
    for (;;) {
        int r = wait_tap(1, mic, &x, &y);
        if (r < 0) return;
        font_show("录音中...", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 180);
        ret = system(REC_WAV_CMD);
        font_show(ret ? "录音失败" : "录音完成，点击播放", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 180);
        if (ret) continue;
        for (;;) {
            r = wait_tap(1, mic, &x, &y);
            if (r < 0) return;
            if (r == 0) break;
        }
        font_show("播放中...", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 180);
        system(PLAY_WAV_CMD);
        font_show("播放完成", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 180);
    }
}

/* ================= 灯光控制 ================= */
static void led_text(int i) {
    static const int xpos[] = {100, 340, 580};
    static const unsigned int col[] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF};
    char s[16];
    snprintf(s, sizeof(s), "%s:%s", i == 0 ? "红" : i == 1 ? "绿" : "蓝", led[i] ? "开" : "关");
    font_show(s, 24, 120, 50, led[i] ? (int)col[i] : (int)0xFF555555u, 10, 10, 0xFFFFFFFF, xpos[i], 240);
}

void light_run(void) {
    static const int led_rects[3][4] = {
        {100, 230, 240, 310},
        {340, 230, 480, 310},
        {580, 230, 720, 310}
    };
    int x, y;
    show_24bmp("ybj.bmp", 0, 0);
    font_show("灯光控制", 32, 200, 50, 0xFF444444, 10, 10, 0xFFFFFFFF, 300, 60);
    draw_back();
    for (int i = 0; i < 3; ++i) led_text(i);
    for (;;) {
        int r = wait_tap(3, (const int *)led_rects, &x, &y);
        if (r < 0) return;
        if (r < 3) { lock_touch(x, y); send_led(r, !led[r]); led_text(r); }
    }
}

/* ================= 语音控制 ================= */
static void voice_page(void) {
    show_24bmp("ybj.bmp", 0, 0);
    draw_back();
    font_show("点击麦克风开始语音控制", 28, 700, 40, 0xFFFFFFFF, 10, 8, 0xFF000000, 50, 130);
}

void ai_voice(int sockfd) {
    static const int mic[4] = {300, 340, 520, 460};
    int cmd, value, x, y;
    char s[128];
    (void)x; (void)y;
    voice_page();
    for (;;) {
        int r = wait_tap(1, mic, &x, &y);
        if (r < 0) return;
        cmd = get_voice_info(sockfd);
        if (cmd == 999) return;
        if (cmd == 2) {
            font_show("你好!", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
            system("aplay -q /ai_wav/nihao.wav &");
        } else if (cmd == 3 || cmd == 6) {
            font_show("请把手放到传感器，等待一会", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
            if (cmd == 3) {
                system("aplay -q /ai_wav/dengdai.wav");
                value = get_stm32_data("mks-sbp\n");
                snprintf(s, sizeof(s), value <= 0 ? "测量失败，请重试" : value <= 139 ? "血压正常，继续保持" : "血压偏高，请注意");
            } else {
                value = get_stm32_data("mks-hr\n");
                snprintf(s, sizeof(s), value > 0 ? "心率：%d" : "测量失败，请重试", value);
            }
            font_show(s, 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
        } else if (cmd == 100) {
            value = send_led(0, 1) && send_led(1, 1) && send_led(2, 1);
            font_show(value ? "灯光已全部打开" : "灯光控制失败", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
        }else if (cmd == 4) {   // ★ 打开相册：进入相册循环，点返回后回到语音界面
            font_show("好的，打开相册", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
            gallery_run();        // 阻塞在相册内，return 后回到本循环的下一轮等待语音
        } else if (cmd == 5) {   // ★ 打开留言板
            font_show("好的，打开留言板", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
            recorder_run();
        }
        else {
            font_show("我好像没听懂，请再说一遍", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
        }
    }
}

/* ================= 串口 / 网络 ================= */
void serial_init(void) {
    tty2_fd = open(DEV_PATH2, O_RDWR | O_NOCTTY);
    if (tty2_fd < 0) exit(1);
    init_tty(tty2_fd);
}

int get_stm32_data(char *cmd) {
    if (strstr(cmd, "mks-sbp")) return stm_query(cmd, 0);
    if (strstr(cmd, "mks-hr"))  return stm_query(cmd, 0);
    if (strstr(cmd, "asm"))     return stm_query(cmd, 32);
    return 0;
}

int get_voice_info(int sockfd) {
    xmlChar *id;
    int n;
    font_show("我在听，请说出你的需求", 32, 700, 300, 0xFFFFFF00, 10, 80, 0xFF000000, 36, 26);
    if (system(REC_CMD)) return 0;
    send_pcm(sockfd, "./cmd.pcm");
    id = wait4id(sockfd);
    if (!id) return 0;
    n = atoi((char *)id);
    xmlFree(id);
    return n;
}

/* ================= main ================= */
int main(int argc, char **argv) {
    int sockfd;
    if (argc != 2) { printf("Usage: %s <ubuntu-IP>\n", argv[0]); return 1; }
    lcd_init();
    ts_init();
    serial_init();
    sockfd = init_sock(argv[1]);
    while (1) {
        start();
        loading();
        page_main(sockfd);
    }
    close(sockfd);
    close(tty2_fd);
    if (g_font) fontUnload(g_font);
    return 0;
}
