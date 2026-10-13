/* fmt/cgtable.c — dat/MODE.CGM (AST §4; exe FUN_005321c0 loader, FUN_006d1a50 XOR, FUN_006d1a80 LZSS).
 * Header: char magic[16] ("CGTABLE2" / "CGTABLE"), i32 count, 12 zero bytes. Body at 0x20: XOR with
 * crypto_cgm_table[i & 0xff], then byte LZSS. CGTABLE2 entries are 0x3c bytes: name[32], flag_no, code[5],
 * field_38; CGTABLE entries are 0x24 bytes: name[32], flag_no.
 *
 * Memory: the file bytes come from fs (fs_free), the decrypted copy and the decoded body live in a scratch arena for
 * the duration of the load, and the table itself is static with a fixed capacity: nothing to free, nothing to
 * leak. */
#include "fmt/cgtable.h"
#include "core/bin.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/crypto.h"
#include "fmt/lzss.h"
#include "plat/fs.h"
#include <string.h>

enum { CGT_HEADER = 0x20, CGT_MAGIC_LEN = 16, CGT_ENTRY_V2 = 0x3c, CGT_ENTRY_V1 = 0x24, CGT_CODES = 5 };

typedef struct { char name[CGT_NAME_MAX + 1]; int32_t flag_no; int32_t code[CGT_CODES]; } CgEnt;

static CgEnt g_ent[CGT_MAX_ENTRIES];
static int g_n;

/* Entry size for the magic, 0 if unknown. */
static size_t entry_size(const uint8_t magic[CGT_MAGIC_LEN])
{
    char m[CGT_MAGIC_LEN + 1];
    memcpy(m, magic, CGT_MAGIC_LEN);
    m[CGT_MAGIC_LEN] = 0;
    return !strcmp(m, "CGTABLE2") ? CGT_ENTRY_V2 : !strcmp(m, "CGTABLE") ? CGT_ENTRY_V1 : 0;
}

static void parse_entry(Rd *r, size_t esz, CgEnt *e)
{
    (void)rd_bytes(r, e->name, CGT_NAME_MAX);
    e->name[CGT_NAME_MAX] = 0;
    e->flag_no = rd_i32le(r);
    for (int k = 0; k < CGT_CODES; k++) e->code[k] = esz == CGT_ENTRY_V2 ? rd_i32le(r) : 0;
    if (esz == CGT_ENTRY_V2) (void)rd_skip(r, 4);           /* field_38 */
}

/* body = the decoded entry array; fills the table (count entries of esz bytes each). */
static bool parse_entries(const uint8_t *body, size_t len, size_t esz, uint32_t count)
{
    Rd r = rd_make(body, len);
    for (uint32_t i = 0; i < count; i++) parse_entry(&r, esz, &g_ent[i]);
    if (!rd_ok(&r)) return false;
    g_n = (int)count;
    return true;
}

/* d is the whole file (not modified: the body is decrypted in a scratch copy). */
static bool parse_file(const uint8_t *d, size_t n, const char *path)
{
    Rd r = rd_make(d, n);
    const uint8_t *magic = rd_view(&r, CGT_MAGIC_LEN);
    int32_t count = rd_i32le(&r);
    if (!rd_ok(&r) || n < CGT_HEADER + LZSS_HEADER) {
        log_msg(LOG_ERR, "cgtable: %s is truncated", path);
        return false;
    }
    size_t esz = entry_size(magic);
    if (!esz || count < 0 || count > CGT_MAX_ENTRIES) {
        log_msg(LOG_ERR, "cgtable: bad header in %s (count %d, limit %d)", path, (int)count, CGT_MAX_ENTRIES);
        return false;
    }
    Scratch s = mem_scratch_begin(NULL);
    size_t blen = n - CGT_HEADER;
    uint8_t *enc = arena_try_alloc(s.arena, blen, 1);
    const uint8_t *body = NULL;
    uint32_t len = 0;
    if (enc) {
        memcpy(enc, d + CGT_HEADER, blen);
        crypto_xor_cycle(enc, blen, crypto_cgm_table, sizeof crypto_cgm_table, 0);
        body = lzss_decode_arena(s.arena, enc, blen, &len);
    }
    bool ok = body && len >= (size_t)count * esz && parse_entries(body, len, esz, (uint32_t)count);
    mem_scratch_end(&s);
    if (!ok) log_msg(LOG_ERR, "cgtable: decode failed");
    return ok;
}

bool cgt_load_mem(const uint8_t *data, size_t n, const char *what)
{
    g_n = 0;
    return data && parse_file(data, n, what ? what : "MODE.CGM");
}

bool cgt_load(const char *path)
{
    g_n = 0;
    size_t n = 0;
    uint8_t *d = fs_read(path, &n);
    if (!d) return false;
    bool ok = parse_file(d, n, path);
    fs_free(d);
    return ok;
}

int cgt_count(void) { return g_n; }
const char *cgt_name(int i) { return i >= 0 && i < g_n ? g_ent[i].name : ""; }

int cgt_flag_no(const char *name_ci)
{
    if (!name_ci) return -1;
    Str want = str_c(name_ci);
    for (int i = 0; i < g_n; i++)
        if (str_eq_fold(str_c(g_ent[i].name), want)) return g_ent[i].flag_no;
    return -1;
}

