// The menus' Brazilian Portuguese (see i18n.h): one table of English -> pt-BR, looked up by the exact English text.
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

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;  // (the start screen's thread and the render thread)

static const char *lookup(std::string_view en) {
    static std::unordered_map<std::string_view, const char *> *map;
    if (!map) {
        map = new std::unordered_map<std::string_view, const char *>(sizeof kPtBr / sizeof kPtBr[0] * 2);
        for (const Pair &p : kPtBr) map->emplace(p.en, p.pt);
    }
    auto it = map->find(en);
    return it == map->end() ? NULL : it->second;
}

const char *T(const char *en) {
    if (!en || !*en || !settings_ui_portuguese()) return en;
    const char *id = strstr(en, "##");
    pthread_mutex_lock(&g_lock);
    const char *r = en;
    if (!id) {
        if (const char *pt = lookup(en)) r = pt;
    } else if (id > en) {  // "Visible##id": the translation + the same ID, kept (interned) for the pointer to stay valid
        static std::unordered_map<std::string, std::string> *cache;  // (node-based: values don't move)
        if (!cache) cache = new std::unordered_map<std::string, std::string>;
        auto it = cache->find(en);
        if (it != cache->end()) r = it->second.c_str();
        else if (const char *pt = lookup(std::string_view(en, id - en)))
            r = cache->emplace(en, std::string(pt) + id).first->second.c_str();
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
