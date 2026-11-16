#pragma once
#ifndef ES_CORE_UTILS_I18N_H
#define ES_CORE_UTILS_I18N_H

#include <string>

// Translations of the user interface through GNU gettext (glibc): the catalogue of the text
// domain "emulationstation" in LOCALEDIR (CMake option, default <prefix>/share/locale), for
// example locale/ru/LC_MESSAGES/emulationstation.mo. The language comes from the environment
// (LANGUAGE, LC_ALL, LC_MESSAGES, LANG); only LC_MESSAGES is taken from it, so number parsing
// (themes, settings) keeps the "C" locale. Catalogues are UTF-8.
//   _("text")        translated text (msgid "text")
//   N_("text")       marks "text" for the catalogue without translating it here (a value that
//                    is stored untranslated and shown through _(value) elsewhere)
//   ngettext-style plurals: Utils::I18n::trn("%i GAME", "%i GAMES", n)
namespace Utils
{
	namespace I18n
	{
		void        init();
		const char* tr (const char* _msgid);
		std::string tr (const std::string& _msgid);
		const char* trn(const char* _singular, const char* _plural, unsigned long _n);
	} // I18n::

} // Utils::

#define _(_msgid) Utils::I18n::tr(_msgid)
#define N_(_msgid) _msgid

#endif // ES_CORE_UTILS_I18N_H
