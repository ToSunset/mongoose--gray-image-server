/*
 * Pure C Mongoose 8位灰度图像服务器
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
static int g_interval = 1;          // 图像生成间隔(秒)

// ---------- BMP 缓冲区 ----------
static uint8_t* g_bmp = NULL;
static size_t g_bmp_size = 0;
static int g_running = 1;

// ---------- 简易登录 ----------
static char g_session_token[64] = { 0 };
static const char* USERNAME = "admin";
static const char* PASSWORD = "123456";

// 前向声明
void alloc_bmp(int w, int h);
void free_bmp(void);
void generate_frame(int w, int h, uint8_t* pixels, int frame);

// ===== BMP 8位灰度构造 =====
static int row_size(int w) {
    return ((w + 3) / 4) * 4;
}

void alloc_bmp(int w, int h) {
    int rsize = row_size(w);
    size_t headers = 14 + 40 + 256 * 4;   // 文件头+信息头+调色板
    size_t total = headers + (size_t)rsize * h;
    uint8_t* buf = (uint8_t*)malloc(total);
    if (!buf) return;
    memset(buf, 0, total);

    // BMP 头
    buf[0] = 'B'; buf[1] = 'M';
    uint32_t fsize = (uint32_t)total;      memcpy(buf + 2, &fsize, 4);
    uint32_t offset = (uint32_t)headers;   memcpy(buf + 10, &offset, 4);
    uint32_t biSize = 40;                  memcpy(buf + 14, &biSize, 4);
    int32_t biWidth = w;                   memcpy(buf + 18, &biWidth, 4);
    int32_t biHeight = h;                  memcpy(buf + 22, &biHeight, 4);
    uint16_t biPlanes = 1;                 memcpy(buf + 26, &biPlanes, 2);
    uint16_t biBitCount = 8;               memcpy(buf + 28, &biBitCount, 2);

    // 灰度调色板 (B,G,R,0)
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

// 生成动态条纹 + 渐变
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
    mg_http_reply(c, 200, "Content-Type: text/html; charset=gbk\r\n", "%s", data);
    free(data);
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
            }
            else {
                serve_file(c, "fail.html");;
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

        // --- 获取图像 ---
        if (mg_match(hm->uri, mg_str("/image"), NULL)) {
            if (g_bmp) {
                size_t headers = 14 + 40 + 256 * 4;
                static int frame = 0;
                generate_frame(g_width, g_height, g_bmp + headers, frame++);
                mg_printf(c,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: image/bmp\r\n"
                    "Content-Length: %zu\r\n"
                    "Cache-Control: no-cache\r\n\r\n", g_bmp_size);
                mg_send(c, g_bmp, g_bmp_size);
            }
            else {
                mg_http_reply(c, 500, NULL, "no image\n");
            }
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
            if (niv < 1) niv = 1;
            g_width = nw;
            g_height = nh;
            g_interval = niv;
            alloc_bmp(nw, nh);          // 立即重建缓冲区

            mg_http_reply(c, 302, "Location: /\r\n", "");
            return;
        }

        // --- 获取当前参数 (JSON) ---
        if (mg_match(hm->uri, mg_str("/config_data"), NULL)) {
            char buf[128];
            snprintf(buf, sizeof(buf),
                "{\"width\":%d,\"height\":%d,\"interval\":%d}",
                g_width, g_height, g_interval);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", buf);
            return;
        }

        mg_http_reply(c, 404, NULL, "Not found\n");
    }
}

// ===== 主函数 =====
int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    if (mg_http_listen(&mgr, "http://0.0.0.0:8000", ev_handler, NULL) == NULL) {
        fprintf(stderr, "Cannot start server\n");
        return 1;
    }
    printf("Server: http://localhost:8000\n");

    alloc_bmp(g_width, g_height);
    while (g_running) mg_mgr_poll(&mgr, 100);
    free_bmp();
    mg_mgr_free(&mgr);
    return 0;
}