#pragma once

#include <sys/stat.h>

#include <cstdint>
#include <fstream>
#include <string>

namespace duckdb {
namespace gdrive {

//! Credential files (KEY_FILE, the ADC file, a workload-identity token) are
//! never larger than this; anything bigger is not a credential.
constexpr std::uint64_t kMaxCredentialFileBytes = 1024 * 1024;

//! Read a credential file defensively. The path can come from SQL (KEY_FILE,
//! SET gdrive_adc_file), so it must not be able to hang a thread on a FIFO,
//! stream /dev/zero into memory, or read a directory. Only a regular file of at
//! most kMaxCredentialFileBytes is read. On failure returns false with a short,
//! content-free reason in `why` ("not found", "not a regular file", ...).
inline bool ReadCredentialFile(const std::string &path, std::string &out, std::string &why) {
	struct stat st;
	if (stat(path.c_str(), &st) != 0) {
		why = "not found or not readable";
		return false;
	}
	if ((st.st_mode & S_IFMT) != S_IFREG) {
		why = "not a regular file";
		return false;
	}
	if (static_cast<std::uint64_t>(st.st_size) > kMaxCredentialFileBytes) {
		why = "larger than 1 MiB, so not a credential file";
		return false;
	}
	std::ifstream f(path, std::ios::binary);
	if (!f.good()) {
		why = "not found or not readable";
		return false;
	}
	out.assign(static_cast<std::size_t>(st.st_size), '\0');
	f.read(&out[0], static_cast<std::streamsize>(out.size()));
	out.resize(static_cast<std::size_t>(f.gcount()));
	return true;
}

} // namespace gdrive
} // namespace duckdb
