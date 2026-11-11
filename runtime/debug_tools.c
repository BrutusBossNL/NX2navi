// The debug menu's hands in the game (ui/debug_menu.cpp is its window, F9 with debug_menu = 1). Ocarina of Time 1.1.1.
//
// Reads go straight to the game's memory from the menu's thread, through plat_mem_read, which answers 0 instead of
// faulting. Changes are queued and made on the game's own thread at its pad read (dbg_tick, from ov_input.c), between
// two of the game's frames: a flag through the game's own function, so everything that follows a flag in the game
// follows this change too; a memory value by a plain write into memory the game may read and write.
//
// The game's on/off flags ("game data"): the manager is **[main+0x4c7e180]; +0x10 the number of flags, +0x18 their
// entries (0x18 bytes each, the value's byte at +0x16), +0x20 a table of {hash, index} pairs probed from hash % size
// (+0x2c the size). main+0x20d9c0 (manager, u8 *out, hash) reads a flag and main+0x682b20 (manager, value, hash)
// queues a change, which the manager applies at its next update. A flag is known to the game by the MurmurHash3 of
// its name only; the names shown are those found in the player's own game files (the event flows), hashed here and
// matched (dbg_names), so nothing of them is kept in this code.
//
// The game's speed: the runtime gives the game its step every frame (frame_rate.c ocarina_step_write, the display
// interval); the menu's factor multiplies that step, so 0.5 is half speed for the whole game and its interface.
//
// Link's position is shown but not written: it is the actor's copy ([[main+0x4e371d0]+0x480]+0x31c), which the physics
// writes every frame. Writing it, or every copy of it in the heap, moved Link back within three frames.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zstd.h>

#include "cfg.h"
#include "cpu.h"
#include "debug_tools.h"
#include "fs.h"
#include "host.h"
#include "layout.h"
#include "platform/platform.h"

int dbg_supported(void) { return RT_V111; }

static int rd(uint64_t a, void *out, size_t n) { return a && plat_mem_read(a, out, n); }
static uint64_t rd_ptr(uint64_t a) {
    uint64_t p = 0;
    return rd(a, &p, 8) ? p : 0;
}
// A write into memory the game may read and write: 0 when the place is not such memory.
static int wr(uint64_t a, const void *from, size_t n) {
    RtMemoryInfo mi;
    if (!a || mem_query(a, &mi) || !mi.state || (mi.perm & 3) != 3 || a + n > mi.base + mi.size) return 0;
    mem_thaw(a, n);
    memcpy((void *)(uintptr_t)a, from, n);
    return 1;
}

// ---- the player and the game's speed
static uint64_t player(void) {
    uint64_t manager = rd_ptr(MOD_MAIN_BASE + 0x4e371d0), actor = manager ? rd_ptr(manager + 0x480) : 0;
    char name[7];
    uint64_t np = actor ? rd_ptr(actor + 0x20) : 0;
    return np && rd(np, name, 7) && !memcmp(name, "Player", 7) ? actor : 0;
}

int dbg_player_pos(float xyz[3]) {
    uint64_t a = dbg_supported() ? player() : 0;
    return a && rd(a + 0x31c, xyz, 12) && isfinite(xyz[0]) && isfinite(xyz[1]) && isfinite(xyz[2]);
}

static float speed_factor = 1.0f;
float dbg_speed_factor(void) { return speed_factor; }
void dbg_speed_set(float factor) { speed_factor = factor >= 0.05f && factor <= 8.0f ? factor : 1.0f; }

// ---- what the menu asks for, done by dbg_tick
enum { Q_FLAG, Q_WRITE, Q_MAX = 256, FREEZE_MAX = 64 };
typedef struct { int kind; uint32_t hash; uint64_t addr, value; int size; } Req;
static PlatMutex q_lock = PLAT_MUTEX_INIT;
static Req q[Q_MAX];
static int q_n;
static struct { uint64_t addr, value; int size; } frozen[FREEZE_MAX];
static int frozen_n;
static uint64_t stat_flags, stat_writes, stat_refused;

static void ask(Req r) {
    plat_mutex_lock(&q_lock);
    if (q_n < Q_MAX) q[q_n++] = r;
    plat_mutex_unlock(&q_lock);
}

void dbg_flag_set(uint32_t hash, int on) { ask((Req){Q_FLAG, hash, 0, (uint64_t)(on != 0), 0}); }
void dbg_mem_write(uint64_t addr, uint64_t value, int size) {
    if (addr) ask((Req){Q_WRITE, 0, addr, value, size});
}

int dbg_freeze(uint64_t addr, uint64_t value, int size, int on) {
    if (!addr) return 0;
    int ok = 1;
    plat_mutex_lock(&q_lock);
    int at = -1;
    for (int i = 0; i < frozen_n; i++)
        if (frozen[i].addr == addr) at = i;
    if (on && at < 0 && frozen_n < FREEZE_MAX) at = frozen_n++;
    if (on && at >= 0) frozen[at].addr = addr, frozen[at].value = value, frozen[at].size = size;
    else if (on) ok = 0;
    else if (at >= 0) frozen[at] = frozen[--frozen_n];
    plat_mutex_unlock(&q_lock);
    return ok;
}

int dbg_frozen(uint64_t addr) {
    int is = 0;
    plat_mutex_lock(&q_lock);
    for (int i = 0; i < frozen_n; i++) is |= frozen[i].addr == addr;
    plat_mutex_unlock(&q_lock);
    return is;
}

int dbg_frozen_count(void) { return frozen_n; }

void dbg_unfreeze_all(void) {
    plat_mutex_lock(&q_lock);
    frozen_n = 0;
    plat_mutex_unlock(&q_lock);
}

// ---- the flags
static uint64_t gmd(void) {
    uint64_t slot = rd_ptr(MOD_MAIN_BASE + 0x4c7e180);
    return slot ? rd_ptr(slot) : 0;
}

typedef struct { uint32_t hash, index; } Pair;
static Pair *flag_pairs;       // the flags by hash, sorted; made once the game's table is there
static const char **flag_name; // parallel: the name found for the hash, or NULL
static uint8_t *flag_value;    // parallel: the value at the last dbg_flags_refresh
static int flag_n;
static volatile int32_t names_state;   // 0 not asked, 1 being read, 2 read
static int names_found;

static int by_hash(const void *a, const void *b) {
    uint32_t x = ((const Pair *)a)->hash, y = ((const Pair *)b)->hash;
    return x < y ? -1 : x > y;
}

// The list of the game's flags from its hash table. 0 until the manager exists (a save is loaded).
static int flags_list(void) {
    if (flag_pairs) return 1;
    uint64_t m = gmd(), table = m ? rd_ptr(m + 0x20) : 0;
    uint32_t count = 0, size = 0;
    if (!table || !rd(m + 0x10, &count, 4) || !rd(m + 0x2c, &size, 4) || !count || count > 200000 || size < count ||
        size > 1000000)
        return 0;
    Pair *slots = malloc(sizeof(Pair) * size), *list = malloc(sizeof(Pair) * count);
    int n = 0;
    if (slots && list && rd(table, slots, sizeof(Pair) * size))
        for (uint32_t i = 0; i < size && n < (int)count; i++)
            if (slots[i].hash && slots[i].index < count) list[n++] = slots[i];
    free(slots);
    if (!list || !n) {
        free(list);
        return 0;
    }
    qsort(list, (size_t)n, sizeof(Pair), by_hash);
    flag_name = calloc((size_t)n, sizeof *flag_name);
    flag_value = calloc((size_t)n, 1);
    if (!flag_name || !flag_value) {
        free(list);
        return 0;
    }
    flag_n = n;
    flag_pairs = list;
    rt_log("debug: %d of the game's %u on/off flags listed", n, count);
    return 1;
}

int dbg_flags_refresh(void) {
    if (!dbg_supported() || !flags_list()) return 0;
    uint64_t m = gmd(), entries = m ? rd_ptr(m + 0x18) : 0;
    uint32_t count = 0;
    if (!entries || !rd(m + 0x10, &count, 4) || !count || count > 200000) return flag_n;
    uint8_t *raw = malloc((size_t)count * 0x18);
    if (raw && rd(entries, raw, (size_t)count * 0x18))
        for (int i = 0; i < flag_n; i++)
            if (flag_pairs[i].index < count) flag_value[i] = raw[(size_t)flag_pairs[i].index * 0x18 + 0x16] & 1;
    free(raw);
    return flag_n;
}

uint32_t dbg_flag_hash(int i) { return i >= 0 && i < flag_n ? flag_pairs[i].hash : 0; }
int dbg_flag_value(int i) { return i >= 0 && i < flag_n ? flag_value[i] : 0; }
const char *dbg_flag_name(int i) {
    return i >= 0 && i < flag_n && __atomic_load_n(&names_state, __ATOMIC_ACQUIRE) == 2 ? flag_name[i] : NULL;
}

// ---- the flags' names, from the player's game files
static uint32_t murmur3(const uint8_t *p, size_t len) {
    uint32_t h = 0;
    size_t n = len & ~(size_t)3;
    for (size_t i = 0; i < n; i += 4) {
        uint32_t k;
        memcpy(&k, p + i, 4);
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k, h = (h << 13) | (h >> 19), h = h * 5 + 0xe6546b64u;
    }
    uint32_t k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)p[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)p[n + 1] << 8;    // fall through
    case 1:
        k ^= p[n];
        k *= 0xcc9e2d51u, k = (k << 15) | (k >> 17), k *= 0x1b873593u;
        h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

static int name_char(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.';
}

// Every run of name characters in a file is hashed and looked up among the flags.
static void names_in(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n;) {
        if (!name_char(p[i])) {
            i++;
            continue;
        }
        size_t e = i;
        while (e < n && name_char(p[e])) e++;
        if (e - i >= 4 && e - i <= 96) {
            Pair key = {murmur3(p + i, e - i), 0};
            Pair *hit = bsearch(&key, flag_pairs, (size_t)flag_n, sizeof(Pair), by_hash);
            if (hit && !flag_name[hit - flag_pairs]) {
                char *s = malloc(e - i + 1);
                if (s) {
                    memcpy(s, p + i, e - i);
                    s[e - i] = 0;
                    flag_name[hit - flag_pairs] = s;
                    names_found++;
                }
            }
        }
        i = e;
    }
}

// A member of a SARC archive: its place and size, 0 when it is not there.
static int sarc_member(const uint8_t *p, size_t n, const char *name, size_t *at, size_t *size) {
    if (n < 0x20 || memcmp(p, "SARC", 4)) return 0;
    uint16_t head, count;
    uint32_t data;
    memcpy(&head, p + 4, 2);
    memcpy(&data, p + 12, 4);
    if ((size_t)head + 12 > n || memcmp(p + head, "SFAT", 4)) return 0;
    memcpy(&count, p + head + 6, 2);
    size_t names = (size_t)head + 12 + (size_t)count * 16 + 8, len = strlen(name);
    for (unsigned i = 0; i < count; i++) {
        uint32_t attr, lo, hi;
        size_t e = (size_t)head + 12 + (size_t)i * 16;
        if (e + 16 > n) return 0;
        memcpy(&attr, p + e + 4, 4), memcpy(&lo, p + e + 8, 4), memcpy(&hi, p + e + 12, 4);
        size_t s = names + (size_t)(attr & 0xffffff) * 4;
        if (s + len + 1 > n || memcmp(p + s, name, len + 1)) continue;
        if (lo > hi || (size_t)data + hi > n) return 0;
        *at = data + lo, *size = hi - lo;
        return 1;
    }
    return 0;
}

static void names_thread(void *arg) {
    (void)arg;
    uint64_t t0 = plat_now_ns(), dn = 0;
    uint8_t *dz = romfs_load("Pack/ZsDic.pack.zs", &dn), *dics = NULL;
    ZSTD_DDict *dd = NULL;
    ZSTD_DCtx *dctx = ZSTD_createDCtx();
    unsigned long long dsize = dz ? ZSTD_getFrameContentSize(dz, (size_t)dn) : 0;
    size_t at = 0, size = 0;
    int files = 0;
    if (dz && dsize && dsize < (64u << 20) && (dics = malloc((size_t)dsize)) &&
        ZSTD_decompress(dics, (size_t)dsize, dz, (size_t)dn) == dsize && sarc_member(dics, (size_t)dsize, "zs.zsdic", &at, &size))
        dd = ZSTD_createDDict(dics + at, size);
    if (dd && dctx) {
        uint64_t count = romfs_file_count();
        for (uint64_t i = 0; i < count; i++) {
            uint64_t off, fsize;
            const char *path = romfs_entry(i, &off, &fsize);
            size_t len = path ? strlen(path) : 0;
            if (!path || strncmp(path, "Event/EventFlow/", 16) || len < 4 || strcmp(path + len - 3, ".zs")) continue;
            uint8_t *z = romfs_load(path, &fsize);
            unsigned long long rn = z ? ZSTD_getFrameContentSize(z, (size_t)fsize) : 0;
            uint8_t *raw = z && rn && rn < (64u << 20) ? malloc((size_t)rn) : NULL;
            if (raw && ZSTD_decompress_usingDDict(dctx, raw, (size_t)rn, z, (size_t)fsize, dd) == rn) {
                names_in(raw, (size_t)rn);
                files++;
            }
            free(raw);
            free(z);
        }
    }
    ZSTD_freeDCtx(dctx);
    ZSTD_freeDDict(dd);
    free(dics);
    free(dz);
    rt_log("debug: names for %d of %d flags, from %d of the game's event files, in %.0f ms", names_found, flag_n, files,
           (double)(plat_now_ns() - t0) / 1e6);
    __atomic_store_n(&names_state, 2, __ATOMIC_RELEASE);
}

// 0 not started (no flags yet), 1 being read, 2 there. Starts the reading once the flags are listed.
int dbg_names(int *found) {
    int s = __atomic_load_n(&names_state, __ATOMIC_ACQUIRE);
    if (!s && flag_pairs) {
        int32_t zero = 0;
        if (__atomic_compare_exchange_n(&names_state, &zero, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            if (!plat_thread_spawn(names_thread, NULL, "debug names", 0)) __atomic_store_n(&names_state, 2, __ATOMIC_RELEASE);
            s = 1;
        }
    }
    if (found) *found = s == 2 ? names_found : 0;
    return s;
}

// ---- the memory search: values of 1, 2 or 4 bytes in the game's heap
enum { RESULT_MAX = 2000000 };
static uint64_t *res_addr;
static uint32_t *res_last;
static int res_n, res_size, res_over;

int dbg_mem_read(uint64_t addr, int size, uint32_t *v) {
    *v = 0;
    return (size == 1 || size == 2 || size == 4) && rd(addr, v, (size_t)size);
}

int dbg_search_count(int *over) {
    if (over) *over = res_over;
    return res_n;
}
uint64_t dbg_search_addr(int i) { return i >= 0 && i < res_n ? res_addr[i] : 0; }
int dbg_search_size(void) { return res_size; }
void dbg_search_clear(void) { res_n = res_over = 0; }

// A new search: every place in the heap, aligned to the size, that holds the value. Returns how many (at most
// RESULT_MAX are kept; dbg_search_count's over says there were more).
int dbg_search_new(uint32_t value, int size) {
    enum { PIECE = 16 << 20 };
    if (!res_addr) res_addr = malloc(sizeof(uint64_t) * RESULT_MAX), res_last = malloc(sizeof(uint32_t) * RESULT_MAX);
    res_n = res_over = 0;
    res_size = size;
    if (!res_addr || !res_last || !dbg_supported()) return 0;
    const RtGuestLayout *l = mem_layout();
    const RtRange ranges[2] = {l->heap, l->alias};
    for (int r = 0; r < 2; r++) {
        uint64_t a = ranges[r].base, end = ranges[r].base + ranges[r].size;
        while (a < end) {
            RtMemoryInfo mi;
            if (mem_query(a, &mi) || !mi.size) break;
            uint64_t stop = mi.base + mi.size < end ? mi.base + mi.size : end;
            if (!mi.state || (mi.perm & 3) != 3) {   // free, or not the game's to read and write
                a = stop;
                continue;
            }
            while (a < stop) {
                uint64_t n = stop - a < PIECE ? stop - a : PIECE;
                if (mem_read_hold(a, n)) {
                    const uint8_t *p = (const uint8_t *)(uintptr_t)a;
#define FIND(T)                                                                                                        \
    for (uint64_t i = 0; i + sizeof(T) <= n; i += sizeof(T)) {                                                         \
        T x;                                                                                                           \
        memcpy(&x, p + i, sizeof(T));                                                                                  \
        if (x == (T)value) {                                                                                           \
            if (res_n < RESULT_MAX) res_addr[res_n] = a + i, res_last[res_n++] = value;                                \
            else res_over = 1;                                                                                         \
        }                                                                                                              \
    }
                    if (size == 4) { FIND(uint32_t) } else if (size == 2) { FIND(uint16_t) } else { FIND(uint8_t) }
#undef FIND
                    mem_read_release();
                }
                a += n;
            }
        }
    }
    return res_n;
}

// Keeps the results whose value now passes: mode 0 equals value, 1 changed, 2 unchanged, 3 greater, 4 less than at the
// search before (unsigned; a float is searched by its bits with mode 0).
int dbg_search_next(int mode, uint32_t value) {
    int kept = 0;
    for (int i = 0; i < res_n; i++) {
        uint32_t v, was = res_last[i];
        if (!dbg_mem_read(res_addr[i], res_size, &v)) continue;
        int pass = mode == 0 ? v == value : mode == 1 ? v != was : mode == 2 ? v == was : mode == 3 ? v > was : v < was;
        if (pass) res_addr[kept] = res_addr[i], res_last[kept++] = v;
    }
    res_n = kept;
    res_over = 0;
    return kept;
}

// ---- NX2NAVI_DBG_TEST="<tick>:flag <hash hex> <0|1>;<tick>:speed <factor>;<tick>:find <value> <size>;
// <tick>:next <mode> <value>;<tick>:setfirst <value>;<tick>:holdfirst <value>;<tick>:show [<hash hex>]": the menu's
// requests without the menu, for test runs. Ticks count the game's pad reads from the player's first appearance.
static void test_script(uint64_t tick) {
    static char buf[512];
    static const char *next;
    static int state;
    if (!state) {
        const char *e = cfg_get("NX2NAVI_DBG_TEST");
        state = e && *e ? 1 : -1;
        if (state > 0) snprintf(buf, sizeof buf, "%s", e), next = buf;
    }
    while (state > 0 && next && *next && strtoull(next, NULL, 10) <= tick) {
        const char *colon = strchr(next, ':'), *end = strchr(next, ';');
        if (!colon) break;
        unsigned hash = 0, on = 0, size = 4;
        unsigned long long value = 0;
        float f = 0;
        if (sscanf(colon + 1, "flag %x %u", &hash, &on) == 2) {
            dbg_flag_set(hash, (int)on);
        } else if (sscanf(colon + 1, "speed %f", &f) == 1) {
            dbg_speed_set(f);
        } else if (sscanf(colon + 1, "setfirst %llu", &value) == 1) {
            dbg_mem_write(dbg_search_addr(0), value, dbg_search_size());
        } else if (sscanf(colon + 1, "holdfirst %llu", &value) == 1) {
            dbg_freeze(dbg_search_addr(0), value, dbg_search_size(), value != 0);
        } else if (sscanf(colon + 1, "next %u %llu", &on, &value) == 2) {
            int n = dbg_search_next((int)on, (uint32_t)value);
            uint32_t now = 0;
            dbg_mem_read(dbg_search_addr(0), dbg_search_size(), &now);
            rt_log("debug: tick %llu: %d places left (mode %u, value %llu); the first holds %u; %d held", (unsigned long long)tick, n,
                   on, value, now, dbg_frozen_count());
        } else if (sscanf(colon + 1, "find %llu %u", &value, &size) >= 1) {
            uint64_t t0 = plat_now_ns();
            int n = dbg_search_new((uint32_t)value, (int)size), over = 0;
            dbg_search_count(&over);
            rt_log("debug: tick %llu: %d places hold %llu as %u bytes%s, searched in %.0f ms; first %#llx", (unsigned long long)tick,
                   n, value, size, over ? " (and more)" : "", (double)(plat_now_ns() - t0) / 1e6,
                   (unsigned long long)dbg_search_addr(0));
        } else if (!strncmp(colon + 1, "show", 4)) {
            float p[3] = {0, 0, 0}, sp = dbg_speed_factor();
            int n = dbg_flags_refresh(), set = 0, found = 0;
            for (int i = 0; i < n; i++) set += flag_value[i];
            int names = dbg_names(&found);
            dbg_player_pos(p);
            rt_log("debug: tick %llu: %d flags, %d on; names state %d, %d found; player %.2f %.2f %.2f; speed %.3f; done: %llu flag "
                   "changes, %llu writes, %llu refused", (unsigned long long)tick, n, set, names, found, p[0], p[1], p[2], sp,
                   (unsigned long long)stat_flags, (unsigned long long)stat_writes, (unsigned long long)stat_refused);
            if (sscanf(colon + 1, "show %x", &hash) == 1)
                for (int i = 0; i < n; i++)
                    if (flag_pairs[i].hash == hash)
                        rt_log("debug:   flag %08x (%s) is %d", hash, names == 2 && flag_name[i] ? flag_name[i] : "no name", flag_value[i]);
        }
        next = end ? end + 1 : NULL;
    }
}

// The game's thread, at its pad read.
void dbg_tick(Cpu *c) {
    static uint64_t tick;
    if (!dbg_supported() || (!tick && !player())) return;
    tick++;
    test_script(tick);
    static Req todo[Q_MAX];
    int n;
    plat_mutex_lock(&q_lock);
    n = q_n, q_n = 0;
    memcpy(todo, q, sizeof(Req) * (size_t)n);
    for (int i = 0; i < frozen_n && n < Q_MAX; i++) todo[n++] = (Req){Q_WRITE, 0, frozen[i].addr, frozen[i].value, frozen[i].size};
    plat_mutex_unlock(&q_lock);
    uint64_t m = gmd();
    for (int i = 0; i < n; i++) {
        if (todo[i].kind == Q_FLAG) {
            if (!m) continue;
            rt_call(c, MOD_MAIN_BASE + 0x682b20, 3, m, todo[i].value, (uint64_t)todo[i].hash);
            stat_flags++;
            rt_log("debug: flag %08x set to %s", todo[i].hash, todo[i].value ? "on" : "off");   // what a tester changed, for the log
        } else if (todo[i].size == 1 || todo[i].size == 2 || todo[i].size == 4) {
            if (wr(todo[i].addr, &todo[i].value, (size_t)todo[i].size)) stat_writes++;
            else stat_refused++;
        }
    }
}
