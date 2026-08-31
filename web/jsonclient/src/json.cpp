#include "json.h"

#include "error.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

using namespace rts2web;

namespace rts2web
{

/**
 * Recursive-descent parser over the whole document held in memory - the
 * responses this client reads are HTTP bodies already fully buffered by
 * libcurl, so there is nothing to gain from a streaming parser.
 */
class JsonParser
{
	public:
		JsonParser (const std::string &_text): text (_text) {}

		JsonPtr parseDocument ()
		{
			JsonPtr v = parseValue (0);
			skipWhitespace ();
			if (pos != text.length ())
				fail ("trailing garbage after JSON value");
			return v;
		}

	private:
		// Guards against a hand-crafted (or simply corrupt) response
		// nesting deep enough to blow the C stack in parseValue() - the
		// recursion here is unbounded otherwise, and this parser reads
		// whatever a server on the other end of the socket sends.
		static const int maxDepth = 100;

		const std::string &text;
		size_t pos = 0;

		void fail (const char *what)
		{
			std::ostringstream os;
			os << "invalid JSON at offset " << pos << ": " << what;
			throw rts2core::Error (os.str ());
		}

		void skipWhitespace ()
		{
			while (pos < text.length () && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\n' || text[pos] == '\r'))
				pos++;
		}

		char peek ()
		{
			if (pos >= text.length ())
				fail ("unexpected end of input");
			return text[pos];
		}

		bool literal (const char *word)
		{
			size_t len = strlen (word);
			if (text.compare (pos, len, word) != 0)
				return false;
			pos += len;
			return true;
		}

		JsonPtr parseValue (int depth)
		{
			if (depth > maxDepth)
				fail ("JSON nested too deeply");
			skipWhitespace ();
			switch (peek ())
			{
				case '{':
					return parseObject (depth);
				case '[':
					return parseArray (depth);
				case '"':
				{
					JsonPtr v (new JsonValue ());
					v->type = JsonValue::JSON_STRING;
					v->str = parseString ();
					return v;
				}
				case 't':
				case 'f':
				{
					JsonPtr v (new JsonValue ());
					v->type = JsonValue::JSON_BOOL;
					if (literal ("true"))
						v->boolean = true;
					else if (literal ("false"))
						v->boolean = false;
					else
						fail ("expected true or false");
					return v;
				}
				case 'n':
				{
					JsonPtr v (new JsonValue ());
					v->type = JsonValue::JSON_NULL;
					if (!literal ("null"))
						fail ("expected null");
					return v;
				}
				default:
					return parseNumber ();
			}
		}

		JsonPtr parseObject (int depth)
		{
			JsonPtr v (new JsonValue ());
			v->type = JsonValue::JSON_OBJECT;
			pos++;						 // '{'
			skipWhitespace ();
			if (peek () == '}')
			{
				pos++;
				return v;
			}
			while (true)
			{
				skipWhitespace ();
				if (peek () != '"')
					fail ("expected a quoted member name");
				std::string name = parseString ();
				skipWhitespace ();
				if (peek () != ':')
					fail ("expected ':' after member name");
				pos++;
				v->obj.push_back (std::pair <std::string, JsonPtr> (name, parseValue (depth + 1)));
				skipWhitespace ();
				char c = peek ();
				pos++;
				if (c == '}')
					return v;
				if (c != ',')
					fail ("expected ',' or '}' in object");
			}
		}

		JsonPtr parseArray (int depth)
		{
			JsonPtr v (new JsonValue ());
			v->type = JsonValue::JSON_ARRAY;
			pos++;						 // '['
			skipWhitespace ();
			if (peek () == ']')
			{
				pos++;
				return v;
			}
			while (true)
			{
				v->arr.push_back (parseValue (depth + 1));
				skipWhitespace ();
				char c = peek ();
				pos++;
				if (c == ']')
					return v;
				if (c != ',')
					fail ("expected ',' or ']' in array");
			}
		}

		JsonPtr parseNumber ()
		{
			size_t start = pos;
			if (pos < text.length () && (text[pos] == '-' || text[pos] == '+'))
				pos++;
			while (pos < text.length () && (isdigit (text[pos]) || text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E'
				|| ((text[pos] == '-' || text[pos] == '+') && (text[pos - 1] == 'e' || text[pos - 1] == 'E'))))
				pos++;
			if (pos == start)
				fail ("expected a value");

			std::string raw = text.substr (start, pos - start);
			const char *s = raw.c_str ();
			char *end = nullptr;
			double d = strtod (s, &end);
			if (end == nullptr || *end != '\0')
			{
				pos = start;
				fail ("malformed number");
			}

			JsonPtr v (new JsonValue ());
			v->type = JsonValue::JSON_NUMBER;
			v->number = d;
			v->str = raw;
			return v;
		}

		/** Append one Unicode code point to out as UTF-8. */
		static void appendUtf8 (std::string &out, unsigned int cp)
		{
			if (cp < 0x80)
			{
				out += (char) cp;
			}
			else if (cp < 0x800)
			{
				out += (char) (0xc0 | (cp >> 6));
				out += (char) (0x80 | (cp & 0x3f));
			}
			else if (cp < 0x10000)
			{
				out += (char) (0xe0 | (cp >> 12));
				out += (char) (0x80 | ((cp >> 6) & 0x3f));
				out += (char) (0x80 | (cp & 0x3f));
			}
			else
			{
				out += (char) (0xf0 | (cp >> 18));
				out += (char) (0x80 | ((cp >> 12) & 0x3f));
				out += (char) (0x80 | ((cp >> 6) & 0x3f));
				out += (char) (0x80 | (cp & 0x3f));
			}
		}

		unsigned int parseHex4 ()
		{
			if (pos + 4 > text.length ())
				fail ("truncated \\u escape");
			unsigned int cp = 0;
			for (int i = 0; i < 4; i++)
			{
				char c = text[pos + i];
				cp <<= 4;
				if (c >= '0' && c <= '9')
					cp |= c - '0';
				else if (c >= 'a' && c <= 'f')
					cp |= c - 'a' + 10;
				else if (c >= 'A' && c <= 'F')
					cp |= c - 'A' + 10;
				else
					fail ("non-hexadecimal digit in \\u escape");
			}
			pos += 4;
			return cp;
		}

		std::string parseString ()
		{
			pos++;						 // opening '"'
			std::string out;
			while (true)
			{
				if (pos >= text.length ())
					fail ("unterminated string");
				char c = text[pos++];
				if (c == '"')
					return out;
				if (c != '\\')
				{
					out += c;
					continue;
				}
				if (pos >= text.length ())
					fail ("unterminated escape sequence");
				char e = text[pos++];
				switch (e)
				{
					case '"':
					case '\\':
					case '/':
						out += e;
						break;
					case 'b':
						out += '\b';
						break;
					case 'f':
						out += '\f';
						break;
					case 'n':
						out += '\n';
						break;
					case 'r':
						out += '\r';
						break;
					case 't':
						out += '\t';
						break;
					case 'u':
					{
						unsigned int cp = parseHex4 ();
						// A code point outside the BMP arrives as a
						// \uD8xx\uDCxx surrogate pair; decoding the two
						// halves independently would emit two invalid
						// UTF-8 sequences instead of the one character
						// they jointly encode.
						if (cp >= 0xd800 && cp <= 0xdbff && pos + 1 < text.length () && text[pos] == '\\' && text[pos + 1] == 'u')
						{
							size_t save = pos;
							pos += 2;
							unsigned int low = parseHex4 ();
							if (low >= 0xdc00 && low <= 0xdfff)
								cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
							else
								pos = save;
						}
						appendUtf8 (out, cp);
						break;
					}
					default:
						fail ("unknown escape sequence");
				}
			}
		}
};

}

JsonPtr JsonValue::parse (const std::string &text)
{
	JsonParser parser (text);
	return parser.parseDocument ();
}

double JsonValue::asDouble () const
{
	switch (type)
	{
		case JSON_NUMBER:
			return number;
		case JSON_BOOL:
			return boolean ? 1 : 0;
		default:
			return NAN;
	}
}

size_t JsonValue::size () const
{
	switch (type)
	{
		case JSON_ARRAY:
			return arr.size ();
		case JSON_OBJECT:
			return obj.size ();
		default:
			return 0;
	}
}

const JsonValue *JsonValue::at (size_t i) const
{
	if (type != JSON_ARRAY || i >= arr.size ())
		return nullptr;
	return arr[i].get ();
}

const JsonValue *JsonValue::get (const char *key) const
{
	if (type != JSON_OBJECT)
		return nullptr;
	for (std::vector <std::pair <std::string, JsonPtr> >::const_iterator iter = obj.begin (); iter != obj.end (); iter++)
	{
		if (iter->first == key)
			return iter->second.get ();
	}
	return nullptr;
}

std::string JsonValue::text () const
{
	switch (type)
	{
		case JSON_NULL:
			return "null";
		case JSON_BOOL:
			return boolean ? "true" : "false";
		case JSON_NUMBER:
		case JSON_STRING:
			return str;
		case JSON_ARRAY:
		case JSON_OBJECT:
		default:
		{
			// Containers print as one compact line here (arrays of
			// numbers - an RTS2 array value - are the common case, and
			// "[1,2,3]" on the value's own line reads better than four
			// lines of pretty-printing); print() is what expands them.
			std::ostringstream os;
			if (type == JSON_ARRAY)
			{
				os << "[";
				for (size_t i = 0; i < arr.size (); i++)
				{
					if (i)
						os << ",";
					if (arr[i]->type == JSON_STRING)
						os << "\"" << arr[i]->text () << "\"";
					else
						os << arr[i]->text ();
				}
				os << "]";
			}
			else
			{
				os << "{";
				for (size_t i = 0; i < obj.size (); i++)
				{
					if (i)
						os << ",";
					os << obj[i].first << ":";
					if (obj[i].second->type == JSON_STRING)
						os << "\"" << obj[i].second->text () << "\"";
					else
						os << obj[i].second->text ();
				}
				os << "}";
			}
			return os.str ();
		}
	}
}

/**
 * JSON-escape a string for print()'s output. text() deliberately does
 * *not* do this - it is the human-facing rendering (a value printed as
 * `EXPOSURE = 10`), while print() re-emits valid JSON.
 */
static std::string jsonEscaped (const std::string &s)
{
	std::string out;
	for (std::string::const_iterator iter = s.begin (); iter != s.end (); iter++)
	{
		unsigned char c = (unsigned char) *iter;
		switch (c)
		{
			case '"':
				out += "\\\"";
				break;
			case '\\':
				out += "\\\\";
				break;
			case '\n':
				out += "\\n";
				break;
			case '\r':
				out += "\\r";
				break;
			case '\t':
				out += "\\t";
				break;
			default:
				if (c < 0x20)
				{
					char buf[8];
					snprintf (buf, sizeof (buf), "\\u%04x", c);
					out += buf;
				}
				else
				{
					out += (char) c;
				}
				break;
		}
	}
	return out;
}

void JsonValue::print (std::ostream &os, int indent) const
{
	std::string pad (indent, ' ');
	std::string padIn (indent + 2, ' ');
	switch (type)
	{
		case JSON_ARRAY:
			if (arr.empty ())
			{
				os << "[]";
				break;
			}
			os << "[" << std::endl;
			for (size_t i = 0; i < arr.size (); i++)
			{
				os << padIn;
				arr[i]->print (os, indent + 2);
				os << (i + 1 < arr.size () ? "," : "") << std::endl;
			}
			os << pad << "]";
			break;
		case JSON_OBJECT:
			if (obj.empty ())
			{
				os << "{}";
				break;
			}
			os << "{" << std::endl;
			for (size_t i = 0; i < obj.size (); i++)
			{
				os << padIn << "\"" << jsonEscaped (obj[i].first) << "\": ";
				obj[i].second->print (os, indent + 2);
				os << (i + 1 < obj.size () ? "," : "") << std::endl;
			}
			os << pad << "}";
			break;
		case JSON_STRING:
			os << "\"" << jsonEscaped (str) << "\"";
			break;
		default:
			os << text ();
			break;
	}
}
