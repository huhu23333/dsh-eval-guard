#include "tank.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
using namespace tank;

// ---------- distance fields (cached by terrain hash) ----------
struct Fields {
    bool valid = false;
    uint64_t key = 0;
    float atk[2][CELLS];      // cost (steps) to start damaging base b from cell
    float sup[MAX_SUPPLY][CELLS];
    int fireB[2][CELLS];      // bricks between cell and base b (99 = no line / steel)
    int fireD[2][CELLS];      // cells to base b along line
};
static Fields Fcache[8];
static int Fslot = 0;
static const float FINF = 1e9f;

static uint64_t terrain_key(const State& s) {
    uint64_t h = 1469598103934665603ULL;
    for (int c = 0; c < CELLS; ++c) { h ^= (uint64_t)(s.grid[c] + 1); h *= 1099511628211ULL; }
    h ^= (uint64_t)(s.base_pos[0] * 1000 + s.base_pos[1]);
    for (int k = 0; k < s.n_supply; ++k) h ^= (uint64_t)(s.supply[k] * (k + 7));
    return h;
}

static float enter_cost(const State& s, int cell) {
    uint8_t g = s.grid[cell];
    if (g == EMPTY) return 1.0f;
    if (g == BRICK) return 3.0f;
    return FINF;
}

// Dijkstra over playable cells; dist[] pre-seeded at sources, expanded by enter costs.
static void dijkstra(const State& s, float* dist) {
    static bool done[CELLS];
    for (int c = 0; c < CELLS; ++c) done[c] = false;
    int N = s.P.size;
    for (;;) {
        int u = -1; float best = FINF;
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            int c = cell_of(x, y);
            if (!done[c] && dist[c] < best) { best = dist[c]; u = c; }
        }
        if (u < 0) break;
        done[u] = true;
        for (int d = 0; d < 4; ++d) {
            int v = u + DIR_DELTA[d];
            if (done[v]) continue;
            float ec = enter_cost(s, v);
            if (ec >= FINF) continue;
            float nd = dist[u] + ec;
            if (nd < dist[v]) dist[v] = nd;
        }
    }
}

static void compute_fields(const State& s, Fields& F) {
    int N = s.P.size;
    for (int b = 0; b < 2; ++b) {
        int base = s.base_pos[b], bx = cell_x(base), by = cell_y(base);
        for (int c = 0; c < CELLS; ++c) { F.fireB[b][c] = 99; F.fireD[b][c] = 99; F.atk[b][c] = FINF; }
        for (int y = 0; y < N; ++y) for (int x = 0; x < N; ++x) {
            int c = cell_of(x, y);
            if (c == base) continue;
            if (x != bx && y != by) continue;
            int dx = (bx > x) ? 1 : (bx < x ? -1 : 0);
            int dy = (by > y) ? 1 : (by < y ? -1 : 0);
            int bricks = 0, dist = 0; bool steel = false;
            int cx = x, cy = y;
            while (cx != bx || cy != by) {
                cx += dx; cy += dy; ++dist;
                if (cx == bx && cy == by) break;
                uint8_t g = s.grid[cell_of(cx, cy)];
                if (g == BRICK) ++bricks;
                else if (g == STEEL) steel = true;
                else if (g == BASE0 || g == BASE1) steel = true;
            }
            if (steel) continue;
            F.fireB[b][c] = bricks; F.fireD[b][c] = dist;
            uint8_t g = s.grid[c];
            if (g == EMPTY || g == BRICK) {
                float flightPen = (float)(((dist + 1) / 2 - 1) > 0 ? ((dist + 1) / 2 - 1) : 0) * 3.5f;
                float init = bricks * 3.0f + flightPen + dist * 0.2f + (g == BRICK ? 3.0f : 0.0f);
                if (init < F.atk[b][c]) F.atk[b][c] = init;
            }
        }
        dijkstra(s, F.atk[b]);
    }
    for (int k = 0; k < MAX_SUPPLY; ++k) {
        for (int c = 0; c < CELLS; ++c) F.sup[k][c] = FINF;
        if (k < s.n_supply) { F.sup[k][s.supply[k]] = 0.0f; dijkstra(s, F.sup[k]); }
    }
    F.key = terrain_key(s); F.valid = true;
}

static const Fields& get_fields(const State& s) {
    uint64_t k = terrain_key(s);
    for (int i = 0; i < 8; ++i) if (Fcache[i].valid && Fcache[i].key == k) return Fcache[i];
    Fslot = (Fslot + 1) & 7;
    compute_fields(s, Fcache[Fslot]);
    return Fcache[Fslot];
}

// ---------- raycast ----------
struct Ray { int kind; int cell; int dist; int tank; int seat; }; // kind 0 none 1 brick 2 steel 3 base 4 tank
static Ray raycast(const State& s, const int* tpos, int from, int dir) {
    int c = from, dist = 0;
    for (;;) {
        c += DIR_DELTA[dir]; ++dist;
        uint8_t g = s.grid[c];
        if (g == OUTSIDE) return Ray{0, c, dist, -1, -1};
        if (g == BRICK) return Ray{1, c, dist, -1, -1};
        if (g == STEEL) return Ray{2, c, dist, -1, -1};
        if (g == BASE0 || g == BASE1) return Ray{3, c, dist, -1, (int)g - BASE0};
        int t = tpos[c];
        if (t >= 0) return Ray{4, c, dist, t, seat_of(t)};
    }
}

static bool is_ring(const State& s, int seatB, int cell) {
    int base = s.base_pos[seatB], bx = cell_x(base), by = cell_y(base);
    int dy = (by == 0) ? 1 : -1;
    int cx = cell_x(cell), cy = cell_y(cell);
    if (cy == by && (cx == bx - 1 || cx == bx + 1)) return true;
    if (cy == by + dy && cx >= bx - 1 && cx <= bx + 1) return true;
    return false;
}
static int ring_bricks(const State& s, int seatB) {
    int base = s.base_pos[seatB], bx = cell_x(base), by = cell_y(base);
    int dy = (by == 0) ? 1 : -1, n = 0;
    const int offs[5][2] = {{-1,0},{1,0},{-1,dy},{0,dy},{1,dy}};
    for (auto& o : offs) if (s.grid[cell_of(bx + o[0], by + o[1])] == BRICK) ++n;
    return n;
}
static int dir_toward(int from, int to) { // aligned cells
    int fx = cell_x(from), fy = cell_y(from), tx = cell_x(to), ty = cell_y(to);
    if (tx > fx) return RIGHT; if (tx < fx) return LEFT;
    if (ty > fy) return DOWN; return UP;
}
static bool aligned(int a, int b) { return cell_x(a) == cell_x(b) || cell_y(a) == cell_y(b); }
static int turns_to_spawn(const State& s) {
    int t = s.turn, f = s.P.supply_first, p = s.P.supply_period;
    if (t < f) return f - t;
    int r = (t - f) % p;
    return r == 0 ? p : p - r;
}
static float speed_of(const State& s, int i) { return (float)s.P.types[type_of(i)].speed; }

// Unweighted BFS (only EMPTY passable: real movement) from src; dist in steps, -1 unreachable.
static void bfs_dist(const State& s, int src, int* dist) {
    for (int c = 0; c < CELLS; ++c) dist[c] = -1;
    static int q[CELLS];
    int head = 0, tail = 0;
    dist[src] = 0; q[tail++] = src;
    while (head < tail) {
        int u = q[head++];
        for (int d = 0; d < 4; ++d) {
            int v = u + DIR_DELTA[d];
            if (dist[v] >= 0) continue;
            if (s.grid[v] != EMPTY) continue;
            dist[v] = dist[u] + 1; q[tail++] = v;
        }
    }
}
static int manh(int a, int b) { return std::abs(cell_x(a) - cell_x(b)) + std::abs(cell_y(a) - cell_y(b)); }

// ---------- evaluation ----------
static float side_score(const State& s, const Fields& F, const int* tpos, int seat) {
    const int foe = 1 - seat;
    float sc = 0.0f;
    // material
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type); const Tank& t = s.tank[i];
        if (t.alive) sc += 135.0f + t.hp * 50.0f + t.level * 70.0f + (type == FIRE ? 18 : (type == FAST ? 12 : 10)) + (can_fire(s, i) ? 6.0f : 0.0f);
        else sc -= respawn_in(s, i) * 22.0f;
    }
    // attack ETA (assigned weights: best attacker matters most)
    float eta[3]; int nAlive = 0; float etas[3];
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type); const Tank& t = s.tank[i];
        eta[type] = FINF;
        if (!t.alive) continue;
        float a = F.atk[foe][t.pos];
        eta[type] = (a >= FINF / 2 ? 60.0f : a / speed_of(s, i)) + (can_fire(s, i) ? 0.0f : 0.4f);
        etas[nAlive++] = eta[type];
    }
    std::sort(etas, etas + nAlive);
    const float wgt[3] = {1.0f, 0.5f, 0.22f};
    for (int k = 0; k < nAlive; ++k) {
        float e = std::min(etas[k], 45.0f);
        sc -= wgt[k] * e * 6.0f;
        sc += wgt[k] * 500.0f / (1.0f + e * 0.30f);   // base-pressure: strong pull toward firing range
    }
    // firing positions vs enemy base
    float fb[3] = {0,0,0}, fb2[3] = {0,0,0}; int nfb = 0, nfb2 = 0;
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type); const Tank& t = s.tank[i];
        if (!t.alive || F.fireB[foe][t.pos] >= 99) continue;
        int dir = dir_toward(t.pos, s.base_pos[foe]);
        Ray r = raycast(s, tpos, t.pos, dir);
        if (F.fireB[foe][t.pos] == 0 && r.kind == 3 && r.seat == foe) {
            float flightT = (float)((r.dist + 1) / 2);
            float interval = std::max((float)(reload_turns(s, i) + 1), flightT / (float)std::max(1, bullet_cap(s, i)));
            fb[nfb++] = 430.0f / interval + (can_fire(s, i) ? 40.0f : 12.0f);
        } else if (r.kind == 1)
            fb2[nfb2++] = 95.0f - r.dist * 6.0f - F.fireB[foe][t.pos] * 10.0f + (can_fire(s, i) ? 25.0f : 0.0f);
    }
    std::sort(fb, fb + nfb, std::greater<float>()); std::sort(fb2, fb2 + nfb2, std::greater<float>());
    if (nfb > 0) sc += fb[0] + (nfb > 1 ? 0.35f * fb[1] : 0.0f);
    if (nfb2 > 0) sc += fb2[0] + (nfb2 > 1 ? 0.3f * fb2[1] : 0.0f);
    // defense: enemy threat to my base
    float eEtaMin = 60.0f; int attacker = -1;
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(foe, type); const Tank& t = s.tank[i];
        if (!t.alive) continue;
        float a = F.atk[seat][t.pos];
        float e = (a >= FINF / 2 ? 60.0f : a / speed_of(s, i));
        if (e < eEtaMin) { eEtaMin = e; attacker = i; }
    }
    float threat = std::clamp(11.0f - eEtaMin, 0.0f, 11.0f);
    if (threat > 0.5f && attacker >= 0) {
        float bestDef = 60.0f;
        for (int type = 0; type < 3; ++type) {
            int i = tank_id(seat, type); const Tank& t = s.tank[i];
            if (!t.alive) continue;
            float a = F.atk[seat][t.pos];
            float d = (a >= FINF / 2 ? 60.0f : a / speed_of(s, i));
            if (d < bestDef) bestDef = d;
        }
        sc += threat * std::max(0.0f, 10.0f - std::min(bestDef, 10.0f)) * 2.2f;
        // intercept: gradient toward the attacker's actual position
        bool firingNow = false;
        {
            const Tank& at = s.tank[attacker];
            if (F.fireB[seat][at.pos] == 0) {
                Ray r = raycast(s, tpos, at.pos, dir_toward(at.pos, s.base_pos[seat]));
                firingNow = (r.kind == 3 && r.seat == seat);
            }
        }
        float urgency = threat + (firingNow ? 12.0f : 0.0f);
        static int datt[CELLS];
        bfs_dist(s, s.tank[attacker].pos, datt);
        float dd[3]; int nd = 0;
        for (int type = 0; type < 3; ++type) {
            int i = tank_id(seat, type); const Tank& t = s.tank[i];
            if (!t.alive) continue;
            int d = datt[t.pos];
            dd[nd++] = (d < 0 ? (12.0f + manh(t.pos, s.tank[attacker].pos) * 1.5f) / speed_of(s, i) : d / speed_of(s, i));
        }
        std::sort(dd, dd + nd);
        float dw1 = firingNow ? 95.0f : (2.0f + urgency * 1.7f);
        float dw2 = firingNow ? 35.0f : urgency * 0.5f;
        if (nd > 0) sc -= std::min(dd[0], 30.0f) * dw1;
        if (nd > 1) sc -= std::min(dd[1], 30.0f) * dw2;
        float bestShot = 0.0f;
        int apos = s.tank[attacker].pos;
        for (int type = 0; type < 3; ++type) {
            int i = tank_id(seat, type); const Tank& t = s.tank[i];
            if (!t.alive) continue;
            // lane block between attacker and my base
            int base = s.base_pos[seat];
            if (aligned(t.pos, base) && aligned(t.pos, apos) && F.fireB[seat][t.pos] == 0) {
                int d1 = std::abs(cell_x(apos)-cell_x(base)) + std::abs(cell_y(apos)-cell_y(base));
                int d2 = std::abs(cell_x(t.pos)-cell_x(base)) + std::abs(cell_y(t.pos)-cell_y(base));
                int d3 = std::abs(cell_x(apos)-cell_x(t.pos)) + std::abs(cell_y(apos)-cell_y(t.pos));
                if (d2 + d3 == d1) sc += (t.hp >= 2 ? 45.0f : 18.0f) * threat / 11.0f;
            }
            if (!aligned(t.pos, apos)) continue;
            Ray r = raycast(s, tpos, t.pos, dir_toward(t.pos, apos));
            if (r.kind == 4 && r.tank == attacker) bestShot = std::max(bestShot, 30.0f + threat * 4.0f + (can_fire(s, i) ? 30.0f : 0.0f));
        }
        sc += bestShot;
    }
    // stars
    {
        int tts = turns_to_spawn(s);
        bool usedMine[3] = {false,false,false}, usedFoe[3] = {false,false,false};
        for (int k = 0; k < s.n_supply; ++k) {
            bool present = (s.star_mask >> k) & 1;
            if (!present && tts > 10) continue;
            float myArr = 60.0f, enArr = 60.0f; int myT = -1, enT = -1;
            for (int type = 0; type < 3; ++type) {
                int i = tank_id(seat, type); const Tank& t = s.tank[i];
                if (!t.alive || t.level >= s.P.max_level || usedMine[type]) continue;
                float a = F.sup[k][t.pos]; float arr = (a >= FINF/2 ? 60.0f : a / speed_of(s, i));
                if (arr < myArr) { myArr = arr; myT = type; }
            }
            for (int type = 0; type < 3; ++type) {
                int i = tank_id(foe, type); const Tank& t = s.tank[i];
                if (!t.alive || t.level >= s.P.max_level || usedFoe[type]) continue;
                float a = F.sup[k][t.pos]; float arr = (a >= FINF/2 ? 60.0f : a / speed_of(s, i));
                if (arr < enArr) { enArr = arr; enT = type; }
            }
            if (myT >= 0 && myArr <= enArr + 2.0f) usedMine[myT] = true;
            if (enT >= 0 && enArr <= myArr + 2.0f) usedFoe[enT] = true;
            float value = present ? 110.0f : 55.0f * (1.0f - tts / 22.0f);
            if (myArr <= enArr + 0.5f) sc += value * std::clamp(1.05f - myArr * 0.05f, 0.15f, 1.0f) + std::min(std::max(enArr - myArr, 0.0f), 6.0f) * 3.0f;
            else if (myArr <= enArr + 1.5f || myArr < 4.0f) sc += value * 0.18f * std::clamp(1.0f - myArr * 0.05f, 0.0f, 1.0f);
        }
    }
    // aim at enemy tanks
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type); const Tank& t = s.tank[i];
        if (!t.alive) continue;
        float bestAim = 0.0f;
        for (int et = 0; et < 3; ++et) {
            int j = tank_id(foe, et); const Tank& e = s.tank[j];
            if (!e.alive || !aligned(t.pos, e.pos)) continue;
            Ray r = raycast(s, tpos, t.pos, dir_toward(t.pos, e.pos));
            if (r.kind == 4 && r.tank == j)
                bestAim = std::max(bestAim, (can_fire(s, i) ? 26.0f : 8.0f) + (e.hp <= 1 ? 14.0f : 7.0f) + e.level * 6.0f - r.dist * 0.8f);
        }
        sc += bestAim;
    }
    // bullets in flight
    {
        float bb1 = 0.0f, bb2 = 0.0f;
        for (int b = 0; b < s.n_bullets; ++b) {
            const Bullet& bul = s.bullet[b];
            if (seat_of(bul.owner) != seat) continue;
            Ray r = raycast(s, tpos, bul.pos, bul.dir);
            float flight = (r.dist + 1) / 2.0f;
            if (r.kind == 3) {
                float v = 175.0f / (1.0f + flight * 0.7f);
                if (r.seat == foe) { if (v > bb1) { bb2 = bb1; bb1 = v; } else if (v > bb2) bb2 = v; }
                else sc -= v;
            } else if (r.kind == 4) {
                const Tank& vct = s.tank[r.tank];
                float v = (vct.hp <= 1 ? 70.0f : 40.0f) / (1.0f + flight * 0.6f);
                sc += (r.seat == foe ? v : -v * 1.15f);
            } else if (r.kind == 1 || (r.kind == 2 && bul.steel)) {
                float v = 50.0f / (1.0f + flight * 0.5f);
                if (is_ring(s, foe, r.cell)) sc += v;
                else if (is_ring(s, seat, r.cell)) sc -= v;
                else if (aligned(bul.pos, s.base_pos[foe]) && dir_toward(bul.pos, s.base_pos[foe]) == bul.dir) sc += v * 0.55f;
                else sc += 3.0f;
            }
        }
        sc += bb1 + 0.3f * bb2;
    }
    // spawn hazards + own-ring pocket (trapped, blocks own base exits)
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(seat, type); const Tank& t = s.tank[i];
        if (!t.alive) continue;
        if (is_ring(s, seat, t.pos)) sc -= 35.0f;
        for (int e = 0; e < 6; ++e) {
            if (s.tank[e].alive || s.spawn[e] != t.pos) continue;
            int ri = respawn_in(s, e);
            if (seat_of(e) == foe) sc -= (ri <= 1 ? 380.0f : (ri == 2 ? 70.0f : 15.0f));
            else if (ri <= 1) sc -= 80.0f;
        }
    }
    return sc;
}

static float evaluate(const State& s, int seat) {
    if (s.done) {
        if (s.winner == seat) return 1000000.0f + s.base_hp[seat] * 10.0f;
        if (s.winner == 1 - seat) return -1000000.0f - s.base_hp[1 - seat] * 10.0f;
        return 0.0f;
    }
    const Fields& F = get_fields(s);
    static int tpos[CELLS];
    for (int c = 0; c < CELLS; ++c) tpos[c] = -1;
    for (int i = 0; i < 6; ++i) if (s.tank[i].alive) tpos[s.tank[i].pos] = i;
    float tieScale = 1.0f + std::max(0, (int)s.turn - 230) / 60.0f;
    float sc = (s.base_hp[seat] - s.base_hp[1 - seat]) * 620.0f
             + (s.kills[seat] - s.kills[1 - seat]) * 70.0f * tieScale
             + (s.stars_eaten[seat] - s.stars_eaten[1 - seat]) * 50.0f * tieScale
             + (ring_bricks(s, seat) - ring_bricks(s, 1 - seat)) * 22.0f;
    sc += side_score(s, F, tpos, seat) - side_score(s, F, tpos, 1 - seat);
    return sc;
}

// ---------- robust coordinate-ascent search ----------
static Orders heuristic_orders(const State& s, int side, int mode = 0);  // defined below
static float robust_eval(const State& s, int side, const Orders& mine, const Orders* scen, int nScen) {
    float mn = 1e18f, sum = 0.0f;
    for (int k = 0; k < nScen; ++k) {
        State ns = s;
        step_as(ns, side, mine, scen[k]);
        float v = evaluate(ns, side);
        mn = std::min(mn, v); sum += v;
    }
    return mn;
}

// Optimize `side`'s orders against fixed opponent scenario(s), starting from `start`.
static Orders optimize(const State& s, int side, const Orders* scen, int nScen, Orders start, int passes) {
    Orders cur = start;
    float curScore = robust_eval(s, side, cur, scen, nScen);
    Cmd list[N_CMDS];
    for (int pass = 0; pass < passes; ++pass) {
        bool improved = false;
        for (int type = 0; type < 3; ++type) {
            if (budget_left() < 5.0) return cur;
            int i = tank_id(side, type);
            if (!s.tank[i].alive) { cur.cmd[type] = STAY; continue; }
            int n = legal_commands(s, i, list);
            Cmd bestCmd = cur.cmd[type];
            float bestScore = curScore;
            for (int k = 0; k < n; ++k) {
                if (list[k] == cur.cmd[type]) continue;
                Orders cand = cur; cand.cmd[type] = list[k];
                float v = robust_eval(s, side, cand, scen, nScen);
                if (v > bestScore + 0.01f) { bestScore = v; bestCmd = list[k]; }
            }
            if (bestCmd != cur.cmd[type]) { cur.cmd[type] = bestCmd; curScore = bestScore; improved = true; }
        }
        if (!improved) break;
    }
    return cur;
}

// Simple rusher heuristic: fire at base/enemy/lane-brick, else advance along attack field (or to a nearby star).
static Orders heuristic_orders(const State& s, int side, int mode) {
    const Fields& F = get_fields(s);
    const int foe = 1 - side;
    static int tpos[CELLS];
    for (int c = 0; c < CELLS; ++c) tpos[c] = -1;
    for (int i = 0; i < 6; ++i) if (s.tank[i].alive) tpos[s.tank[i].pos] = i;
    // defense assignment: closest tank intercepts an enemy that is firing at (or about to fire at) our base
    int attacker = -1; float atkEta = 60.0f;
    for (int et = 0; et < 3; ++et) {
        int j = tank_id(foe, et); const Tank& e = s.tank[j];
        if (!e.alive) continue;
        bool firing = false;
        if (F.fireB[side][e.pos] == 0) {
            Ray r = raycast(s, tpos, e.pos, dir_toward(e.pos, s.base_pos[side]));
            firing = (r.kind == 3 && r.seat == side);
        }
        float a = F.atk[side][e.pos];
        float eta = (a >= FINF / 2 ? 60.0f : a / speed_of(s, j));
        if (firing) { attacker = j; atkEta = 0.0f; break; }
        if (eta < atkEta) { atkEta = eta; attacker = j; }
    }
    static float defField[CELLS];
    int defenderType = -1;
    if (mode != 1 && attacker >= 0 && atkEta < 3.5f) {
        static int datt[CELLS];
        bfs_dist(s, s.tank[attacker].pos, datt);
        for (int c = 0; c < CELLS; ++c) defField[c] = (datt[c] < 0 ? 40.0f + (float)manh(c, s.tank[attacker].pos) : (float)datt[c]);
        float bd = 1e9f;
        for (int type = 0; type < 3; ++type) {
            int i = tank_id(side, type); const Tank& t = s.tank[i];
            if (!t.alive) continue;
            float d = (datt[t.pos] < 0 ? 60.0f : datt[t.pos] / speed_of(s, i));
            if (d < bd) { bd = d; defenderType = type; }
        }
    }
    Orders o;
    for (int type = 0; type < 3; ++type) {
        int i = tank_id(side, type); const Tank& t = s.tank[i];
        if (!t.alive) continue;
        bool done = false;
        if (can_fire(s, i)) {
            int fireD = -1, firePri = 99;
            for (int d = 0; d < 4; ++d) {
                Ray r = raycast(s, tpos, t.pos, d);
                int pri = 99;
                if (r.kind == 3 && r.seat == foe) pri = 0;
                else if (r.kind == 4 && r.seat == foe) pri = 1;
                else if (r.kind == 1 && aligned(t.pos, s.base_pos[foe]) && dir_toward(t.pos, s.base_pos[foe]) == d) pri = 2;
                else if (r.kind == 1 && is_ring(s, foe, r.cell)) pri = 2;
                if (pri < firePri) { firePri = pri; fireD = d; }
            }
            if (fireD >= 0) { o.cmd[type] = cmd_fire(fireD); done = true; }
        }
        if (done) continue;
        const float* field = F.atk[foe];
        if (type == defenderType) field = defField;
        else if (mode != 1 && t.level < s.P.max_level && (s.star_mask || (mode == 2 && turns_to_spawn(s) <= 8))) {
            float bestSup = FINF; int bk = -1;
            for (int k = 0; k < s.n_supply; ++k) if (((s.star_mask >> k) & 1) || mode == 2) {
                if (F.sup[k][t.pos] < bestSup) { bestSup = F.sup[k][t.pos]; bk = k; }
            }
            if (bk >= 0 && bestSup < (mode == 2 ? 16.0f : 9.0f)) field = F.sup[bk];
        }
        auto best_step = [&](int from) {
            int bd = -1; float bv = field[from];
            for (int d = 0; d < 4; ++d) {
                int v = from + DIR_DELTA[d];
                if (s.grid[v] == EMPTY && field[v] < bv) { bv = field[v]; bd = d; }
            }
            return bd;
        };
        int d1 = best_step(t.pos);
        if (d1 < 0) {
            // brick ahead on the gradient? shoot it
            if (can_fire(s, i)) {
                for (int d = 0; d < 4; ++d) {
                    int v = t.pos + DIR_DELTA[d];
                    if (s.grid[v] == BRICK && field[v] < field[t.pos]) { o.cmd[type] = cmd_fire(d); break; }
                }
            }
            continue;
        }
        if (type == FAST) {
            int p1 = t.pos + DIR_DELTA[d1];
            int d2 = best_step(p1);
            o.cmd[type] = (d2 >= 0) ? cmd_move2(d1, d2) : cmd_move(d1);
        } else o.cmd[type] = cmd_move(d1);
    }
    return o;
}

void init(const State& s, int me) {
    (void)s; (void)me;
    for (auto& f : Fcache) f.valid = false;
    get_fields(s);
}

Orders decide(const State& s, int me) {
    const int foe = 1 - me;
    Orders hMe = heuristic_orders(s, me);
    Orders hFoe = heuristic_orders(s, foe);
    // 1) enemy optimized plan vs my heuristic rush
    Orders e0 = optimize(s, foe, &hMe, 1, hFoe, 2);
    // 2) my robust response to enemy heuristic + optimized plans (keep all scenarios)
    Orders scenA[2] = {hFoe, e0};
    // multi-start: regular / all-attack / star-priority starts, keep the best robust result
    Orders m1 = optimize(s, me, scenA, 2, hMe, 1);
    {
        float bestSc = robust_eval(s, me, m1, scenA, 2);
        Orders starts[2] = {heuristic_orders(s, me, 1), heuristic_orders(s, me, 2)};
        for (int k = 0; k < 2; ++k) {
            if (starts[k] == hMe) continue;
            Orders cand = optimize(s, me, scenA, 2, starts[k], 1);
            float sc = robust_eval(s, me, cand, scenA, 2);
            if (sc > bestSc + 0.01f) { bestSc = sc; m1 = cand; }
        }
        m1 = optimize(s, me, scenA, 2, m1, 1);
    }
    if (budget_left() < 20.0) return m1;
    // 3) enemy best response to my plan
    Orders e1 = optimize(s, foe, &m1, 1, e0, 2);
    Orders scenB[3] = {hFoe, e0, e1};
    Orders m2 = optimize(s, me, scenB, 3, m1, 2);
    if (budget_left() < 22.0) return m2;
    // 4) one more fictitious-play round, scenarios accumulate
    Orders e2 = optimize(s, foe, &m2, 1, e1, 1);
    Orders scenC[4] = {hFoe, e0, e1, e2};
    Orders m3 = optimize(s, me, scenC, 4, m2, 1);
    if (budget_left() < 25.0) return m3;
    // 5) second extra round when budget is plentiful
    Orders e3 = optimize(s, foe, &m3, 1, e2, 1);
    Orders scenD[5] = {hFoe, e0, e1, e2, e3};
    Orders m4 = optimize(s, me, scenD, 5, m3, 1);
    if (budget_left() < 28.0) return m4;
    Orders e4 = optimize(s, foe, &m4, 1, e3, 1);
    Orders scenE[6] = {hFoe, e0, e1, e2, e3, e4};
    Orders m5 = optimize(s, me, scenE, 6, m4, 2);
    if (budget_left() < 30.0) return m5;
    Orders e5 = optimize(s, foe, &m5, 1, e4, 1);
    Orders scenF[7] = {hFoe, e0, e1, e2, e3, e4, e5};
    Orders m6 = optimize(s, me, scenF, 7, m5, 1);
    return m6;
}
