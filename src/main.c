/*
 * 智能家居中控终端 —— 应用层主程序
 *
 * 编译依赖：实训方 BSP（common.h / common.c，提供 lcd_init、ts_init、init_tty、
 *           init_sock、get_xy、send_pcm、wait4id、fontLoad 等）与字体文件
 * 运行依赖：/dev/fb0、/dev/ttySAC2、Ubuntu 语音识别服务、alsa-utils
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "common.h"

// 外部字库句柄
extern font *f;
extern char board_info[128];

#define W 800
#define H 480

// 屏幕缓冲指针（由 lcd_init 提供）
extern unsigned char *lcd;

#define CMD  "./recordcmd.sh"   // 录音脚本
#define CMD1 "./myplay.sh"      // 播放脚本

// ================== 小工具 ==================
static void msleep(int ms)
{
    struct timespec ts = {0, (long)ms * 1000000L};
    nanosleep(&ts, NULL);
}

static unsigned int ts_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned int)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

// 纯色矩形
static void draw_solid_rect(int x, int y, int w, int h, unsigned int color)
{
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++)
        {
            int px = x + xx, py = y + yy;
            if (px < 0 || py < 0 || px >= W || py >= H)
                continue;
            unsigned char *p = lcd + (py * W + px) * 4;
            p[0] = color & 0xff;
            p[1] = (color >> 8) & 0xff;
            p[2] = (color >> 16) & 0xff;
            p[3] = (color >> 24) & 0xff;
        }
}

// 横线
static void draw_hline(int x, int y, int w, unsigned int color)
{
    draw_solid_rect(x, y, w, 1, color);
}

// 空心矩形边框
static void draw_frame(int x, int y, int w, int h, unsigned int color)
{
    draw_hline(x, y, w, color);
    draw_hline(x, y + h - 1, w, color);
    draw_solid_rect(x, y, 1, h, color);
    draw_solid_rect(x + w - 1, y, 1, h, color);
}

// 居中写字（返回实际宽度）
static int draw_text_center(const char *text, int size, int cx, int y, unsigned int color)
{
    int width = W;
    fontSetSize(f, size);
    bitmap bm;
    fontPrint(f, (unsigned char *)text, &bm, width, 0);
    int leftplace = (cx - bm.width) / 2;
    show_font(f, (unsigned char *)text, color, leftplace < 0 ? cx : leftplace, y);
    return bm.width;
}

static void draw_text_left(const char *text, int size, int x, int y, unsigned int color)
{
    fontSetSize(f, size);
    show_font(f, (unsigned char *)text, color, x, y);
}

// 进度条
static void draw_progress(int x, int y, int w, int h, int perc,
                          unsigned int bg, unsigned int fg)
{
    draw_solid_rect(x, y, w, h, bg);
    draw_frame(x, y, w, h, 0xFFFFFFFF);
    int inner = (w - 4) * perc / 100;
    draw_solid_rect(x + 2, y + 2, inner, h - 4, fg);
}

// ================== 触摸去抖 ==================
#define TOUCH_JITTER_MS 20  // 按下后 20 ms 内的采样视为抖动
#define TOUCH_LOCK_DIST 24  // 位移小于此值（像素）视为同一次触摸

static unsigned int last_down_ms = 0;
static int last_down_x = -1, last_down_y = -1;

// 读取一次稳定的触摸坐标：
//   1) 首次读到点记为按下，记录时间与位置
//   2) 之后持续采样，落在抖动窗口内且位移未超阈值的点丢弃
//   3) 位置稳定或超过抖动窗口后才上报
static void read_touch(int *x, int *y)
{
    int lx = 0, ly = 0;
    for (;;)
    {
        get_xy(&lx, &ly);
        unsigned int now = ts_ms();

        if (last_down_x < 0)
        {
            last_down_x = lx;
            last_down_y = ly;
            last_down_ms = now;
            continue;
        }

        int dx = lx - last_down_x, dy = ly - last_down_y;
        int dist2 = dx * dx + dy * dy;
        int moved = dist2 > TOUCH_LOCK_DIST * TOUCH_LOCK_DIST;

        if (moved || now - last_down_ms > TOUCH_JITTER_MS)
        {
            *x = lx, *y = ly;
            last_down_x = last_down_y = -1;
            return;
        }
    }
}

static int in_rect(int x, int y, int l, int t, int r, int b)
{
    return x >= l && x <= r && y >= t && y <= b;
}

// ================== 图片资源 ==================
typedef struct
{
    const char *name;
    const char *file;
    int x, y, w, h;
} card_t;

// 主界面四个功能入口
static card_t cards[] = {
    {"相册",     "home/xc.bmp",  80,  130, 200, 150},
    {"语音控制", "home/yy.bmp",  410, 130, 200, 150},
    {"留言器",   "home/lyb.bmp", 80,  320, 200, 150},
    {"灯光控制", "home/dsk.bmp", 410, 320, 200, 150},
};
#define NCARD (sizeof(cards) / sizeof(cards[0]))

// 相册页图片
static const char *photos[] = {"home/0720.bmp", "home/07.bmp", "home/06.bmp", "home/10.bmp"};
#define NPHOTO (sizeof(photos) / sizeof(photos[0]))

// ================== BMP 解码 ==================
/*
 * 自写 24 位 BMP 解码器
 *   厂商例程里的解码函数对 24 位图支持不全，这里自己实现：
 *   - 只处理 24 位 BMP：逐像素 BGR -> ARGB
 *   - 每行字节数按 4 字节对齐补齐
 *   - BMP 自下而上存储，写帧缓冲时垂直翻转，并处理 x0/y0 偏移
 *   - 超出屏幕的部分自动裁剪
 * 返回 0 成功，-1 失败。
 */
static int show_24bmp(const char *file, int x0, int y0)
{
    FILE *fp = fopen(file, "rb");
    if (!fp)
    {
        perror("打开 BMP 失败");
        return -1;
    }

    unsigned char hdr[54];
    if (fread(hdr, 1, sizeof(hdr), fp) != sizeof(hdr))
    {
        fclose(fp);
        return -1;
    }

    if (!(hdr[0] == 'B' && hdr[1] == 'M'))
    {
        fclose(fp);
        return -1;
    }

    int width  = hdr[18] | (hdr[19] << 8) | (hdr[20] << 16) | (hdr[21] << 24);
    int height = hdr[22] | (hdr[23] << 8) | (hdr[24] << 16) | (hdr[25] << 24);
    unsigned short depth = hdr[28] | (hdr[29] << 8);

    if (depth != 24)
    {
        printf("抱歉，该功能暂不支持。\n");
        fclose(fp);
        return -1;
    }

    int line_bytes = width * 3;
    int line_bytes_vaild = (((line_bytes * 8) + 31) / 32) * 4;
    int line_bytes_offset = line_bytes_vaild - line_bytes;
    int total_line_bytes = (line_bytes + line_bytes_offset) * height;

    unsigned char *data = (unsigned char *)malloc(total_line_bytes);
    fread(data, total_line_bytes, 1, fp);

    unsigned char r, g, b;
    unsigned char *p = data;

    for (int h = height - 1; h >= 0; h--)
    {
        for (int w = 0; w < width; w++)
        {
            b = *(p++);
            g = *(p++);
            r = *(p++);

            int x1 = x0 + w;
            int y1 = y0 + height - 1 - h;

            if (x1 < 0 || y1 < 0 || x1 >= W || y1 >= H)
                continue;

            unsigned char *q = lcd + (y1 * W + x1) * 4;
            q[0] = b;
            q[1] = g;
            q[2] = r;
            q[3] = 255;
        }
        p += line_bytes_offset;
    }

    free(data);
    fclose(fp);
    return 0;
}

/* ---- 全屏图片：文件缺失时画占位背景，便于排查 ---- */
static int show_image_full(const char *file)
{
    struct stat st;
    if (stat(file, &st) != 0)
    {
        draw_solid_rect(0, 0, W, H, 0xFF202020);
        draw_text_center(file, 20, W / 2, H / 2, 0xFFFF0000);
        return -1;
    }
    return show_24bmp(file, 0, 0);
}

/* ---- 卡片缩略图 ---- */
static int show_image_at(const char *file, int x, int y, int w, int h)
{
    struct stat st;
    if (stat(file, &st) != 0)
    {
        draw_solid_rect(x, y, w, h, 0xFF303030);
        draw_frame(x, y, w, h, 0xFFFFFFFF);
        draw_text_center(file, 16, x + w / 2, y + h / 2 - 10, 0xFFFF0000);
        return -1;
    }
    draw_solid_rect(x, y, w, h, 0xFF101010);
    return show_24bmp(file, x, y);
}

// ================== 欢迎页 ==================
static void draw_group_info(void)
{
    fontSetSize(f, 36);
    show_font(f, "小卷", 0xFFFFFFFF, 20, 20);
    draw_text_left("智能家居中控系统", 24, 20, 70, 0xFFAAAAAA);
    draw_hline(0, 110, W, 0xFF555555);
}

void start(void)
{
    draw_solid_rect(0, 0, W, H, 0xFF000000);
    show_image_full("bj.bmp");
    draw_group_info();
    draw_text_center("点击任意位置继续", 28, W / 2, H - 60, 0xFFFFFF00);
    int x, y;
    read_touch(&x, &y);
}

// ================== 开机动画 ==================
static void loading(void)
{
    // 直接 mmap /dev/fb0 绘制进度条
    int fd_lcd = open("/dev/fb0", O_RDWR);
    if (fd_lcd < 0)
    {
        perror("打开 /dev/fb0 失败");
        return;
    }
    unsigned char *fb = (unsigned char *)mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE,
                                              MAP_SHARED, fd_lcd, 0);
    if (fb == MAP_FAILED)
    {
        perror("mmap 失败");
        close(fd_lcd);
        return;
    }
    draw_solid_rect(0, 0, W, H, 0xFF000000);
    for (int i = 0; i <= 100; i += 5)
    {
        draw_progress(150, H / 2 - 10, W - 300, 20, i, 0xFF333333, 0xFF00CC66);
        msleep(30);
    }
    draw_text_center("加载完成！", 28, W / 2, H / 2 - 60, 0xFFFFFFFF);
    msleep(300);

    munmap(fb, W * H * 4);
    close(fd_lcd);
}

// ================== 页面枚举 ==================
enum
{
    PAGE_MAIN = 0,
    PAGE_GALLERY,
    PAGE_VOICE,
    PAGE_RECORDER,
    PAGE_LIGHT
};

void gallery(void)
{
    int index = 0;
    int x, y;
    while (1)
    {
        draw_solid_rect(0, 0, W, H, 0xFF000000);
        show_image_full(photos[index]);
        draw_solid_rect(0, H - 80, W, 80, 0x80000000);
        draw_text_left("下一张", 28, 20, H - 60, 0xFFFFFFFF);
        draw_text_left("返回", 28, W - 100, H - 60, 0xFFFFFFFF);

        read_touch(&x, &y);
        if (y >= H - 80)
        {
            if (x < 200)
            {
                index = (index + 1) % NPHOTO;
            }
            else if (x > W - 200)
            {
                return;
            }
        }
    }
}

void recorder(void)
{
    draw_solid_rect(0, 0, W, H, 0xFF000000);
    draw_group_info();
    draw_text_center("留言器", 36, W / 2, 40, 0xFFFFFF00);
    draw_text_center("点击任意位置开始录音（5 秒）", 24, W / 2, H / 2 - 40, 0xFFFFFFFF);
    int x, y;
    read_touch(&x, &y);

    if (system(CMD) < 0)
        perror("system failed");

    draw_solid_rect(0, 0, W, H, 0xFF000000);
    draw_text_center("留言已保存", 36, W / 2, H / 2, 0xFF00FF00);
    msleep(500);
    if (system(CMD1) < 0)
        perror("system failed");
}

void light(void)
{
    static int led_states[3] = {0, 0, 0};
    int x, y;
    while (1)
    {
        draw_solid_rect(0, 0, W, H, 0xFF000000);
        draw_group_info();
        draw_text_center("灯光控制", 36, W / 2, 40, 0xFFFFFF00);

        const char *labels[] = {"灯 1", "灯 2", "灯 3"};
        unsigned int colors[] = {0xFFFF0000, 0xFF00FF00, 0xFF0000FF};
        int w = 200, h = 150;
        int gap = 50;
        int total = 3 * w + 2 * gap;
        int start_x = (W - total) / 2;

        for (int i = 0; i < 3; i++)
        {
            int x0 = start_x + i * (w + gap);
            int y0 = 180;
            draw_solid_rect(x0, y0, w, h, led_states[i] ? colors[i] : 0xFF333333);
            draw_frame(x0, y0, w, h, 0xFFFFFFFF);
            draw_text_center(labels[i], 28, x0 + w / 2, y0 + h / 2 - 15, 0xFFFFFFFF);
        }

        draw_solid_rect(0, H - 80, W, 80, 0x80000000);
        draw_text_left("返回", 28, W - 100, H - 60, 0xFFFFFFFF);

        read_touch(&x, &y);
        if (y >= H - 80 && x > W - 200)
            return;

        for (int i = 0; i < 3; i++)
        {
            int x0 = start_x + i * (w + gap);
            if (in_rect(x, y, x0, 180, x0 + w, 180 + h))
            {
                led_states[i] = !led_states[i];
                char buf[128];
                snprintf(buf, sizeof(buf), "led%d-%s\n", i + 1,
                         led_states[i] ? "on" : "off");
                int fd = open("/dev/ttySAC2", O_RDWR);
                if (fd < 0)
                {
                    perror("打开串口失败");
                    continue;
                }
                write(fd, buf, strlen(buf));
                close(fd);
            }
        }
    }
}

void voice(int sockfd)
{
    draw_solid_rect(0, 0, W, H, 0xFF000000);
    draw_group_info();
    draw_text_center("语音控制", 36, W / 2, 40, 0xFFFFFF00);
    char hint[64];
    snprintf(hint, sizeof(hint), "socketfd = %d", sockfd);
    draw_text_left(hint, 20, 20, H - 30, 0xFF888888);

    // 画个麦克风当按钮
    int w = 200, h = 200;
    int x0 = W / 2 - w / 2, y0 = 190;
    draw_frame(x0, y0, w, h, 0xFFFFFF00);
    draw_text_center("🎤", 64, W / 2, y0 + 60, 0xFFFFFFFF);
    draw_text_center("点击开始说话", 24, W / 2, y0 + h - 40, 0xFFFFFFFF);

    int x, y;
    read_touch(&x, &y);
    if (!in_rect(x, y, x0, y0, x0 + w, y0 + h))
        return;

    if (get_voice_info(sockfd) < 0)
        return;

    draw_solid_rect(0, 160, W, H - 160, 0xFF000000);
    draw_text_center("识别中…", 32, W / 2, H / 2, 0xFFFFFF00);

    xmlChar *id = wait4id(sockfd);
    if (!id)
    {
        draw_text_center("未识别", 32, W / 2, H / 2, 0xFFFF0000);
        msleep(1000);
        return;
    }

    draw_solid_rect(0, 160, W, H - 160, 0xFF000000);
    if (strcmp((char *)id, "2") == 0)
    {
        draw_text_center("你好！", 48, W / 2, H / 2, 0xFFFFFF00);
    }
    else if (strcmp((char *)id, "3") == 0)
    { // 测量血压
        draw_text_center("正在测量血压…", 32, W / 2, H / 2, 0xFFFFFF00);
        int bp = stm_query("mks-sbp", 1);
        char buf[64];
        if (bp > 0)
            snprintf(buf, sizeof(buf), "血压：%d mmHg", bp);
        else
            snprintf(buf, sizeof(buf), "测量失败，请再试一次");
        draw_solid_rect(0, 160, W, H - 160, 0xFF000000);
        draw_text_center(buf, 36, W / 2, H / 2, 0xFFFFFFFF);
    }
    else if (strcmp((char *)id, "6") == 0)
    { // 测量心率
        draw_text_center("正在测量心率…", 32, W / 2, H / 2, 0xFFFFFF00);
        int hr = stm_query("mks-hr", 1);
        char buf[64];
        if (hr > 0)
            snprintf(buf, sizeof(buf), "心率：%d bpm", hr);
        else
            snprintf(buf, sizeof(buf), "测量失败，请再试一次");
        draw_solid_rect(0, 160, W, H - 160, 0xFF000000);
        draw_text_center(buf, 36, W / 2, H / 2, 0xFFFFFFFF);
    }
    else if (strcmp((char *)id, "4") == 0)
    { // 打开相册
        gallery();
        return;
    }
    else if (strcmp((char *)id, "5") == 0)
    { // 打开留言器
        recorder();
        return;
    }
    else if (strcmp((char *)id, "100") == 0)
    { // 打开全部灯光
        static char cmd[128];
        snprintf(cmd, sizeof(cmd), "led1-on\nled2-on\nled3-on\n");
        stm_send(cmd);
        draw_text_center("灯光已全部打开！", 36, W / 2, H / 2, 0xFFFFFF00);
    }
    else
    {
        draw_text_center("抱歉，我好像没听懂，请再说一遍", 28, W / 2, H / 2, 0xFFFF0000);
    }
    msleep(1500);
}

// ================== 主界面 ==================
// 返回值：下一个要跳转的页面
static int page_main(int sockfd)
{
    draw_solid_rect(0, 0, W, H, 0xFF000000);
    show_image_full("bj2.bmp");
    draw_group_info();

    char buf[128];
    snprintf(buf, sizeof(buf), "socketfd = %d", sockfd);
    draw_text_left(buf, 18, 20, H - 25, 0xFF666666);

    for (int i = 0; i < NCARD; i++)
    {
        int x = cards[i].x, y = cards[i].y, w = cards[i].w, h = cards[i].h;
        show_image_at(cards[i].file, x, y, w, h);
        draw_solid_rect(x, y + h - 30, w, 30, 0x80000000);
        draw_text_center(cards[i].name, 20, x + w / 2, y + h - 26, 0xFFFFFFFF);
    }

    int x, y;
    read_touch(&x, &y);
    for (int i = 0; i < NCARD; i++)
    {
        int x0 = cards[i].x, y0 = cards[i].y;
        if (in_rect(x, y, x0, y0, x0 + cards[i].w, y0 + cards[i].h))
        {
            return i + 1; // PAGE_GALLERY = 1
        }
    }
    return PAGE_MAIN;
}

// ================== 串口 ==================
#define TTY_DEV  "/dev/ttySAC2"
#define TTY_BAUD 9600
static int tty_fd = -1;

void serial_init()
{
    tty_fd = init_tty(TTY_DEV);
    if (tty_fd < 0)
        printf("open error tty_fd!\n");

    char *send_buf = "A";
    write(tty_fd, send_buf, strlen(send_buf));
    return;
}

// 发送 \n 结尾的命令
static void stm_send(const char *cmd)
{
    if (tty_fd < 0)
        return;
    write(tty_fd, cmd, strlen(cmd));
    fsync(tty_fd);
}

// 轮询等待 STM32 回一个整数，wait_secs 秒超时返回 -1
static int stm_query(const char *cmd, int wait_secs)
{
    if (tty_fd < 0)
        return -1;

    write(tty_fd, cmd, strlen(cmd));

    // 清空 pending
    char ch;
    while (read(tty_fd, &ch, 1) > 0)
    {
    };

    // 串口可能分批给出数据，用缓冲累计，遇到数字或换行即成帧
    char buf[64];
    size_t idx = 0;
    for (int waited = 0; waited < wait_secs * 1000 / 200; ++waited)
    {
        struct pollfd pfd = {.fd = tty_fd, .events = POLLIN};
        if (poll(&pfd, 1, 200) > 0 && (pfd.revents & POLLIN))
        {
            ssize_t n = read(tty_fd, &ch, 1);
            if (n <= 0)
                continue;
            if ((ch >= '0' && ch <= '9') || ch == '-')
            {
                if (idx + 1 < sizeof(buf))
                    buf[idx++] = ch;
                if (idx >= 2)
                {
                    buf[idx] = '\0';
                    return atoi(buf);
                }
            }
            else if (ch == '\n' || ch == '\r')
            {
                if (idx > 0)
                {
                    buf[idx] = '\0';
                    return atoi(buf);
                }
            }
        }
    }
    return -1;
}

/* ======================== main ======================== */
int main(int argc, char **argv)
{
    serial_init();
    if (argc < 2)
    {
        fprintf(stderr, "用法: %s <ubuntu-IP>\n", argv[0]);
        return 1;
    }
    int sockfd = init_sock(argv[1]);
    lcd_init();
    ts_init();

    // 开机：欢迎页 → 加载动画
    start();
    loading();

    // 主循环：主页 + 四个功能页
    int page = PAGE_MAIN;
    for (;;)
    {
        switch (page)
        {
        case PAGE_MAIN:
            page = page_main(sockfd);
            break;
        case PAGE_GALLERY:
            gallery();
            page = PAGE_MAIN;
            break;
        case PAGE_VOICE:
            voice(sockfd);
            page = PAGE_MAIN;
            break;
        case PAGE_RECORDER:
            recorder();
            page = PAGE_MAIN;
            break;
        case PAGE_LIGHT:
            light();
            page = PAGE_MAIN;
            break;
        default:
            page = PAGE_MAIN;
            break;
        }
    }
    return 0;
}
