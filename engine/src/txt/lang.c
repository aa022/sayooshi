/* txt/lang.c — game language, engine UI strings in Japanese and English, speaker-name translation for the backlog,
 * language preselection and runtime switch requests (docs/SPEC_ENPATCH.md §1.4, §6).
 *
 * Memory: none. Codes are the static literals "ja" / "en"; name lookups compare on the stack. */
#include "txt/lang.h"
#include "core/log.h"
#include "core/str.h"
#include "plat/fs.h"
#include "vm/vm.h"
#include <SDL3/SDL.h>

enum {
    NAME_UTF8_MAX = 64,         /* bytes of a speaker name compared against the table (the longest is 9) */
    NAME_BANK_GLOBAL = 107,     /* global namae ＊Ａ / ＊Ｂ */
};

static LangId g_pending;
static LangId g_override;

void lang_test_override(LangId id) { g_override = id; }

LangId lang_id(void)
{
    if (g_override != LANG_NONE) return g_override;
    return lang_parse(fs_language()) == LANG_EN ? LANG_EN : LANG_JA;   /* no overlay = the original data */
}

const char *lang_code(void) { return lang_code_of(lang_id()); }
bool lang_is_en(void) { return lang_id() == LANG_EN; }
bool lang_latin_layout(void) { return lang_is_en(); }

static bool in_list(Str s, const char *const *list, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (str_eq_fold(s, str_c(list[i]))) return true;
    return false;
}

LangId lang_parse(const char *s)
{
    static const char *const ja[] = { "ja", "jp", "jpn", "ja_jp", "ja-jp", "japanese", "nihongo",
                                      "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" };
    static const char *const en[] = { "en", "eng", "english", "en_us", "en-us", "en_gb", "en-gb" };
    if (!s || !*s) return LANG_NONE;
    Str in = str_c(s);
    if (in_list(in, ja, COUNT_OF(ja))) return LANG_JA;
    if (in_list(in, en, COUNT_OF(en))) return LANG_EN;
    return LANG_NONE;
}

const char *lang_code_of(LangId id) { return id == LANG_JA ? "ja" : id == LANG_EN ? "en" : NULL; }
LangId lang_from_config(int value) { return value == LANG_JA ? LANG_JA : value == LANG_EN ? LANG_EN : LANG_NONE; }

const char *lang_native_name(LangId id)
{
    return id == LANG_EN ? "English" : "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";   /* 日本語 */
}

bool lang_available(LangId id)
{
    const char *c = lang_code_of(id);
    return c && fs_has_language(c);
}

int lang_available_count(void) { return (int)lang_available(LANG_JA) + (int)lang_available(LANG_EN); }

LangId lang_default(void)
{
    bool en = lang_available(LANG_EN), ja = lang_available(LANG_JA);
    if (!en) return LANG_JA;
    if (!ja) return LANG_EN;
    LangId pick = LANG_EN;
    int n = 0;
    SDL_Locale **loc = SDL_GetPreferredLocales(&n);
    if (loc && n > 0 && loc[0] && loc[0]->language && lang_parse(loc[0]->language) == LANG_JA) pick = LANG_JA;
    SDL_free((void *)loc);                           /* SDL's memory */
    return pick;
}

/* UTF-8 literals. The Japanese ones are the labels the engine used before (WP6); the English menu and dialog
 * texts follow the EN patch's Gameexe/.eng wording (SPEC_ENPATCH §1.3.5, §2). */
static const char *const TEXT_JA[LANG_STR_COUNT] = {
    [LANG_STR_YES] = "はい",
    [LANG_STR_NO] = "いいえ",
    [LANG_STR_BACK] = "戻る",
    [LANG_STR_NO_DATA] = "データがありません。",
    [LANG_STR_SETTINGS] = "環境設定",
    [LANG_STR_SAVE] = "セーブ",
    [LANG_STR_LOAD] = "ロード",
    [LANG_STR_SAVE_WARN] = "セーブしてもよろしいですか？",
    [LANG_STR_LOAD_WARN] = "ロードしてもよろしいですか？",
    [LANG_STR_MENU_READ_SKIP] = "既読文章を早送り",
    [LANG_STR_MENU_UNREAD_SKIP] = "文章を早送り",
    [LANG_STR_MENU_AUTO_SKIP] = "自動早送り",
    [LANG_STR_MENU_AUTO_MODE] = "オートモード",
    [LANG_STR_MENU_HIDE_MWND] = "ウィンドウを消す",
    [LANG_STR_MENU_RETURN_SEL] = "前の選択肢に戻る",
    [LANG_STR_MENU_RETURN_MENU] = "タイトルに戻る",
    [LANG_STR_MENU_GAME_END] = "ゲームを終了する",
    [LANG_STR_MENU_CANCEL] = "キャンセル",
    [LANG_STR_WARN_RETURN_MENU] = "タイトルに戻ってもよろしいですか？",
    [LANG_STR_WARN_RETURN_SEL] = "前の選択肢に戻ってもよろしいですか？",
    [LANG_STR_WARN_GAME_END] = "終了してもよろしいですか？",
    [LANG_STR_SLOT_OTHER_LANG] = "%sのテキストのセーブです。%sで読み込めます。",
    [LANG_STR_SLOT_OTHER_DATA] = "異なるゲームデータで作成されたため、ロードできません。",
    [LANG_STR_SLOT_OLD_FORMAT] = "旧形式のデータです。日本語でのみロードできます。",
    [LANG_STR_SLOT_DAMAGED] = "データが壊れているため、ロードできません。",
    [LANG_STR_SLOT_LANG_TAG_JA] = "日本語",
    [LANG_STR_SLOT_LANG_TAG_EN] = "英語",
    [LANG_STR_CF_VOL_ALL] = "全体音量",
    [LANG_STR_CF_VOL_BGM] = "ＢＧＭ音量",
    [LANG_STR_CF_VOL_KOE] = "音声音量",
    [LANG_STR_CF_VOL_PCM] = "効果音音量",
    [LANG_STR_CF_VOL_SE] = "システム音量",
    [LANG_STR_CF_MSG_SPEED] = "文字表示速度",
    [LANG_STR_CF_AUTO_SPEED] = "オートモード速度",
    [LANG_STR_CF_SKIP_UNREAD] = "未読文章も早送りする",
    [LANG_STR_CF_KOE_DONT_STOP] = "次の文章に進んでも声を止めない",
    [LANG_STR_CF_WHEEL] = "ホイール下回しで文章を進める",
    [LANG_STR_CF_SHADOW] = "文字の影",
    [LANG_STR_CF_WINDOW] = "フルスクリーン",
    [LANG_STR_CF_LANGUAGE] = "言語 / Language",
    [LANG_STR_SHADOW_0] = "なし",
    [LANG_STR_SHADOW_1] = "影",
    [LANG_STR_SHADOW_2] = "縁取り",
    [LANG_STR_SHADOW_3] = "縁取り＋影",
    [LANG_STR_ON] = "ON",
    [LANG_STR_OFF] = "OFF",
    [LANG_STR_LANG_SWITCH_TO_JA] = "日本語に切り替えてタイトルに戻りますか？",
    [LANG_STR_LANG_SWITCH_TO_EN] = "Englishに切り替えてタイトルに戻りますか？",
};

static const char *const TEXT_EN[LANG_STR_COUNT] = {
    [LANG_STR_YES] = "Yes", [LANG_STR_NO] = "No", [LANG_STR_BACK] = "Back", [LANG_STR_NO_DATA] = "No data.",
    [LANG_STR_SETTINGS] = "Settings",
    [LANG_STR_SAVE] = "Save", [LANG_STR_LOAD] = "Load", [LANG_STR_SAVE_WARN] = "Save the game?",
    [LANG_STR_LOAD_WARN] = "Load this save?",
    [LANG_STR_MENU_READ_SKIP] = "Skip Read Text", [LANG_STR_MENU_UNREAD_SKIP] = "Skip Text",
    [LANG_STR_MENU_AUTO_SKIP] = "Auto Skip",
    [LANG_STR_MENU_AUTO_MODE] = "Auto Mode", [LANG_STR_MENU_HIDE_MWND] = "Hide Text",
    [LANG_STR_MENU_RETURN_SEL] = "Return to Previous Choice", [LANG_STR_MENU_RETURN_MENU] = "Return to Title",
    [LANG_STR_MENU_GAME_END] = "Quit Game", [LANG_STR_MENU_CANCEL] = "Cancel",
    [LANG_STR_WARN_RETURN_MENU] = "Return to the title screen?",
    [LANG_STR_WARN_RETURN_SEL] = "Return to the previous choice?",
    [LANG_STR_WARN_GAME_END] = "Quit the game?",
    [LANG_STR_SLOT_OTHER_LANG] = "%s save: switch the language to %s to load it.",
    [LANG_STR_SLOT_OTHER_DATA] = "Made with different game data; it cannot be loaded.",
    [LANG_STR_SLOT_OLD_FORMAT] = "Old-format save; it loads only in Japanese.",
    [LANG_STR_SLOT_DAMAGED] = "This save is damaged and cannot be loaded.",
    [LANG_STR_SLOT_LANG_TAG_JA] = "Japanese", [LANG_STR_SLOT_LANG_TAG_EN] = "English",
    [LANG_STR_CF_VOL_ALL] = "Master Volume", [LANG_STR_CF_VOL_BGM] = "Music Volume",
    [LANG_STR_CF_VOL_KOE] = "Voice Volume",
    [LANG_STR_CF_VOL_PCM] = "Effects Volume", [LANG_STR_CF_VOL_SE] = "System Volume",
    [LANG_STR_CF_MSG_SPEED] = "Text Speed",
    [LANG_STR_CF_AUTO_SPEED] = "Auto Mode Speed", [LANG_STR_CF_SKIP_UNREAD] = "Skip unread text",
    [LANG_STR_CF_KOE_DONT_STOP] = "Keep voices on advance", [LANG_STR_CF_WHEEL] = "Wheel down advances text",
    [LANG_STR_CF_SHADOW] = "Text Shadow", [LANG_STR_CF_WINDOW] = "Fullscreen",
    [LANG_STR_CF_LANGUAGE] = "Language / 言語",
    [LANG_STR_SHADOW_0] = "None", [LANG_STR_SHADOW_1] = "Shadow", [LANG_STR_SHADOW_2] = "Outline",
    [LANG_STR_SHADOW_3] = "Outline + Shadow",
    [LANG_STR_ON] = "ON", [LANG_STR_OFF] = "OFF",
    [LANG_STR_LANG_SWITCH_TO_JA] = "Switch to 日本語 and return to the title?",
    [LANG_STR_LANG_SWITCH_TO_EN] = "Switch to English and return to the title?",
};

const char *lang_str(LangStrId id)
{
    if ((int)id < 0 || id >= LANG_STR_COUNT) return "";
    const char *s = lang_is_en() ? TEXT_EN[id] : TEXT_JA[id];
    return s ? s : "";
}

/* Every NAME string of the English Scene.pck (U+3000 removed). ＊Ａ is expanded from the name bank before this. */
static const struct { const char *ja, *en; } NAMES[] = {
    { "御幸", "Miyuki" }, { "となえ", "Tonae" }, { "こより", "Koyori" }, { "まひる", "Mahiru" },
    { "望美", "Nozomi" }, { "睦月", "Mutsuki" }, { "瀬美奈", "Semina" }, { "天使", "Angel" },
    { "引用", "Quote" }, { "姉", "Sister" }, { "少女達", "Girls" }, { "？？？", "???" },
    { "人見", "Hitomi" }, { "広介", "Hirosuke" },
};

/* The English name for a Japanese NAME string (spaces ignored), NULL if it is not in the table. */
static const char *english_name(const WStr *in)
{
    char mem[NAME_UTF8_MAX];
    StrBuf b;
    sb_init(&b, mem, sizeof mem);
    for (int i = 0; i < in->len; i++)               /* WTF-8, like ws_utf8: names are BMP text */
        if (in->p[i] != 0x3000 && in->p[i] != 0x20) (void)sb_put_cp(&b, in->p[i]);   /* overflow: sb_ok below */
    if (!sb_ok(&b)) return NULL;                     /* longer than every table name */
    for (size_t i = 0; i < COUNT_OF(NAMES); i++)
        if (str_eq(sb_str(&b), str_c(NAMES[i].ja))) return NAMES[i].en;
    return NULL;
}

void lang_display_name_buf(const WStr *in, WBuf *out)
{
    const char *en = lang_is_en() && in && in->len > 0 ? english_name(in) : NULL;
    if (en) (void)wb_put_utf8(out, str_c(en));       /* table literals are valid UTF-8; truncation is visible */
    else if (in && in->len > 0) (void)wb_put(out, in->p, (size_t)in->len);
}

/* Does the bank string equal the UTF-8 literal? (compared in UTF-8 on the stack, like ws_utf8 + strcmp) */
static bool ws_equals_utf8(const WStr *s, const char *lit)
{
    char mem[NAME_UTF8_MAX];
    StrBuf b;
    sb_init(&b, mem, sizeof mem);
    return ws_to_utf8(s, &b) && sb_ok(&b) && str_eq(sb_str(&b), str_c(lit));
}

void lang_migrate_global_names(void)
{
    static const char *const from[2] = { "Hitomi", "Hirosuke" };
    static const char *const to[2] = { "人見", "広介" };
    if (lang_is_en()) return;                /* the EN _start does the other direction itself */
    for (int i = 0; i < 2; i++) {
        WStr *s = vm_bank_str(NAME_BANK_GLOBAL, i);
        if (!s || !ws_equals_utf8(s, from[i])) continue;
        ws_set_utf8(s, to[i]);
        log_msg(LOG_INFO, "lang: global name %c %s -> %s (English default name in Japanese)", 'A' + i, from[i], to[i]);
    }
}

void lang_request_switch(LangId id) { g_pending = lang_available(id) ? id : LANG_NONE; }
LangId lang_pending_switch(void) { return g_pending; }
void lang_clear_switch(void) { g_pending = LANG_NONE; }
