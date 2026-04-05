// snake.cc — Snake game for 64x64 HUB75 LED matrix
// Proper game loop: configurable FPS, decoupled game tick, non-blocking input.
// Usage: sudo ./snake --fps=60 --led-gpio-mapping=footleg-robotics ...

#include "led-matrix.h"
#include "graphics.h"

#include <fcntl.h>
#include <linux/input.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <csignal>
#include <cmath>
#include <vector>
#include <deque>
#include <algorithm>
#include <string>
#include <sys/select.h>
#include <fstream>

using rgb_matrix::RGBMatrix;
using rgb_matrix::FrameCanvas;
using rgb_matrix::Color;

// ------------------------------------------------------------------
// Timing
// ------------------------------------------------------------------
static double now_sec() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

// ------------------------------------------------------------------
// Constants
// ------------------------------------------------------------------
static const int CELL      = 4;
static const int COLS      = 15;  // 15*4=60, centered in 64 with 1px border + 1px pad
static const int ROWS      = 15;
static const int X_OFF     = 2;   // play area starts at pixel 2
static const int Y_OFF     = 2;
static const double TICK_START  = 0.16;
static const double TICK_MIN    = 0.055;

// Configurable
static double g_target_fps = 60.0;

// Palette
static const Color BG          (4, 4, 14);
static const Color GRID_DOT    (10, 10, 25);
static const Color HEAD_COLOR  (0, 255, 110);
static const Color HEAD_INNER  (80, 255, 160);
static const Color HEAD_EYE    (20, 20, 40);
static const Color FOOD_HL     (255, 200, 150);
static const Color DEATH_RED   (120, 0, 0);
static const Color TEXT_DIM    (60, 60, 100);
static const Color TEXT_SCORE  (200, 200, 255);
static const Color GAMEOVER_TXT(255, 60, 60);
static const Color YELLOW      (255, 255, 0);
static const Color DIM_BLUE    (80, 80, 100);
static const Color DIM_PURPLE  (100, 100, 140);
static const Color BORDER_OUTER(40, 30, 80);
static const Color BORDER_INNER(25, 20, 50);
static const Color SCORE_BG    (2, 2, 8);

// ------------------------------------------------------------------
// Direction
// ------------------------------------------------------------------
enum Dir { UP, DOWN, LEFT, RIGHT };
static int DX[] = {0, 0, -1, 1};
static int DY[] = {-1, 1, 0, 0};

// ------------------------------------------------------------------
// HSV to RGB (h: 0-360, s/v: 0-1)
// ------------------------------------------------------------------
static Color hsv(float h, float s, float v) {
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = v * s;
    float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    float m = v - c;
    float r, g, b;
    if      (h < 60)  { r=c; g=x; b=0; }
    else if (h < 120) { r=x; g=c; b=0; }
    else if (h < 180) { r=0; g=c; b=x; }
    else if (h < 240) { r=0; g=x; b=c; }
    else if (h < 300) { r=x; g=0; b=c; }
    else              { r=c; g=0; b=x; }
    return Color((int)((r+m)*255), (int)((g+m)*255), (int)((b+m)*255));
}

// ------------------------------------------------------------------
// Rainbow border: walks the perimeter, assigns hue per pixel
// ------------------------------------------------------------------
static void draw_rainbow_border(FrameCanvas *canvas, int W, int H, int frame) {
    int perimeter = 2 * (W + H) - 4;
    float offset = frame * 3.0f;  // speed of rainbow rotation

    int idx = 0;
    // Top edge: left to right
    for (int x = 0; x < W; x++, idx++) {
        float hue = fmodf(offset + (float)idx / perimeter * 360.0f, 360.0f);
        Color c = hsv(hue, 1.0f, 0.8f);
        canvas->SetPixel(x, 0, c.r, c.g, c.b);
    }
    // Right edge: top+1 to bottom
    for (int y = 1; y < H; y++, idx++) {
        float hue = fmodf(offset + (float)idx / perimeter * 360.0f, 360.0f);
        Color c = hsv(hue, 1.0f, 0.8f);
        canvas->SetPixel(W - 1, y, c.r, c.g, c.b);
    }
    // Bottom edge: right-1 to left
    for (int x = W - 2; x >= 0; x--, idx++) {
        float hue = fmodf(offset + (float)idx / perimeter * 360.0f, 360.0f);
        Color c = hsv(hue, 1.0f, 0.8f);
        canvas->SetPixel(x, H - 1, c.r, c.g, c.b);
    }
    // Left edge: bottom-1 to top+1
    for (int y = H - 2; y >= 1; y--, idx++) {
        float hue = fmodf(offset + (float)idx / perimeter * 360.0f, 360.0f);
        Color c = hsv(hue, 1.0f, 0.8f);
        canvas->SetPixel(0, y, c.r, c.g, c.b);
    }
}

// ------------------------------------------------------------------
// Input
// ------------------------------------------------------------------
enum Key { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BACK };

static int g_kbd_fd = -1;
static volatile bool g_running = true;

static void sig_handler(int) { g_running = false; }

static int find_keyboard() {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        unsigned long evbits = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), &evbits);
        if (evbits & (1 << EV_KEY)) {
            unsigned long keybits[KEY_MAX / (8 * sizeof(unsigned long)) + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits);
            auto has = [&](int k) {
                return keybits[k / (8*sizeof(unsigned long))] &
                       (1UL << (k % (8*sizeof(unsigned long))));
            };
            if (has(KEY_UP) && has(KEY_DOWN) && has(KEY_ENTER)) {
                ioctl(fd, EVIOCGRAB, 1);
                closedir(d);
                char name[256] = "Unknown";
                ioctl(fd, EVIOCGNAME(sizeof(name)), name);
                fprintf(stderr, "Input: %s (%s)\n", name, path);
                return fd;
            }
        }
        close(fd);
    }
    closedir(d);
    return -1;
}

static std::deque<Key> g_key_queue;

static void poll_keys(int fd) {
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv = {0, 0};
    if (select(fd + 1, &fds, nullptr, nullptr, &tv) <= 0) return;
    struct input_event ev[32];
    int n = read(fd, ev, sizeof(ev));
    if (n <= 0) return;
    for (int i = 0; i < n / (int)sizeof(struct input_event); i++) {
        if (ev[i].type == EV_KEY && ev[i].value == 1) {
            Key k = K_NONE;
            switch (ev[i].code) {
                case KEY_UP:        k = K_UP; break;
                case KEY_DOWN:      k = K_DOWN; break;
                case KEY_LEFT:      k = K_LEFT; break;
                case KEY_RIGHT:     k = K_RIGHT; break;
                case KEY_ENTER:
                case KEY_SPACE:     k = K_ENTER; break;
                case KEY_BACKSPACE:
                case KEY_ESC:       k = K_BACK; break;
            }
            if (k != K_NONE) g_key_queue.push_back(k);
        }
    }
}

static Key wait_key(int fd, double timeout) {
    // Check queue first
    if (!g_key_queue.empty()) {
        Key k = g_key_queue.front();
        g_key_queue.pop_front();
        return k;
    }
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    struct timeval tv;
    tv.tv_sec = (int)timeout;
    tv.tv_usec = (int)((timeout - (int)timeout) * 1e6);
    if (select(fd + 1, &fds, nullptr, nullptr, &tv) > 0) {
        poll_keys(fd);
        if (!g_key_queue.empty()) {
            Key k = g_key_queue.front();
            g_key_queue.pop_front();
            return k;
        }
    }
    return K_NONE;
}

// ------------------------------------------------------------------
// 4x6 pixel font
// ------------------------------------------------------------------
struct Glyph { char ch; uint8_t rows[6]; };

static const Glyph FONT[] = {
    {'0', {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'1', {0x2, 0x6, 0x2, 0x2, 0x2, 0x7}},
    {'2', {0x6, 0x9, 0x2, 0x4, 0x8, 0xF}},
    {'3', {0xE, 0x1, 0x6, 0x1, 0x1, 0xE}},
    {'4', {0xA, 0xA, 0xF, 0x2, 0x2, 0x2}},
    {'5', {0xF, 0x8, 0xE, 0x1, 0x1, 0xE}},
    {'6', {0x6, 0x8, 0xE, 0x9, 0x9, 0x6}},
    {'7', {0xF, 0x1, 0x2, 0x4, 0x4, 0x4}},
    {'8', {0x6, 0x9, 0x6, 0x9, 0x9, 0x6}},
    {'9', {0x6, 0x9, 0x9, 0x7, 0x1, 0x6}},
    {'A', {0x6, 0x9, 0x9, 0xF, 0x9, 0x9}},
    {'B', {0xE, 0x9, 0xE, 0x9, 0x9, 0xE}},
    {'C', {0x7, 0x8, 0x8, 0x8, 0x8, 0x7}},
    {'D', {0xE, 0x9, 0x9, 0x9, 0x9, 0xE}},
    {'E', {0xF, 0x8, 0xE, 0x8, 0x8, 0xF}},
    {'F', {0xF, 0x8, 0xE, 0x8, 0x8, 0x8}},
    {'G', {0x6, 0x9, 0x8, 0xB, 0x9, 0x6}},
    {'H', {0x9, 0x9, 0xF, 0x9, 0x9, 0x9}},
    {'I', {0xE, 0x4, 0x4, 0x4, 0x4, 0xE}},
    {'K', {0x9, 0xA, 0xC, 0xC, 0xA, 0x9}},
    {'L', {0x8, 0x8, 0x8, 0x8, 0x8, 0xF}},
    {'M', {0x9, 0xF, 0xF, 0x9, 0x9, 0x9}},
    {'N', {0x9, 0xD, 0xB, 0x9, 0x9, 0x9}},
    {'O', {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'P', {0xE, 0x9, 0x9, 0xE, 0x8, 0x8}},
    {'R', {0xE, 0x9, 0x9, 0xE, 0xA, 0x9}},
    {'S', {0x7, 0x8, 0x6, 0x1, 0x1, 0xE}},
    {'T', {0xF, 0x4, 0x4, 0x4, 0x4, 0x4}},
    {'U', {0x9, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'V', {0x9, 0x9, 0x9, 0x9, 0x6, 0x6}},
    {'W', {0x9, 0x9, 0x9, 0xF, 0xF, 0x9}},
    {'X', {0x9, 0x9, 0x6, 0x6, 0x9, 0x9}},
    {'Y', {0x9, 0x9, 0x6, 0x4, 0x4, 0x4}},
    {'!', {0x4, 0x4, 0x4, 0x4, 0x0, 0x4}},
    {':', {0x0, 0x4, 0x0, 0x0, 0x4, 0x0}},
    {' ', {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},
};
static const int FONT_COUNT = sizeof(FONT) / sizeof(FONT[0]);

static const Glyph *find_glyph(char ch) {
    for (int i = 0; i < FONT_COUNT; i++)
        if (FONT[i].ch == ch) return &FONT[i];
    return nullptr;
}

static void draw_char(FrameCanvas *c, int x, int y, char ch, const Color &col) {
    const Glyph *g = find_glyph(ch);
    if (!g) return;
    for (int row = 0; row < 6; row++) {
        uint8_t bits = g->rows[row];
        for (int ci = 0; ci < 4; ci++) {
            if (bits & (0x8 >> ci))
                c->SetPixel(x + ci, y + row, col.r, col.g, col.b);
        }
    }
}

static void draw_string(FrameCanvas *c, int x, int y, const char *s,
                        const Color &col) {
    for (int i = 0; s[i]; i++)
        draw_char(c, x + i * 5, y, s[i], col);
}

static int string_width(const char *s) {
    int len = strlen(s);
    return len > 0 ? len * 5 - 1 : 0;
}

// ------------------------------------------------------------------
// Drawing helpers
// ------------------------------------------------------------------
static void fill_rect(FrameCanvas *c, int x0, int y0, int x1, int y1,
                      const Color &col) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            c->SetPixel(x, y, col.r, col.g, col.b);
}

static void draw_rect_outline(FrameCanvas *c, int x0, int y0, int x1, int y1,
                              const Color &col) {
    for (int x = x0; x <= x1; x++) {
        c->SetPixel(x, y0, col.r, col.g, col.b);
        c->SetPixel(x, y1, col.r, col.g, col.b);
    }
    for (int y = y0; y <= y1; y++) {
        c->SetPixel(x0, y, col.r, col.g, col.b);
        c->SetPixel(x1, y, col.r, col.g, col.b);
    }
}

// ------------------------------------------------------------------
// High score
// ------------------------------------------------------------------
static std::string best_path() {
    const char *home = getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.hub75_snake_best";
}

static int load_best() {
    std::ifstream f(best_path());
    int v = 0;
    if (f >> v) return v;
    return 0;
}

static void save_best(int score) {
    std::ofstream f(best_path());
    f << score;
}

// ------------------------------------------------------------------
// Game
// ------------------------------------------------------------------
struct Pos { int x, y; };

static void render(RGBMatrix *matrix, FrameCanvas *&canvas,
                   const std::deque<Pos> &snake, const Pos &food,
                   int score, Dir dir, int frame) {
    int W = matrix->width();
    int H = matrix->height();
    canvas->Clear();

    // Rainbow border
    draw_rainbow_border(canvas, W, H, frame);

    // Grid dots
    for (int gx = CELL; gx < COLS * CELL; gx += CELL)
        for (int gy = CELL; gy < ROWS * CELL; gy += CELL)
            canvas->SetPixel(gx + X_OFF, gy + Y_OFF, GRID_DOT.r, GRID_DOT.g, GRID_DOT.b);

    // Food (pulsing)
    {
        float pulse = fabsf((frame % 16) - 8) / 8.0f;
        int gr = (int)(180 * pulse * 0.3f);
        int gg = (int)(60 * pulse * 0.3f);
        int gb = (int)(20 * pulse * 0.3f);
        int fx0 = food.x * CELL + X_OFF;
        int fy0 = food.y * CELL + Y_OFF;
        fill_rect(canvas, fx0 - 1, fy0 - 1, fx0 + CELL, fy0 + CELL,
            Color(gr, gg, gb));
        int bright = (int)(200 + 55 * pulse);
        fill_rect(canvas, fx0, fy0, fx0 + CELL - 1, fy0 + CELL - 1,
            Color(bright, 30 + (int)(20 * pulse), 20));
        canvas->SetPixel(fx0 + 1, fy0 + 1, FOOD_HL.r, FOOD_HL.g, FOOD_HL.b);
    }

    // Snake
    {
        int len = (int)snake.size();
        for (int i = 0; i < len; i++) {
            int x0 = snake[i].x * CELL + X_OFF;
            int y0 = snake[i].y * CELL + Y_OFF;
            int x1 = x0 + CELL - 1;
            int y1 = y0 + CELL - 1;

            if (i == 0) {
                fill_rect(canvas, x0, y0, x1, y1, HEAD_COLOR);
                fill_rect(canvas, x0+1, y0+1, x1-1, y1-1, HEAD_INNER);
                int ex = x0 + 2, ey = y0 + 1;
                if (dir == LEFT)  { ex = x0 + 1; ey = y0 + 1; }
                if (dir == DOWN)  { ex = x0 + 2; ey = y0 + 2; }
                if (dir == UP)    { ex = x0 + 1; ey = y0 + 1; }
                canvas->SetPixel(ex, ey, HEAD_EYE.r, HEAD_EYE.g, HEAD_EYE.b);
            } else {
                float t = (float)i / std::max(len - 1, 1);
                int g = (int)(230 - t * 170);
                int b = (int)(90 + t * 100);
                fill_rect(canvas, x0, y0, x1, y1, Color(0, g, b));
                fill_rect(canvas, x0+1, y0+1, x1-1, y1-1,
                    Color(10, std::min(255, g+35), std::min(255, b+15)));
            }
        }
    }

    // Score overlay (top-right, with dark bg)
    {
        char sc[16];
        snprintf(sc, sizeof(sc), "%d", score);
        int sw = string_width(sc);
        int sx = W - sw - 3;
        int sy = 2;
        draw_string(canvas, sx, sy, sc, TEXT_SCORE);
    }

    canvas = matrix->SwapOnVSync(canvas);
}

static int play(RGBMatrix *matrix, FrameCanvas *&canvas, int kbd_fd) {
    int mid = COLS / 2;
    std::deque<Pos> snake;
    snake.push_back({mid, ROWS / 2});
    snake.push_back({mid - 1, ROWS / 2});
    Dir dir = RIGHT, next_dir = RIGHT;
    int score = 0;
    int frame = 0;
    double game_tick = TICK_START;
    double frame_dt = 1.0 / g_target_fps;

    srand(time(nullptr));

    auto occupied = [&](int x, int y) {
        for (auto &s : snake) if (s.x == x && s.y == y) return true;
        return false;
    };
    auto spawn_food = [&]() -> Pos {
        std::vector<Pos> free;
        for (int x = 0; x < COLS; x++)
            for (int y = 0; y < ROWS; y++)
                if (!occupied(x, y)) free.push_back({x, y});
        return free.empty() ? Pos{0,0} : free[rand() % free.size()];
    };

    Pos food = spawn_food();
    g_key_queue.clear();

    double tick_accum = 0.0;
    double last_time = now_sec();
    int fps_count = 0;
    double fps_timer = last_time;

    while (g_running) {
        double current = now_sec();
        double dt = current - last_time;
        last_time = current;

        // --- Input (queue all keys every frame) ---
        poll_keys(kbd_fd);

        // Check for back/quit
        for (auto it = g_key_queue.begin(); it != g_key_queue.end(); ++it) {
            if (*it == K_BACK) {
                g_key_queue.clear();
                return -1;
            }
        }

        // --- Game logic (fixed timestep) ---
        tick_accum += dt;
        while (tick_accum >= game_tick) {
            tick_accum -= game_tick;

            // Consume one direction from the queue per tick
            while (!g_key_queue.empty()) {
                Key k = g_key_queue.front();
                g_key_queue.pop_front();
                Dir candidate = dir;
                switch (k) {
                    case K_UP:    candidate = UP; break;
                    case K_DOWN:  candidate = DOWN; break;
                    case K_LEFT:  candidate = LEFT; break;
                    case K_RIGHT: candidate = RIGHT; break;
                    default: continue;  // skip non-directional
                }
                // Prevent reversing
                if (!(dir == UP && candidate == DOWN) &&
                    !(dir == DOWN && candidate == UP) &&
                    !(dir == LEFT && candidate == RIGHT) &&
                    !(dir == RIGHT && candidate == LEFT)) {
                    next_dir = candidate;
                    break;  // one direction per tick
                }
            }

            dir = next_dir;
            int nx = snake.front().x + DX[dir];
            int ny = snake.front().y + DY[dir];

            if (nx < 0 || nx >= COLS || ny < 0 || ny >= ROWS) return score;
            if (occupied(nx, ny)) return score;

            snake.push_front({nx, ny});

            if (nx == food.x && ny == food.y) {
                score++;
                food = spawn_food();
                game_tick = std::max(TICK_MIN, TICK_START - score * 0.005);
            } else {
                snake.pop_back();
            }
        }

        // --- Render ---
        render(matrix, canvas, snake, food, score, dir, frame);
        frame++;

        // --- FPS counter ---
        fps_count++;
        if (current - fps_timer >= 1.0) {
            fprintf(stderr, "\rFPS: %d  ", fps_count);
            fps_count = 0;
            fps_timer = current;
        }

        // --- Frame limiter ---
        double elapsed = now_sec() - current;
        double sleep_time = frame_dt - elapsed;
        if (sleep_time > 0) {
            usleep((useconds_t)(sleep_time * 1e6));
        }
    }
    return -1;
}

static bool death_screen(RGBMatrix *matrix, FrameCanvas *&canvas, int kbd_fd,
                         int score) {
    int W = matrix->width();
    int H = matrix->height();

    // Red flash
    for (int i = 0; i < 3; i++) {
        canvas->Clear();
        fill_rect(canvas, 0, 0, W-1, H-1, DEATH_RED);
        canvas = matrix->SwapOnVSync(canvas);
        usleep(60000);
        canvas->Clear();
        fill_rect(canvas, 0, 0, W-1, H-1, BG);
        canvas = matrix->SwapOnVSync(canvas);
        usleep(60000);
    }

    canvas->Clear();
    fill_rect(canvas, 0, 0, W-1, H-1, BG);
    draw_rect_outline(canvas, 2, 2, W-3, H-3, BORDER_OUTER);
    draw_rect_outline(canvas, 3, 3, W-4, H-4, BORDER_INNER);

    draw_string(canvas, 4, 8, "GAME OVER", GAMEOVER_TXT);

    char buf[32];
    snprintf(buf, sizeof(buf), "SCORE:%d", score);
    draw_string(canvas, 4, 22, buf, TEXT_SCORE);

    int best = load_best();
    if (score > best) {
        save_best(score);
        draw_string(canvas, 4, 32, "NEW BEST!", YELLOW);
    } else {
        snprintf(buf, sizeof(buf), "BEST:%d", best);
        draw_string(canvas, 4, 32, buf, DIM_PURPLE);
    }

    draw_string(canvas, 4, 46, "ENTER", DIM_BLUE);
    draw_string(canvas, 4, 54, "BACK", DIM_BLUE);

    canvas = matrix->SwapOnVSync(canvas);

    while (g_running) {
        Key k = wait_key(kbd_fd, 1.0);
        if (k == K_ENTER) return true;
        if (k == K_BACK) return false;
    }
    return false;
}

// ------------------------------------------------------------------
// Main
// ------------------------------------------------------------------
int main(int argc, char *argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    // Parse --fps before handing off to matrix lib
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--fps=", 6) == 0) {
            g_target_fps = atof(argv[i] + 6);
            if (g_target_fps < 1) g_target_fps = 1;
            if (g_target_fps > 1000) g_target_fps = 1000;
            fprintf(stderr, "Target FPS: %.0f\n", g_target_fps);
            // Remove from argv so matrix lib doesn't choke
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j+1];
            argc--;
            i--;
        }
    }

    RGBMatrix::Options opts;
    rgb_matrix::RuntimeOptions rt;
    rt.drop_privileges = 1;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &opts, &rt)) {
        fprintf(stderr, "Usage: %s [--fps=60] [--led-rows=N --led-cols=N ...]\n",
                argv[0]);
        return 1;
    }

    g_kbd_fd = find_keyboard();
    if (g_kbd_fd < 0) {
        fprintf(stderr, "No keyboard found.\n");
        return 1;
    }

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(opts, rt);
    if (!matrix) {
        fprintf(stderr, "Failed to create matrix.\n");
        close(g_kbd_fd);
        return 1;
    }

    FrameCanvas *canvas = matrix->CreateFrameCanvas();

    while (g_running) {
        int score = play(matrix, canvas, g_kbd_fd);
        if (score < 0) break;
        if (!death_screen(matrix, canvas, g_kbd_fd, score)) break;
    }

    canvas->Clear();
    matrix->SwapOnVSync(canvas);
    delete matrix;
    ioctl(g_kbd_fd, EVIOCGRAB, 0);
    close(g_kbd_fd);
    fprintf(stderr, "\nSnake exited.\n");
    return 0;
}
