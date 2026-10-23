// The menus' Brazilian Portuguese (see i18n.h): one table of English -> pt-BR, looked up by the exact English text.
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include <ctype.h>
#include <pthread.h>
#include <string.h>
#include <string>
#include <string_view>
#include <unordered_map>

#include "i18n.h"
extern "C" {
#include "../settings.h"
}

struct Pair { const char *en, *pt; };
static const Pair kPtBr[] = {
#include "i18n_pt_br.h"
};
struct Multi { const char *en, *tr[5]; };  // es, fr, de, it, nl (UI_LANG_ES..UI_LANG_NL)
static const Multi kMulti[] = {
#include "i18n_multi.h"
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (the start screen's thread and the render thread)

static const char *lookup(std::string_view en, int lang) {
    static std::unordered_map<std::string_view, const char *> *map;
    static std::unordered_map<std::string_view, const Multi *> *multi;
    if (!map) {
        map = new std::unordered_map<std::string_view, const char *>(sizeof kPtBr / sizeof kPtBr[0] * 2);
        for (const Pair &p : kPtBr) map->emplace(p.en, p.pt);
        multi = new std::unordered_map<std::string_view, const Multi *>;
        for (const Multi &m : kMulti) multi->emplace(m.en, &m);
    }
    if (lang == UI_LANG_PT_BR) {
        auto it = map->find(en);
        return it == map->end() ? NULL : it->second;
    }
    auto it = multi->find(en);
    return it == multi->end() || !*it->second->tr[lang - UI_LANG_ES] ? NULL : it->second->tr[lang - UI_LANG_ES];
}

const char *T(const char *en) {
    int lang = en && *en ? settings_ui_language() : UI_LANG_EN;
    if (lang == UI_LANG_EN) return en;
    const char *id = strstr(en, "##");
    pthread_mutex_lock(&g_lock);
    const char *r = en;
    if (!id) {
        if (const char *pt = lookup(en, lang)) r = pt;
    } else if (id > en) {  // "Visible##id": the translation + the same ID, kept (interned) for the pointer to stay valid
        static std::unordered_map<std::string, std::string> *cache;  // (node-based: values don't move)
        if (!cache) cache = new std::unordered_map<std::string, std::string>;
        std::string key = std::to_string(lang) + en;  // (per language: a switch shows at once)
        auto it = cache->find(key);
        if (it != cache->end()) r = it->second.c_str();
        else if (const char *pt = lookup(std::string_view(en, id - en), lang))
            r = cache->emplace(key, std::string(pt) + id).first->second.c_str();
    }
    pthread_mutex_unlock(&g_lock);
    return r;
}

void ui_upper(std::string &t) {
    for (size_t i = 0; i < t.size(); i++) {
        unsigned char c = (unsigned char)t[i];
        if (c < 0x80) t[i] = (char)toupper(c);
        // U+00E0..U+00FE (à..þ, but ÷) -> U+00C0..U+00DE: 0xC3 0xA0..0xBE -> 0xC3 0x80..0x9E
        else if (c == 0xC3 && i + 1 < t.size()) {
            unsigned char d = (unsigned char)t[i + 1];
            if (d >= 0xA0 && d <= 0xBE && d != 0xB7) t[i + 1] = (char)(d - 0x20);
            i++;
        }
    }
}

// (settings.h) the menus' language: the setting, or the system's for auto
extern "C" int settings_ui_language(void) {
    if (g_settings.ui_language > UI_LANG_AUTO && g_settings.ui_language < N_UI_LANGUAGES) return g_settings.ui_language;
    static int sys = -1;
    if (sys < 0) {
        sys = UI_LANG_EN;
        static const struct { const char *code; int lang; } codes[] = {{"pt", UI_LANG_PT_BR}, {"es", UI_LANG_ES}, {"fr", UI_LANG_FR},
                                                                       {"de", UI_LANG_DE}, {"it", UI_LANG_IT}, {"nl", UI_LANG_NL}};
        int n = 0;
        SDL_Locale **l = SDL_GetPreferredLocales(&n);
        for (size_t i = 0; l && n > 0 && l[0]->language && i < sizeof codes / sizeof codes[0]; i++)
            if (!strcmp(l[0]->language, codes[i].code)) sys = codes[i].lang;
        SDL_free(l);
    }
    return sys;
}
extern "C" bool settings_ui_portuguese(void) { return settings_ui_language() == UI_LANG_PT_BR; }
