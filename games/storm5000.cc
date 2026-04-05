// storm5000.cc — Tube shooter for 64x64 HUB75 LED matrix
// Usage: sudo ./storm5000 --fps=60 --led-gpio-mapping=footleg-robotics ...

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
// Config
// ------------------------------------------------------------------
static double g_target_fps = 60.0;

static const int NUM_LANES = 16;
static const int CX = 32;
static const int CY = 32;
static const int RADIUS = 28;
static const int INNER_R = 4;
static const float DEPTH_RINGS = 4;
static const double GAME_TICK = 1.0 / 30.0;

static const int MAX_LIVES = 3;
static const int MAX_PROJECTILES = 8;
static const float PROJ_SPEED = 2.5f;
static const float ENEMY_BASE_SPEED = 0.3f;
static const int FIRE_COOLDOWN = 4;

// ------------------------------------------------------------------
// Palette
// ------------------------------------------------------------------
static const Color BG         (2, 2, 8);
static const Color PLAYER_COL (0, 255, 220);
static const Color PLAYER_WING(0, 180, 160);
static const Color PROJ_COL   (255, 255, 100);
static const Color TEXT_SCORE  (200, 200, 255);
static const Color GAMEOVER_TXT(255, 60, 60);
static const Color YELLOW     (255, 255, 0);
static const Color DIM_BLUE   (80, 80, 100);
static const Color DIM_PURPLE (100, 100, 140);
static const Color DEATH_RED  (120, 0, 0);

// ------------------------------------------------------------------
// HSV
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
// Input
// ------------------------------------------------------------------
enum Key { K_NONE, K_UP, K_DOWN, K_LEFT, K_RIGHT, K_ENTER, K_BACK };

static int g_kbd_fd = -1;
static volatile bool g_running = true;
static std::deque<Key> g_key_queue;
static bool g_key_held[7] = {};

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

static Key evcode_to_key(int code) {
    switch (code) {
        case KEY_UP:        return K_UP;
        case KEY_DOWN:      return K_DOWN;
        case KEY_LEFT:      return K_LEFT;
        case KEY_RIGHT:     return K_RIGHT;
        case KEY_ENTER:
        case KEY_SPACE:     return K_ENTER;
        case KEY_BACKSPACE:
        case KEY_ESC:       return K_BACK;
        default:            return K_NONE;
    }
}

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
        if (ev[i].type == EV_KEY) {
            Key k = evcode_to_key(ev[i].code);
            if (k == K_NONE) continue;
            if (ev[i].value == 1) {
                g_key_queue.push_back(k);
                g_key_held[k] = true;
            } else if (ev[i].value == 0) {
                g_key_held[k] = false;
            }
        }
    }
}

static Key wait_key(int fd, double timeout) {
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
    {'G', {0x6, 0x9, 0x8, 0xB, 0x9, 0x6}},
    {'I', {0xE, 0x4, 0x4, 0x4, 0x4, 0xE}},
    {'K', {0x9, 0xA, 0xC, 0xC, 0xA, 0x9}},
    {'L', {0x8, 0x8, 0x8, 0x8, 0x8, 0xF}},
    {'M', {0x9, 0xF, 0xF, 0x9, 0x9, 0x9}},
    {'N', {0x9, 0xD, 0xB, 0x9, 0x9, 0x9}},
    {'O', {0x6, 0x9, 0x9, 0x9, 0x9, 0x6}},
    {'R', {0xE, 0x9, 0x9, 0xE, 0xA, 0x9}},
    {'S', {0x7, 0x8, 0x6, 0x1, 0x1, 0xE}},
    {'T', {0xF, 0x4, 0x4, 0x4, 0x4, 0x4}},
    {'V', {0x9, 0x9, 0x9, 0x9, 0x6, 0x6}},
    {'W', {0x9, 0x9, 0x9, 0xF, 0xF, 0x9}},
    {'!', {0x4, 0x4, 0x4, 0x4, 0x0, 0x4}},
    {':', {0x0, 0x4, 0x0, 0x0, 0x4, 0x0}},
    {' ', {0x0, 0x0, 0x0, 0x0, 0x0, 0x0}},
};
static const int FONT_COUNT = sizeof(FONT) / sizeof(FONT[0]);

static void draw_char(FrameCanvas *c, int x, int y, char ch, const Color &col) {
    for (int i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].ch != ch) continue;
        for (int row = 0; row < 6; row++) {
            uint8_t bits = FONT[i].rows[row];
            for (int ci = 0; ci < 4; ci++)
                if (bits & (0x8 >> ci))
                    c->SetPixel(x + ci, y + row, col.r, col.g, col.b);
        }
        return;
    }
}

static void draw_string(FrameCanvas *c, int x, int y, const char *s, const Color &col) {
    for (int i = 0; s[i]; i++)
        draw_char(c, x + i * 5, y, s[i], col);
}

static int string_width(const char *s) {
    int len = strlen(s);
    return len > 0 ? len * 5 - 1 : 0;
}

static void fill_rect(FrameCanvas *c, int x0, int y0, int x1, int y1, const Color &col) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            c->SetPixel(x, y, col.r, col.g, col.b);
}

static void draw_rect_outline(FrameCanvas *c, int x0, int y0, int x1, int y1, const Color &col) {
    for (int x = x0; x <= x1; x++) { c->SetPixel(x, y0, col.r, col.g, col.b); c->SetPixel(x, y1, col.r, col.g, col.b); }
    for (int y = y0; y <= y1; y++) { c->SetPixel(x0, y, col.r, col.g, col.b); c->SetPixel(x1, y, col.r, col.g, col.b); }
}

// ------------------------------------------------------------------
// Bresenham line
// ------------------------------------------------------------------
static void draw_line(FrameCanvas *c, int x0, int y0, int x1, int y1, const Color &col) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true) {
        if (x0 >= 0 && x0 < 64 && y0 >= 0 && y0 < 64)
            c->SetPixel(x0, y0, col.r, col.g, col.b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ------------------------------------------------------------------
// Web geometry
// ------------------------------------------------------------------
struct Vec2 { float x, y; };
static Vec2 g_rim[NUM_LANES];

static void init_web() {
    for (int i = 0; i < NUM_LANES; i++) {
        float angle = i * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f;
        g_rim[i] = { CX + RADIUS * cosf(angle), CY + RADIUS * sinf(angle) };
    }
}

static Vec2 lane_pos(int lane, float depth) {
    float d = INNER_R + (RADIUS - INNER_R) * depth;
    float angle = lane * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f;
    return { CX + d * cosf(angle), CY + d * sinf(angle) };
}

static void draw_web(FrameCanvas *canvas, int frame, int wave) {
    float hue_base = frame * 1.5f + wave * 45.0f;

    // Depth rings
    for (int r = 0; r <= (int)DEPTH_RINGS; r++) {
        float t = (float)r / DEPTH_RINGS;
        float rad = INNER_R + (RADIUS - INNER_R) * t;
        float bright = 0.15f + 0.15f * t;
        for (int i = 0; i < NUM_LANES; i++) {
            int j = (i + 1) % NUM_LANES;
            float a0 = i * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f;
            float a1 = j * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f;
            int x0 = (int)(CX + rad * cosf(a0));
            int y0 = (int)(CY + rad * sinf(a0));
            int x1 = (int)(CX + rad * cosf(a1));
            int y1 = (int)(CY + rad * sinf(a1));
            float hue = fmodf(hue_base + i * (360.0f / NUM_LANES), 360.0f);
            Color col = hsv(hue, 0.8f, bright);
            draw_line(canvas, x0, y0, x1, y1, col);
        }
    }

    // Lane lines
    for (int i = 0; i < NUM_LANES; i++) {
        float hue = fmodf(hue_base + i * (360.0f / NUM_LANES), 360.0f);
        Color col = hsv(hue, 0.7f, 0.25f);
        int x0 = (int)(CX + INNER_R * cosf(i * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f));
        int y0 = (int)(CY + INNER_R * sinf(i * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f));
        draw_line(canvas, x0, y0, (int)g_rim[i].x, (int)g_rim[i].y, col);
    }

    // Rim
    for (int i = 0; i < NUM_LANES; i++) {
        int j = (i + 1) % NUM_LANES;
        float hue = fmodf(hue_base + i * (360.0f / NUM_LANES), 360.0f);
        Color col = hsv(hue, 1.0f, 0.7f);
        draw_line(canvas, (int)g_rim[i].x, (int)g_rim[i].y,
                  (int)g_rim[j].x, (int)g_rim[j].y, col);
    }
}

// ------------------------------------------------------------------
// Game objects
// ------------------------------------------------------------------
struct Projectile {
    int lane;
    float depth;
    bool active;
};

enum EnemyType { E_CRAWLER, E_SPINNER };

struct Enemy {
    int lane;
    float depth;
    EnemyType type;
    bool active;
    int hp;
    float speed;
};

struct Particle {
    float x, y, vx, vy;
    int life;
    Color col;
    bool active;
};

static const int MAX_ENEMIES = 32;
static const int MAX_PARTICLES = 64;

struct GameState {
    int player_lane;
    int lives;
    int score;
    int wave;
    int fire_cd;
    bool alive;

    Projectile projs[MAX_PROJECTILES];
    Enemy enemies[MAX_ENEMIES];
    Particle particles[MAX_PARTICLES];

    int enemies_to_spawn;
    int spawn_cd;
    int spawn_interval;
    float enemy_speed;
};

// ------------------------------------------------------------------
// Particles
// ------------------------------------------------------------------
static void spawn_particles(GameState &gs, float x, float y, const Color &col, int count) {
    for (int n = 0; n < count; n++) {
        for (int i = 0; i < MAX_PARTICLES; i++) {
            if (gs.particles[i].active) continue;
            gs.particles[i].active = true;
            gs.particles[i].x = x;
            gs.particles[i].y = y;
            float angle = (rand() % 360) * M_PI / 180.0f;
            float spd = 0.5f + (rand() % 20) / 10.0f;
            gs.particles[i].vx = cosf(angle) * spd;
            gs.particles[i].vy = sinf(angle) * spd;
            gs.particles[i].life = 8 + rand() % 12;
            gs.particles[i].col = col;
            break;
        }
    }
}

// ------------------------------------------------------------------
// Game logic
// ------------------------------------------------------------------
static void init_wave(GameState &gs) {
    gs.wave++;
    gs.enemies_to_spawn = 4 + gs.wave * 2;
    gs.spawn_interval = std::max(8, 30 - gs.wave * 2);
    gs.spawn_cd = 30;
    gs.enemy_speed = ENEMY_BASE_SPEED + gs.wave * 0.05f;
    if (gs.enemy_speed > 1.2f) gs.enemy_speed = 1.2f;
}

static void init_game(GameState &gs) {
    memset(&gs, 0, sizeof(gs));
    gs.player_lane = 0;
    gs.lives = MAX_LIVES;
    gs.score = 0;
    gs.wave = 0;
    gs.alive = true;
    init_wave(gs);
}

static void spawn_enemy(GameState &gs) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (gs.enemies[i].active) continue;
        gs.enemies[i].active = true;
        gs.enemies[i].lane = rand() % NUM_LANES;
        gs.enemies[i].depth = 0.0f;
        gs.enemies[i].hp = 1;
        gs.enemies[i].speed = gs.enemy_speed * (0.8f + (rand() % 40) / 100.0f);
        if (gs.wave >= 3 && (rand() % 3) == 0) {
            gs.enemies[i].type = E_SPINNER;
            gs.enemies[i].hp = 2;
            gs.enemies[i].speed *= 0.7f;
        } else {
            gs.enemies[i].type = E_CRAWLER;
        }
        break;
    }
}

static void fire(GameState &gs) {
    if (gs.fire_cd > 0) return;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        if (gs.projs[i].active) continue;
        gs.projs[i].active = true;
        gs.projs[i].lane = gs.player_lane;
        gs.projs[i].depth = 1.0f;
        gs.fire_cd = FIRE_COOLDOWN;
        break;
    }
}

static void tick_game(GameState &gs) {
    if (!gs.alive) return;
    if (gs.fire_cd > 0) gs.fire_cd--;

    // Spawning
    if (gs.enemies_to_spawn > 0) {
        gs.spawn_cd--;
        if (gs.spawn_cd <= 0) {
            spawn_enemy(gs);
            gs.enemies_to_spawn--;
            gs.spawn_cd = gs.spawn_interval;
        }
    }

    // Wave clear check
    bool any_active = false;
    for (int i = 0; i < MAX_ENEMIES; i++)
        if (gs.enemies[i].active) { any_active = true; break; }
    if (!any_active && gs.enemies_to_spawn <= 0)
        init_wave(gs);

    // Move projectiles
    float proj_step = PROJ_SPEED / RADIUS;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        if (!gs.projs[i].active) continue;
        gs.projs[i].depth -= proj_step;
        if (gs.projs[i].depth <= 0.0f)
            gs.projs[i].active = false;
    }

    // Move enemies
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!gs.enemies[i].active) continue;
        float step = gs.enemies[i].speed / RADIUS;
        gs.enemies[i].depth += step;

        if (gs.enemies[i].type == E_SPINNER && (rand() % 8) == 0)
            gs.enemies[i].lane = (gs.enemies[i].lane + (rand() % 2 ? 1 : -1) + NUM_LANES) % NUM_LANES;

        if (gs.enemies[i].depth >= 1.0f) {
            gs.enemies[i].active = false;
            if (gs.enemies[i].lane == gs.player_lane) {
                gs.lives--;
                Vec2 p = lane_pos(gs.player_lane, 1.0f);
                spawn_particles(gs, p.x, p.y, Color(255, 100, 100), 12);
                if (gs.lives <= 0) {
                    gs.alive = false;
                    return;
                }
            }
        }
    }

    // Collision: projectile vs enemy
    for (int p = 0; p < MAX_PROJECTILES; p++) {
        if (!gs.projs[p].active) continue;
        for (int e = 0; e < MAX_ENEMIES; e++) {
            if (!gs.enemies[e].active) continue;
            int lane_diff = abs(gs.projs[p].lane - gs.enemies[e].lane);
            if (lane_diff > NUM_LANES / 2) lane_diff = NUM_LANES - lane_diff;
            if (lane_diff > 0) continue;
            float depth_diff = fabsf(gs.projs[p].depth - gs.enemies[e].depth);
            if (depth_diff < 0.12f) {
                gs.enemies[e].hp--;
                gs.projs[p].active = false;
                Vec2 pos = lane_pos(gs.enemies[e].lane, gs.enemies[e].depth);
                if (gs.enemies[e].hp <= 0) {
                    gs.enemies[e].active = false;
                    gs.score += (gs.enemies[e].type == E_SPINNER) ? 200 : 100;
                    spawn_particles(gs, pos.x, pos.y,
                        gs.enemies[e].type == E_SPINNER ? Color(255, 100, 255) : Color(255, 200, 50), 8);
                } else {
                    spawn_particles(gs, pos.x, pos.y, Color(255, 255, 255), 3);
                }
                break;
            }
        }
    }

    // Particles
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!gs.particles[i].active) continue;
        gs.particles[i].x += gs.particles[i].vx;
        gs.particles[i].y += gs.particles[i].vy;
        gs.particles[i].life--;
        if (gs.particles[i].life <= 0) gs.particles[i].active = false;
    }
}

// ------------------------------------------------------------------
// Rendering
// ------------------------------------------------------------------
static void draw_player(FrameCanvas *canvas, int lane, int frame) {
    Vec2 pos = lane_pos(lane, 1.0f);
    int px = (int)pos.x;
    int py = (int)pos.y;

    float angle = lane * 2.0f * M_PI / NUM_LANES - M_PI / 2.0f;
    float inward_x = -cosf(angle);
    float inward_y = -sinf(angle);
    float perp_x = -inward_y;
    float perp_y = inward_x;

    canvas->SetPixel(px, py, PLAYER_COL.r, PLAYER_COL.g, PLAYER_COL.b);

    int wx1 = px + (int)(perp_x * 2);
    int wy1 = py + (int)(perp_y * 2);
    int wx2 = px - (int)(perp_x * 2);
    int wy2 = py - (int)(perp_y * 2);
    if (wx1 >= 0 && wx1 < 64 && wy1 >= 0 && wy1 < 64)
        canvas->SetPixel(wx1, wy1, PLAYER_WING.r, PLAYER_WING.g, PLAYER_WING.b);
    if (wx2 >= 0 && wx2 < 64 && wy2 >= 0 && wy2 < 64)
        canvas->SetPixel(wx2, wy2, PLAYER_WING.r, PLAYER_WING.g, PLAYER_WING.b);

    int ix = px + (int)(inward_x * 1.5f);
    int iy = py + (int)(inward_y * 1.5f);
    if (ix >= 0 && ix < 64 && iy >= 0 && iy < 64)
        canvas->SetPixel(ix, iy, PLAYER_COL.r, PLAYER_COL.g, PLAYER_COL.b);
}

static void draw_enemies(FrameCanvas *canvas, const GameState &gs, int frame) {
    for (int i = 0; i < MAX_ENEMIES; i++) {
        if (!gs.enemies[i].active) continue;
        Vec2 pos = lane_pos(gs.enemies[i].lane, gs.enemies[i].depth);
        int ex = (int)pos.x;
        int ey = (int)pos.y;

        Color col;
        if (gs.enemies[i].type == E_SPINNER) {
            float hue = fmodf(frame * 8.0f + i * 30.0f, 360.0f);
            col = hsv(hue, 1.0f, 1.0f);
        } else {
            col = Color(255, 60 + (int)(gs.enemies[i].depth * 140), 20);
        }

        for (int dy = 0; dy <= 1; dy++)
            for (int dx = 0; dx <= 1; dx++) {
                int px = ex + dx, py = ey + dy;
                if (px >= 0 && px < 64 && py >= 0 && py < 64)
                    canvas->SetPixel(px, py, col.r, col.g, col.b);
            }
    }
}

static void draw_projectiles(FrameCanvas *canvas, const GameState &gs) {
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        if (!gs.projs[i].active) continue;
        Vec2 pos = lane_pos(gs.projs[i].lane, gs.projs[i].depth);
        int px = (int)pos.x;
        int py = (int)pos.y;
        if (px >= 0 && px < 64 && py >= 0 && py < 64)
            canvas->SetPixel(px, py, PROJ_COL.r, PROJ_COL.g, PROJ_COL.b);
    }
}

static void draw_particles(FrameCanvas *canvas, const GameState &gs) {
    for (int i = 0; i < MAX_PARTICLES; i++) {
        if (!gs.particles[i].active) continue;
        int px = (int)gs.particles[i].x;
        int py = (int)gs.particles[i].y;
        if (px < 0 || px >= 64 || py < 0 || py >= 64) continue;
        float fade = (float)gs.particles[i].life / 20.0f;
        if (fade > 1.0f) fade = 1.0f;
        Color c = gs.particles[i].col;
        canvas->SetPixel(px, py,
            (int)(c.r * fade), (int)(c.g * fade), (int)(c.b * fade));
    }
}

static void draw_hud(FrameCanvas *canvas, const GameState &gs) {
    // Score top-left
    char buf[32];
    snprintf(buf, sizeof(buf), "%d", gs.score);
    draw_string(canvas, 1, 1, buf, TEXT_SCORE);

    // Lives top-right as dots
    for (int i = 0; i < gs.lives; i++) {
        int lx = 62 - i * 4;
        canvas->SetPixel(lx, 2, PLAYER_COL.r, PLAYER_COL.g, PLAYER_COL.b);
        canvas->SetPixel(lx, 3, PLAYER_COL.r, PLAYER_COL.g, PLAYER_COL.b);
        canvas->SetPixel(lx - 1, 2, PLAYER_WING.r, PLAYER_WING.g, PLAYER_WING.b);
        canvas->SetPixel(lx + 1, 2, PLAYER_WING.r, PLAYER_WING.g, PLAYER_WING.b);
    }
}

static void render(RGBMatrix *matrix, FrameCanvas *&canvas,
                   const GameState &gs, int frame) {
    canvas->Clear();
    draw_web(canvas, frame, gs.wave);
    draw_projectiles(canvas, gs);
    draw_enemies(canvas, gs, frame);
    draw_player(canvas, gs.player_lane, frame);
    draw_particles(canvas, gs);
    draw_hud(canvas, gs);
    canvas = matrix->SwapOnVSync(canvas);
}

// ------------------------------------------------------------------
// Main game loop
// ------------------------------------------------------------------
static int play(RGBMatrix *matrix, FrameCanvas *&canvas, int kbd_fd) {
    GameState gs;
    init_game(gs);
    g_key_queue.clear();
    memset(g_key_held, 0, sizeof(g_key_held));

    double tick_accum = 0.0;
    double last_time = now_sec();
    double frame_dt = 1.0 / g_target_fps;
    int frame = 0;
    int fps_count = 0;
    double fps_timer = last_time;

    while (g_running) {
        double current = now_sec();
        double dt = current - last_time;
        last_time = current;

        // --- Input ---
        poll_keys(kbd_fd);

        // Check back
        for (auto it = g_key_queue.begin(); it != g_key_queue.end(); ++it) {
            if (*it == K_BACK) {
                g_key_queue.clear();
                return -1;
            }
        }

        // One press = one lane move
        while (!g_key_queue.empty()) {
            Key k = g_key_queue.front();
            g_key_queue.pop_front();
            if (k == K_LEFT || k == K_UP) {
                gs.player_lane = (gs.player_lane - 1 + NUM_LANES) % NUM_LANES;
            } else if (k == K_RIGHT || k == K_DOWN) {
                gs.player_lane = (gs.player_lane + 1) % NUM_LANES;
            }
        }

        // Auto-fire when held
        if (g_key_held[K_ENTER]) {
            fire(gs);
        }

        // --- Game logic (fixed timestep) ---
        tick_accum += dt;
        while (tick_accum >= GAME_TICK) {
            tick_accum -= GAME_TICK;
            tick_game(gs);
            if (!gs.alive) return gs.score;
        }

        // --- Render ---
        render(matrix, canvas, gs, frame);
        frame++;

        // --- FPS ---
        fps_count++;
        if (current - fps_timer >= 1.0) {
            fprintf(stderr, "\rFPS: %d  Score: %d  Wave: %d  ", fps_count, gs.score, gs.wave);
            fps_count = 0;
            fps_timer = current;
        }

        // --- Frame limiter ---
        double elapsed = now_sec() - current;
        double sleep_time = frame_dt - elapsed;
        if (sleep_time > 0)
            usleep((useconds_t)(sleep_time * 1e6));
    }
    return -1;
}

// ------------------------------------------------------------------
// Death screen
// ------------------------------------------------------------------
static bool death_screen(RGBMatrix *matrix, FrameCanvas *&canvas, int kbd_fd, int score) {
    int W = matrix->width(), H = matrix->height();

    for (int i = 0; i < 3; i++) {
        canvas->Clear(); fill_rect(canvas, 0, 0, W-1, H-1, DEATH_RED);
        canvas = matrix->SwapOnVSync(canvas); usleep(60000);
        canvas->Clear(); fill_rect(canvas, 0, 0, W-1, H-1, BG);
        canvas = matrix->SwapOnVSync(canvas); usleep(60000);
    }

    canvas->Clear();
    fill_rect(canvas, 0, 0, W-1, H-1, BG);
    draw_rect_outline(canvas, 2, 2, W-3, H-3, Color(40, 30, 80));
    draw_rect_outline(canvas, 3, 3, W-4, H-4, Color(25, 20, 50));

    draw_string(canvas, 8, 4, "GAME OVER", GAMEOVER_TXT);

    char buf[32];
    draw_string(canvas, 4, 18, "SCORE", TEXT_SCORE);
    snprintf(buf, sizeof(buf), "%d", score);
    draw_string(canvas, 4, 26, buf, YELLOW);

    std::string bp = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.hub75_storm_best";
    int best = 0;
    { std::ifstream f(bp); f >> best; }
    if (score > best) {
        best = score;
        std::ofstream f(bp); f << best;
        draw_string(canvas, 4, 38, "BEST!", YELLOW);
    } else {
        draw_string(canvas, 4, 38, "BEST", DIM_PURPLE);
        snprintf(buf, sizeof(buf), "%d", best);
        draw_string(canvas, 4, 46, buf, DIM_PURPLE);
    }

    draw_string(canvas, 4, 56, "ENTER  BACK", DIM_BLUE);
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

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--fps=", 6) == 0) {
            g_target_fps = atof(argv[i] + 6);
            if (g_target_fps < 1) g_target_fps = 1;
            if (g_target_fps > 1000) g_target_fps = 1000;
            fprintf(stderr, "Target FPS: %.0f\n", g_target_fps);
            for (int j = i; j < argc - 1; j++) argv[j] = argv[j+1];
            argc--; i--;
        }
    }

    RGBMatrix::Options opts;
    rgb_matrix::RuntimeOptions rt;
    rt.drop_privileges = 1;
    if (!rgb_matrix::ParseOptionsFromFlags(&argc, &argv, &opts, &rt)) {
        fprintf(stderr, "Usage: %s [--fps=60] [--led-rows=N ...]\n", argv[0]);
        return 1;
    }

    g_kbd_fd = find_keyboard();
    if (g_kbd_fd < 0) { fprintf(stderr, "No keyboard found.\n"); return 1; }

    RGBMatrix *matrix = RGBMatrix::CreateFromOptions(opts, rt);
    if (!matrix) { fprintf(stderr, "Failed to create matrix.\n"); close(g_kbd_fd); return 1; }

    init_web();
    srand(time(nullptr));

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
    fprintf(stderr, "\nStorm 5000 exited.\n");
    return 0;
}
