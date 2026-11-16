#include "utils/I18n.h"

#include <clocale>
#include <libintl.h>

#ifndef ES_LOCALEDIR
#define ES_LOCALEDIR "/usr/share/locale"
#endif

#define ES_TEXTDOMAIN "emulationstation"

namespace Utils
{
	namespace I18n
	{
		void init()
		{
			// messages only: LC_NUMERIC and the rest stay "C" (std::stof of theme values)
			setlocale(LC_MESSAGES, "");
			bindtextdomain(ES_TEXTDOMAIN, ES_LOCALEDIR);
			// the catalogue's UTF-8 as it is, also when LC_CTYPE is "C"
			bind_textdomain_codeset(ES_TEXTDOMAIN, "UTF-8");
			textdomain(ES_TEXTDOMAIN);

		} // init

		const char* tr(const char* _msgid)
		{
			// gettext("") returns the catalogue header
			return (_msgid && *_msgid) ? dgettext(ES_TEXTDOMAIN, _msgid) : _msgid;

		} // tr

		std::string tr(const std::string& _msgid)
		{
			return _msgid.empty() ? _msgid : std::string(dgettext(ES_TEXTDOMAIN, _msgid.c_str()));

		} // tr

		const char* trn(const char* _singular, const char* _plural, unsigned long _n)
		{
			return dngettext(ES_TEXTDOMAIN, _singular, _plural, _n);

		} // trn

	} // I18n::

} // Utils::
