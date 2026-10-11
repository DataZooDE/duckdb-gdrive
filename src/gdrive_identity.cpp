// See gdrive_identity.hpp. PURE.
#include "gdrive_identity.hpp"

namespace duckdb {
namespace gdrive {

namespace {

std::string Lower(std::string s) {
	for (auto &c : s) {
		if (c >= 'A' && c <= 'Z') {
			c = static_cast<char>(c - 'A' + 'a');
		}
	}
	return s;
}

} // namespace

std::vector<std::string> IdentityMaterial(const std::string &provider,
                                          const std::vector<std::pair<std::string, std::string>> &secret_fields,
                                          const std::string &identity_source) {
	const bool rewrites_tokens = provider == "authorization_code";
	std::vector<std::string> material;
	material.reserve(secret_fields.size() + 1);
	for (const auto &field : secret_fields) {
		const std::string key = Lower(field.first);
		if (rewrites_tokens &&
		    (key == "access_token" || key == "token_expiry" || key.compare(0, 7, "expires") == 0)) {
			continue;
		}
		material.push_back(key + "=" + field.second);
	}
	material.push_back("source=" + identity_source);
	return material;
}

} // namespace gdrive
} // namespace duckdb
