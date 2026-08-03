#include "gdrive_version.hpp"

namespace duckdb {
namespace gdrive {

std::string GdriveVersion() {
	// CalVer, vYYYY.MM.DD, matching the tag and the sibling DataZoo extensions
	// (erpl, anofox-*, quack_oauth). Kept in step with the tag by
	// scripts/check_extension_stamp.sh, which fails the build if HEAD is on a
	// tag and this string is not it -- it used to be possible for the two to
	// drift silently, and nothing would have noticed.
	return "2026.08.03";
}

} // namespace gdrive
} // namespace duckdb
