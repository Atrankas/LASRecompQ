// The app's own menus in Brazilian Portuguese and (in part) Spanish, French, German, Italian and Dutch (runtime/ui/i18n.mm): T("English text") is the translation when the menus
// are in Portuguese (settings_ui_portuguese: the App language setting, or the system's language), else the English
// as passed. Looked up by the exact English, at draw time (a language switch shows at once): option tables stay in
// English and go through T() where they're drawn. Text without a translation comes back as passed.
//
//   ImGui::Button(T("Back"));
//   ImGui::Button(T("Play##start"));  // "Jogar##start": only the visible part is translated, the ID stays the same
//   snprintf(buf, sizeof buf, T("%d of %d found"), n, total);  // (the pt-BR text keeps the specifiers, in order)
#pragma once
#include <string>

const char *T(const char *en);
// t in capitals, the accented letters too (Latin-1 range of UTF-8: "Configurações" -> "CONFIGURAÇÕES")
void ui_upper(std::string &t);
