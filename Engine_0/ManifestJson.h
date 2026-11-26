#pragma once

#include <cstdio>
#include <string>
#include <string_view>

namespace dEngine::detail {

	// 仅用于清单输出：转义 JSON 字符串所需的引号、反斜杠与控制字符。
	inline void AppendJsonString(std::string& out, std::string_view value) {
		out.push_back('"');
		for (const unsigned char ch : value) {
			switch (ch) {
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\b': out += "\\b"; break;
			case '\f': out += "\\f"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20) {
					char escaped[7] = {};
					std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
					out += escaped;
				}
				else {
					out.push_back(static_cast<char>(ch));
				}
				break;
			}
		}
		out.push_back('"');
	}
}
