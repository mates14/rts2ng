#pragma once

#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

namespace rts2web
{

class JsonValue;

typedef std::shared_ptr <JsonValue> JsonPtr;

/**
 * Minimal read-only JSON parser, the reading counterpart of httpd's
 * write-only jsonvalue.cpp.
 *
 * Written rather than pulled in as a dependency for the same reason the
 * serializer was: rts2-httpd's responses are a small, known shape, and
 * neither jsoncpp nor nlohmann/json is otherwise anywhere in this tree -
 * adding a build dependency (and a Debian Build-Depends) to turn a few
 * hundred bytes of `{"name":value}` back into printable text is a poor
 * trade against ~200 lines here. It is a full JSON parser all the same
 * (RFC 8259 grammar, \u escapes with surrogate pairs, nesting depth
 * capped) - a client pointed at the wrong port must fail with an error
 * message, not misparse.
 *
 * Object members keep their document order: /api/getall's per-device
 * value list is meaningful as the device ordered it, and re-sorting it
 * alphabetically (which a std::map would silently do) makes a value
 * listing harder to compare against rts2-mon's, not easier.
 */
class JsonValue
{
	public:
		enum Type { JSON_NULL, JSON_BOOL, JSON_NUMBER, JSON_STRING, JSON_ARRAY, JSON_OBJECT };

		/**
		 * Parse a complete JSON document.
		 *
		 * @throw rts2core::Error on malformed input (including trailing
		 *        garbage after the top-level value).
		 */
		static JsonPtr parse (const std::string &text);

		Type getType () const { return type; }

		bool isNull () const { return type == JSON_NULL; }
		bool isObject () const { return type == JSON_OBJECT; }
		bool isArray () const { return type == JSON_ARRAY; }
		bool isString () const { return type == JSON_STRING; }
		bool isNumber () const { return type == JSON_NUMBER; }

		/** Numeric value; NAN for anything that isn't a number. */
		double asDouble () const;

		/**
		 * The value rendered as one line of plain text: string contents
		 * unquoted, a number exactly as it appeared in the document (no
		 * double round-trip, so 1e300 and 0.1 print as sent), true/false/
		 * null as those words, and a compact re-render for arrays and
		 * objects. This is what the CLI prints for a single value.
		 */
		std::string text () const;

		/** Element count of an array or object; 0 for scalars. */
		size_t size () const;

		/** Array element, or nullptr when out of range / not an array. */
		const JsonValue *at (size_t i) const;

		/** Object member by name, or nullptr when missing / not an object. */
		const JsonValue *get (const char *key) const;

		const std::vector <JsonPtr> &elements () const { return arr; }
		const std::vector <std::pair <std::string, JsonPtr> > &members () const { return obj; }

		/** Indented multi-line rendering, used for --json and for
		 * endpoints with no dedicated formatter. */
		void print (std::ostream &os, int indent = 0) const;

	private:
		Type type = JSON_NULL;
		bool boolean = false;
		double number = 0;
		// For JSON_STRING the decoded contents; for JSON_NUMBER the
		// source text (see text() above).
		std::string str;
		std::vector <JsonPtr> arr;
		std::vector <std::pair <std::string, JsonPtr> > obj;

		friend class JsonParser;
};

}
