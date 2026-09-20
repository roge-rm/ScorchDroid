#ifndef SCORCHDROID_JSON_HPP
#define SCORCHDROID_JSON_HPP

#include <string>
#include <vector>

// A JSON *writer*, and deliberately nothing more.
//
// The control channel is asymmetric on purpose: requests are one
// tab-separated line, which needs no parser at all, and only replies are
// JSON, so the server never has to read any. That keeps this to string
// escaping plus two small builders, and means no JSON library and no new
// submodule for a project whose whole dependency diet is the point.
//
// The escaping validates UTF-8 as it goes and replaces anything malformed
// with U+FFFD. Player names arrive from the network and are not trustworthy;
// a single stray byte would otherwise make the reply undecodable and take
// the whole admin page down with it.
namespace Json
{
	inline void appendCodepoint(std::string &out, unsigned int cp)
	{
		static const char *hex = "0123456789abcdef";
		out += "\\u";
		out += hex[(cp >> 12) & 0xF];
		out += hex[(cp >> 8) & 0xF];
		out += hex[(cp >> 4) & 0xF];
		out += hex[cp & 0xF];
	}

	inline std::string str(const std::string &value)
	{
		std::string out;
		out.reserve(value.size() + 2);
		out += '"';
		for (std::size_t i = 0; i < value.size();)
		{
			const unsigned char c = (unsigned char) value[i];
			if (c == '"' || c == '\\')
			{
				out += '\\';
				out += (char) c;
				i++;
			}
			else if (c == '\n') { out += "\\n"; i++; }
			else if (c == '\r') { out += "\\r"; i++; }
			else if (c == '\t') { out += "\\t"; i++; }
			else if (c < 0x20)
			{
				appendCodepoint(out, c);
				i++;
			}
			else if (c < 0x80)
			{
				out += (char) c;
				i++;
			}
			else
			{
				// A multi-byte sequence: copy it through only if it really
				// is one, so the reply stays decodable whatever a player
				// called themselves.
				std::size_t length = 0;
				if ((c & 0xE0) == 0xC0) length = 2;
				else if ((c & 0xF0) == 0xE0) length = 3;
				else if ((c & 0xF8) == 0xF0) length = 4;

				bool valid = length > 0 && i + length <= value.size();
				for (std::size_t j = 1; valid && j < length; j++)
				{
					if (((unsigned char) value[i + j] & 0xC0) != 0x80) valid = false;
				}

				if (valid)
				{
					out.append(value, i, length);
					i += length;
				}
				else
				{
					out += "\xEF\xBF\xBD";  // U+FFFD REPLACEMENT CHARACTER
					i++;
				}
			}
		}
		out += '"';
		return out;
	}

	inline std::string str(const char *value) { return str(std::string(value ? value : "")); }

	inline std::string num(long long value) { return std::to_string(value); }

	inline std::string num(int value) { return std::to_string(value); }

	inline std::string num(unsigned long long value) { return std::to_string(value); }

	inline std::string boolean(bool value) { return value ? "true" : "false"; }

	// Both builders take values that are already JSON, which is what makes
	// them compose: an Object's value can be an Array's text and vice versa.
	class Object
	{
	public:
		Object &raw(const std::string &key, const std::string &jsonValue)
		{
			if (!body_.empty()) body_ += ',';
			body_ += str(key);
			body_ += ':';
			body_ += jsonValue;
			return *this;
		}

		Object &set(const std::string &key, const std::string &value) { return raw(key, str(value)); }
		Object &set(const std::string &key, const char *value) { return raw(key, str(value)); }
		Object &set(const std::string &key, long long value) { return raw(key, num(value)); }
		Object &set(const std::string &key, int value) { return raw(key, num(value)); }
		Object &set(const std::string &key, unsigned int value) { return raw(key, num((long long) value)); }
		Object &set(const std::string &key, unsigned long long value) { return raw(key, num(value)); }
		Object &set(const std::string &key, bool value) { return raw(key, boolean(value)); }

		std::string text() const { return "{" + body_ + "}"; }

	private:
		std::string body_;
	};

	class Array
	{
	public:
		Array &raw(const std::string &jsonValue)
		{
			if (!body_.empty()) body_ += ',';
			body_ += jsonValue;
			return *this;
		}

		Array &add(const std::string &value) { return raw(str(value)); }
		Array &add(const char *value) { return raw(str(value)); }
		Array &add(long long value) { return raw(num(value)); }
		Array &add(int value) { return raw(num(value)); }

		std::string text() const { return "[" + body_ + "]"; }

	private:
		std::string body_;
	};

	inline std::string ok(const std::string &jsonObjectBody)
	{
		return jsonObjectBody;
	}

	inline std::string error(const std::string &message)
	{
		return Object().set("ok", false).set("error", message).text();
	}
}  // namespace Json

#endif  // SCORCHDROID_JSON_HPP
