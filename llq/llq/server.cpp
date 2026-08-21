/*
 * Pure C Mongoose 8-bit BMP Server - 流畅度优化版
 * 编译:  gcc -o server server.cpp mongoose.c -lm
 */
#include "mongoose.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

 // ---------- 默认参数 ----------
static int g_width = 320;
static int g_height = 240;
static int g_interval = 0.1;          // 图像生成间隔(秒)

// ---------- BMP 缓冲区 ----------
static uint8_t* g_bmp = NULL;
static size_t g_bmp_size = 0;
static int g_running = 1;

// ---------- 简易登录 ----------
static char g_session_token[64] = { 0 };
static const char* USERNAME = "admin";
static const char* PASSWORD = "123456";

// ---------- 多流推送 (multipart/x-mixed-replace) ----------
#define MAX_STREAMS 16
static struct mg_connection *g_streams[MAX_STREAMS];
static int g_num_streams;

// ---------- 帧生成定时 ----------
static time_t g_last_gen;           // 上一次生成帧的时间

// 前向声明
void alloc_bmp(int w, int h);
void free_bmp(void);
void generate_frame(int w, int h, uint8_t* pixels, int frame);
static void push_frame_to_streams(void);

// ===== BMP 8位灰度构造 =====
static int row_size(int w) {
    return ((w + 3) / 4) * 4;
}

void alloc_bmp(int w, int h) {
    int rsize = row_size(w);
    size_t headers = 14 + 40 + 256 * 4;
    size_t total = headers + (size_t)rsize * h;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return;
    memset(buf, 0, total);

    buf[0] = 'B'; buf[1] = 'M';
    uint32_t fsize = (uint32_t)total;      memcpy(buf + 2, &fsize, 4);
    uint32_t offset = (uint32_t)headers;   memcpy(buf + 10, &offset, 4);
    uint32_t biSize = 40;                  memcpy(buf + 14, &biSize, 4);
    int32_t biWidth = w;                   memcpy(buf + 18, &biWidth, 4);
    int32_t biHeight = h;                  memcpy(buf + 22, &biHeight, 4);
    uint16_t biPlanes = 1;                 memcpy(buf + 26, &biPlanes, 2);
    uint16_t biBitCount = 8;               memcpy(buf + 28, &biBitCount, 2);

    uint8_t* palette = buf + 54;
    for (int i = 0; i < 256; i++) {
        palette[i * 4 + 0] = palette[i * 4 + 1] = palette[i * 4 + 2] = (uint8_t)i;
        palette[i * 4 + 3] = 0;
    }
    free(g_bmp);
    g_bmp = buf;
    g_bmp_size = total;
}

void free_bmp(void) {
    free(g_bmp);
    g_bmp = NULL;
    g_bmp_size = 0;
}

/* 生成动态条纹 + 渐变 (帧号自增) */
void generate_frame(int w, int h, uint8_t* pixels, int frame) {
    int rsize = row_size(w);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int base = x * 255 / w;
            int wave = (int)(64 * sin((y + frame * 10) * 0.05));
            int val = base + wave;
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            int row = h - 1 - y;
            pixels[row * rsize + x] = (uint8_t)val;
        }
    }
}

// ---------- helper: serve HTML file ----------
static void serve_file(struct mg_connection *c, const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        mg_http_reply(c, 500, NULL, "Internal Server Error\n");
        return;
    }
    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *data = (char*)malloc((size_t)len + 1);
    if (!data) {
        fclose(fp);
        mg_http_reply(c, 500, NULL, "Internal Server Error\n");
        return;
    }
    size_t n = fread(data, 1, (size_t)len, fp);
    fclose(fp);
    data[n] = '\0';
    mg_http_reply(c, 200,
        "Content-Type: text/html; charset=gbk\r\n"
        "Cache-Control: no-cache, no-store, must-revalidate\r\n", "%s", data);
    free(data);
}

// ---------- 向所有流连接推送当前帧 ----------
static void push_frame_to_streams(void) {
    if (g_num_streams == 0 || !g_bmp) return;

    // 计算一次边界字符串长度
    char boundary[64];
    int blen = snprintf(boundary, sizeof(boundary),
        "\r\n--frame\r\n"
        "Content-Type: image/bmp\r\n"
        "Content-Length: %zu\r\n\r\n", g_bmp_size);

    for (int i = 0; i < g_num_streams; ) {
        struct mg_connection *sc = g_streams[i];
        if (!sc) { g_streams[i] = g_streams[--g_num_streams]; continue; }

        // 防止发送缓冲区无限增长: 待发数据超过 1MB 则断开
        if (sc->send.len > 1024 * 1024) {
            sc->is_closing = 1;
            g_streams[i] = g_streams[--g_num_streams];
            continue;
        }

        mg_send(sc, boundary, (size_t)blen);
        mg_send(sc, g_bmp, g_bmp_size);
        i++;
    }
}

// ===== Mongoose 请求处理 =====
static void ev_handler(struct mg_connection* c, int ev, void* ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        struct mg_http_message* hm = (struct mg_http_message*)ev_data;

        // --- 首页 ---
        if (mg_match(hm->uri, mg_str("/"), NULL)) {
            char session[64] = { 0 };
            struct mg_str* cookie = mg_http_get_header(hm, "cookie");
            if (cookie) mg_http_get_var(cookie, "session", session, sizeof(session));
            int logged = (session[0] && strcmp(session, g_session_token) == 0);

            if (!logged)
                serve_file(c, "login.html");
            else
                serve_file(c, "control.html");
            return;
        }

        // --- 登录验证 ---
        if (mg_match(hm->uri, mg_str("/login"), NULL)) {
            char user[64] = { 0 }, pass[64] = { 0 };
            mg_http_get_var(&hm->body, "user", user, sizeof(user));
            mg_http_get_var(&hm->body, "password", pass, sizeof(pass));
            if (strcmp(user, USERNAME) == 0 && strcmp(pass, PASSWORD) == 0) {
                srand((unsigned)time(NULL));
                snprintf(g_session_token, sizeof(g_session_token), "%08x%08x", rand(), rand());
                char cookie_hdr[256];
                snprintf(cookie_hdr, sizeof(cookie_hdr),
                    "Set-Cookie: session=%s; Path=/; HttpOnly\r\n"
                    "Location: /\r\n", g_session_token);
                mg_http_reply(c, 302, cookie_hdr, "");
            } else {
                serve_file(c, "fail.html");
            }
            return;
        }

        // --- 退出 ---
        if (mg_match(hm->uri, mg_str("/logout"), NULL)) {
            g_session_token[0] = '\0';
            mg_http_reply(c, 302,
                "Set-Cookie: session=; Path=/; Max-Age=0\r\n"
                "Location: /\r\n", "");
            return;
        }

        // --- 获取图像 (单次快照，兼容旧客户端) ---
        if (mg_match(hm->uri, mg_str("/image"), NULL)) {
            if (g_bmp) {
                mg_printf(c,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/bmp\r\n"
                    "Content-Length: %zu\r\n"
                    "Cache-Control: no-cache\r\n\r\n", g_bmp_size);
                mg_send(c, g_bmp, g_bmp_size);
            } else {
                mg_http_reply(c, 500, NULL, "no image\n");
            }
            return;
        }

        // --- 多流推送 (multipart/x-mixed-replace) ---
        if (mg_match(hm->uri, mg_str("/stream"), NULL)) {
            // 检查流连接数量上限
            if (g_num_streams >= MAX_STREAMS) {
                mg_http_reply(c, 503, NULL, "Too many streams\n");
                return;
            }

            // 确保至少有一帧缓存
            if (!g_bmp) {
                mg_http_reply(c, 500, NULL, "no image\n");
                return;
            }

            // 发送 multipart 响应头
            mg_printf(c,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
                "Cache-Control: no-cache, no-store\r\n"
                "Connection: keep-alive\r\n"
                "\r\n");

            // 发送第一帧 (无前导 \r\n)
            mg_printf(c,
                "--frame\r\n"
                "Content-Type: image/bmp\r\n"
                "Content-Length: %zu\r\n"
                "\r\n", g_bmp_size);
            mg_send(c, g_bmp, g_bmp_size);

            // 注册到流列表
            g_streams[g_num_streams++] = c;
            return;
        }

        // --- 修改参数 ---
        if (mg_match(hm->uri, mg_str("/config"), NULL)) {
            char session[64] = { 0 };
            struct mg_str* cookie = mg_http_get_header(hm, "cookie");
            if (cookie) mg_http_get_var(cookie, "session", session, sizeof(session));
            if (session[0] == 0 || strcmp(session, g_session_token) != 0) {
                mg_http_reply(c, 403, NULL, "Forbidden\n");
                return;
            }

            char w_str[32] = { 0 }, h_str[32] = { 0 }, iv_str[32] = { 0 };
            mg_http_get_var(&hm->body, "width", w_str, sizeof(w_str));
            mg_http_get_var(&hm->body, "height", h_str, sizeof(h_str));
            mg_http_get_var(&hm->body, "interval", iv_str, sizeof(iv_str));

            int nw = atoi(w_str), nh = atoi(h_str), niv = atoi(iv_str);
            if (nw < 1 || nw>4096) nw = 320;
            if (nh < 1 || nh>4096) nh = 240;
            if (niv <= 0) niv = 1;//我改过了，但是没啥用
            g_width = nw;
            g_height = nh;
            g_interval = niv;
            alloc_bmp(nw, nh);

            mg_http_reply(c, 302, "Location: /\r\n", "");
            return;
        }

        // --- 获取当前参数 (JSON) ---
        if (mg_match(hm->uri, mg_str("/config_data"), NULL)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"width\":%d,\"height\":%d,\"interval\":%d}",
                g_width, g_height, g_interval);
            mg_http_reply(c, 200,
                "Content-Type: application/json\r\n"
                "Cache-Control: no-cache\r\n", "%s", buf);
            return;
        }

        mg_http_reply(c, 404, NULL, "Not found\n");
    }
    // 关闭事件：从流列表中移除
    else if (ev == MG_EV_CLOSE) {
        for (int i = 0; i < g_num_streams; i++) {
            if (g_streams[i] == c) {
                g_streams[i] = g_streams[--g_num_streams];
                break;
            }
        }
    }
}

// ===== 主函数 =====
int main(void) {
    /* 优化1: 降级日志级别，屏蔽海量连接日志
     * 方案A: 只显示 ERROR 级别 (默认是 MG_LL_INFO，会打印每条连接) */
    mg_log_set(MG_LL_ERROR);

    /* 方案B (可选): 自定义日志过滤 - 取消下面注释即可启用
     * 会完全禁用 Mongoose 内置日志，需要时可用 fprintf 手动打日志 */
    // mg_log_set_callback(NULL, NULL);

    struct mg_mgr mgr;
    mg_mgr_init(&mgr);

    /* 优化2: 开启长连接 (Keep-Alive)
     * Mongoose 默认已启用 HTTP/1.1 keep-alive，无需额外配置。
     * 每个请求的 Connection 头默认保持，减少 TCP 握手开销。 */
    if (mg_http_listen(&mgr, "http://0.0.0.0:8000", ev_handler, NULL) == NULL) {
        fprintf(stderr, "Cannot start server\n");
        return 1;
    }
    printf("Server: http://localhost:8000\n");
    printf("Log:   Only errors shown (mg_log_set(MG_LL_ERROR)).\n");
    printf("       Alternative: define mg_log_set_callback(NULL,NULL) in main().\n");

    alloc_bmp(g_width, g_height);
    g_last_gen = time(NULL);

    /* 优化3: 主循环按帧间隔定时生成图像 + 推流
     * mg_mgr_poll 第二个参数 50ms = 每 50ms 轮询一次事件，
     * 兼顾低延迟响应和 CPU 占用。 */
    while (g_running) {
        mg_mgr_poll(&mgr, 50);

        time_t now = time(NULL);
        if (now - g_last_gen >= g_interval) {
            static int s_frame = 0;
            size_t hdr = 14 + 40 + 256 * 4;
            generate_frame(g_width, g_height, g_bmp + hdr, s_frame++);

            /* 优化4: 生成后立即推送给所有流连接
             * 浏览器收到 multipart 新帧后自动渲染，无需前端定时器 */
            push_frame_to_streams();

            g_last_gen = now;
        }
    }

    free_bmp();
    mg_mgr_free(&mgr);
    return 0;
}
