// S-2.18 -- glob pattern matching over Drive file names. PURE: no
// "duckdb.hpp", no I/O. See src/include/gdrive_glob.hpp for the contract.
#include "gdrive_glob.hpp"

namespace duckdb {
namespace gdrive {

namespace {

// ---------------------------------------------------------------------------
// Character class matching: "[abc]", "[a-z]", "[!abc]"/"[^abc]", with the
// classic glob carve-outs for a literal ']' as the first member and a
// literal '-' at either end of the class.
//
// Returns whether the class starting at pattern[start] (the character right
// after '[') is well-formed; on success `end` is set to the index one past
// the closing ']', and `matched` reports whether `c` is a class member.
// On failure (no closing ']' found) the class is not well-formed and the
// caller falls back to treating '[' as a literal character -- see
// MatchSegment's decision note below.
// ---------------------------------------------------------------------------
bool TryMatchClass(const std::string &pattern, size_t start, char c, size_t &end, bool &matched) {
	size_t i = start;
	const size_t n = pattern.size();
	bool negate = false;
	if (i < n && (pattern[i] == '!' || pattern[i] == '^')) {
		negate = true;
		i++;
	}
	bool found = false;
	bool first = true;
	while (true) {
		if (i >= n) {
			// unterminated class -- no closing ']'.
			return false;
		}
		if (pattern[i] == ']' && !first) {
			// closing bracket.
			end = i + 1;
			matched = negate ? !found : found;
			return true;
		}
		first = false;
		// a range like "a-z", but not when '-' is the last class member
		// (immediately followed by ']') or when it starts the class body.
		if (i + 2 < n && pattern[i + 1] == '-' && pattern[i + 2] != ']') {
			char lo = pattern[i];
			char hi = pattern[i + 2];
			if (c >= lo && c <= hi) {
				found = true;
			}
			i += 3;
		} else {
			if (pattern[i] == c) {
				found = true;
			}
			i++;
		}
	}
}

// ---------------------------------------------------------------------------
// Two-pointer backtracking matcher, the same shape as DuckDB's own
// scalar `Glob` (src/function/scalar/string/like.cpp): O(pattern * name)
// worst case, never exponential, because a failed match only ever rewinds
// to the most recent '*' and advances its start position by one -- it does
// not re-explore already-failed suffixes.
// ---------------------------------------------------------------------------
bool MatchSegmentImpl(const std::string &pattern, const std::string &name) {
	const size_t plen = pattern.size();
	const size_t slen = name.size();
	size_t pidx = 0, sidx = 0;
	size_t star_pidx = std::string::npos, star_sidx = 0;

	while (sidx < slen) {
		bool matched = false;
		size_t next_pidx = pidx;

		if (pidx < plen) {
			char p = pattern[pidx];
			if (p == '*') {
				pidx++;
				while (pidx < plen && pattern[pidx] == '*') {
					pidx++;
				}
				if (pidx == plen) {
					return true; // trailing '*' matches the remainder unconditionally
				}
				star_pidx = pidx;
				star_sidx = sidx;
				continue;
			} else if (p == '?') {
				matched = true; // matches exactly one byte, whatever it is
				next_pidx = pidx + 1;
			} else if (p == '[') {
				size_t class_end;
				bool class_matched;
				if (TryMatchClass(pattern, pidx + 1, name[sidx], class_end, class_matched)) {
					matched = class_matched;
					next_pidx = class_end;
				} else {
					// Decision: an unclosed '[' is not an error. It degrades
					// to matching itself as a literal character. A thrown
					// exception here would abort an entire folder scan
					// because of one oddly-named file; staying total is
					// worth more than being strict.
					matched = (name[sidx] == '[');
					next_pidx = pidx + 1;
				}
			} else {
				matched = (name[sidx] == p);
				next_pidx = pidx + 1;
			}
		}

		if (matched) {
			sidx++;
			pidx = next_pidx;
			continue;
		}

		if (star_pidx == std::string::npos) {
			return false;
		}
		// backtrack: the last '*' consumes one more character and we retry.
		star_sidx++;
		sidx = star_sidx;
		pidx = star_pidx;
	}

	// consume any trailing '*'s -- they match the empty remainder.
	while (pidx < plen && pattern[pidx] == '*') {
		pidx++;
	}
	return pidx == plen;
}

std::vector<std::string> SplitOnSlash(const std::string &s) {
	std::vector<std::string> segments;
	size_t start = 0;
	while (true) {
		auto pos = s.find('/', start);
		if (pos == std::string::npos) {
			segments.push_back(s.substr(start));
			break;
		}
		segments.push_back(s.substr(start, pos - start));
		start = pos + 1;
	}
	return segments;
}

// ---------------------------------------------------------------------------
// MatchPath, memoized over (pattern segment index, path segment index) so
// that even a pattern packed with adjacent '**' groups stays O(P*N) instead
// of exploring the same (i, j) state repeatedly.
// ---------------------------------------------------------------------------
bool MatchSegments(const std::vector<std::string> &pattern_segments, const std::vector<std::string> &path_segments,
                    size_t pi, size_t si, std::vector<std::vector<int8_t>> &memo) {
	if (pi == pattern_segments.size()) {
		return si == path_segments.size();
	}
	int8_t &cached = memo[pi][si];
	if (cached != -1) {
		return cached != 0;
	}
	bool result;
	if (pattern_segments[pi] == "**") {
		// zero segments, or consume one more and stay on the same '**'.
		result = MatchSegments(pattern_segments, path_segments, pi + 1, si, memo) ||
		         (si < path_segments.size() && MatchSegments(pattern_segments, path_segments, pi, si + 1, memo));
	} else if (si == path_segments.size()) {
		result = false;
	} else {
		result = MatchSegmentImpl(pattern_segments[pi], path_segments[si]) &&
		         MatchSegments(pattern_segments, path_segments, pi + 1, si + 1, memo);
	}
	cached = result ? 1 : 0;
	return result;
}

} // namespace

bool MatchSegment(const std::string &pattern, const std::string &name) {
	return MatchSegmentImpl(pattern, name);
}

bool MatchPath(const std::string &pattern, const std::string &path) {
	auto pattern_segments = SplitOnSlash(pattern);
	auto path_segments = SplitOnSlash(path);
	std::vector<std::vector<int8_t>> memo(pattern_segments.size() + 1,
	                                       std::vector<int8_t>(path_segments.size() + 1, -1));
	return MatchSegments(pattern_segments, path_segments, 0, 0, memo);
}

bool HasGlobMetacharacters(const std::string &pattern) {
	for (char c : pattern) {
		if (c == '*' || c == '?' || c == '[' || c == '{') {
			return true;
		}
	}
	return false;
}

GlobSplit SplitGlob(const std::string &pattern) {
	GlobSplit result;
	if (!HasGlobMetacharacters(pattern)) {
		result.literal_prefix = pattern;
		result.pattern_tail = "";
		result.recursive = false;
		return result;
	}

	auto segments = SplitOnSlash(pattern);
	size_t first_glob_segment = segments.size();
	for (size_t i = 0; i < segments.size(); i++) {
		if (HasGlobMetacharacters(segments[i])) {
			first_glob_segment = i;
			break;
		}
	}

	std::string prefix;
	for (size_t i = 0; i < first_glob_segment; i++) {
		if (i > 0) {
			prefix += "/";
		}
		prefix += segments[i];
	}

	std::string tail;
	for (size_t i = first_glob_segment; i < segments.size(); i++) {
		if (i > first_glob_segment) {
			tail += "/";
		}
		tail += segments[i];
	}

	result.literal_prefix = prefix;
	result.pattern_tail = tail;
	// Multiple remaining segments, or an explicit '**' anywhere in the tail,
	// force a recursive listing rather than a single-folder one.
	result.recursive = (segments.size() - first_glob_segment > 1) || tail.find("**") != std::string::npos;
	return result;
}

std::vector<std::string> ExpandBraces(const std::string &pattern) {
	// Find the first top-level (unnested) balanced brace group.
	int depth = 0;
	size_t brace_start = std::string::npos;
	size_t brace_end = std::string::npos;
	for (size_t i = 0; i < pattern.size(); i++) {
		if (pattern[i] == '{') {
			if (depth == 0) {
				brace_start = i;
			}
			depth++;
		} else if (pattern[i] == '}') {
			if (depth > 0) {
				depth--;
				if (depth == 0 && brace_start != std::string::npos) {
					brace_end = i;
					break;
				}
			}
		}
	}

	if (brace_start == std::string::npos || brace_end == std::string::npos) {
		// No top-level brace group found (including an unmatched '{' or a
		// stray '}'): left as a literal, no expansion performed.
		return {pattern};
	}

	std::string prefix = pattern.substr(0, brace_start);
	std::string inner = pattern.substr(brace_start + 1, brace_end - brace_start - 1);
	std::string suffix = pattern.substr(brace_end + 1);

	// Split `inner` on top-level commas (commas inside a nested brace group
	// do not separate alternatives at this level).
	std::vector<std::string> alternatives;
	int inner_depth = 0;
	size_t start = 0;
	for (size_t i = 0; i < inner.size(); i++) {
		if (inner[i] == '{') {
			inner_depth++;
		} else if (inner[i] == '}') {
			inner_depth--;
		} else if (inner[i] == ',' && inner_depth == 0) {
			alternatives.push_back(inner.substr(start, i - start));
			start = i + 1;
		}
	}
	alternatives.push_back(inner.substr(start));

	auto suffix_expansions = ExpandBraces(suffix);

	std::vector<std::string> result;
	for (auto &alt : alternatives) {
		for (auto &alt_expanded : ExpandBraces(alt)) {
			for (auto &suffix_expanded : suffix_expansions) {
				result.push_back(prefix + alt_expanded + suffix_expanded);
			}
		}
	}
	return result;
}

} // namespace gdrive
} // namespace duckdb
