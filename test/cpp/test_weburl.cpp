// Browser addresses as gdrive:// paths. Pure logic, no DuckDB linkage.
//
// The addresses are the shapes Drive and the editors actually show (address
// bar, "Copy link", "Share"), with real-looking ids.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "gdrive_weburl.hpp"

using namespace duckdb::gdrive;

namespace {
const std::string kId = "18Yue0IodacKje3gEl632Pkaw7nc_ZVSX";
const std::string kDocId = "1pNWvcXErPx4e6WjDLZi6M3Me4emePxKoq4K2huKPfqM";
} // namespace

TEST_CASE("IsDriveWebUrl: only Drive's own hosts", "[weburl]") {
	CHECK(IsDriveWebUrl("https://drive.google.com/file/d/" + kId + "/view"));
	CHECK(IsDriveWebUrl("https://docs.google.com/spreadsheets/d/" + kDocId + "/edit"));
	CHECK(IsDriveWebUrl("http://drive.google.com/open?id=" + kId));
	CHECK(IsDriveWebUrl("https://drive.usercontent.google.com/download?id=" + kId));
	CHECK_FALSE(IsDriveWebUrl("gdrive://id:" + kId));
	CHECK_FALSE(IsDriveWebUrl("https://storage.googleapis.com/bucket/x.parquet"));
	CHECK_FALSE(IsDriveWebUrl("https://drive.google.com.evil.example/file/d/x/view"));
	CHECK_FALSE(IsDriveWebUrl("https://example.com/?u=https://drive.google.com/file/d/x"));
	CHECK_FALSE(IsDriveWebUrl("s3://drive.google.com/x"));
}

TEST_CASE("ParseDriveWebUrl: file addresses", "[weburl]") {
	for (auto url : {"https://drive.google.com/file/d/" + kId + "/view?usp=sharing",
	                 "https://drive.google.com/file/d/" + kId + "/view?usp=drivesdk",
	                 "https://drive.google.com/file/d/" + kId,
	                 "https://drive.google.com/file/d/" + kId + "/edit#heading=h.1",
	                 "https://drive.google.com/open?id=" + kId,
	                 "https://drive.google.com/open?usp=x&id=" + kId,
	                 "https://drive.google.com/uc?id=" + kId + "&export=download",
	                 "https://drive.usercontent.google.com/download?id=" + kId + "&export=download",
	                 "https://drive.google.com/a/example.com/file/d/" + kId + "/view",
	                 "https://drive.google.com/u/1/file/d/" + kId + "/view"}) {
		auto p = ParseDriveWebUrl(url);
		INFO(url);
		REQUIRE(p.ok);
		CHECK(p.id == kId);
		CHECK_FALSE(p.is_folder);
		CHECK_FALSE(p.is_editor);
	}
}

TEST_CASE("ParseDriveWebUrl: editor addresses are native files", "[weburl]") {
	for (auto url : {"https://docs.google.com/spreadsheets/d/" + kDocId + "/edit#gid=0",
	                 "https://docs.google.com/spreadsheets/u/0/d/" + kDocId + "/edit",
	                 "https://docs.google.com/document/d/" + kDocId + "/edit?usp=drivesdk",
	                 "https://docs.google.com/presentation/d/" + kDocId + "/edit",
	                 "https://docs.google.com/a/example.com/document/d/" + kDocId + "/edit"}) {
		auto p = ParseDriveWebUrl(url);
		INFO(url);
		REQUIRE(p.ok);
		CHECK(p.id == kDocId);
		CHECK(p.is_editor);
	}
	// docs.google.com/file/d/ is the old viewer for UPLOADED files.
	auto f = ParseDriveWebUrl("https://docs.google.com/file/d/" + kId + "/edit");
	REQUIRE(f.ok);
	CHECK_FALSE(f.is_editor);
}

TEST_CASE("ParseDriveWebUrl: folder addresses", "[weburl]") {
	for (auto url : {"https://drive.google.com/drive/folders/" + kId,
	                 "https://drive.google.com/drive/folders/" + kId + "?usp=sharing",
	                 "https://drive.google.com/drive/u/2/folders/" + kId,
	                 "https://drive.google.com/drive/mobile/folders/" + kId}) {
		auto p = ParseDriveWebUrl(url);
		INFO(url);
		REQUIRE(p.ok);
		CHECK(p.id == kId);
		CHECK(p.is_folder);
	}
}

TEST_CASE("ParseDriveWebUrl: a Drive address that names nothing is an error", "[weburl]") {
	for (auto url : std::initializer_list<std::string> {
	         "https://drive.google.com/", "https://drive.google.com/drive/my-drive",
	         "https://drive.google.com/drive/shared-with-me", "https://drive.google.com/open?usp=x",
	         "https://drive.google.com/file/d//view", "https://docs.google.com/forms/d/e/1FAIpQLSf/viewform",
	         "https://drive.google.com/file/d/abc%2F../view"}) {
		auto p = ParseDriveWebUrl(url);
		INFO(url);
		CHECK_FALSE(p.ok);
		CHECK(p.error.find("names no file or folder") != std::string::npos);
	}
}

TEST_CASE("WebUrlToGDrivePath: addresses map to the id: form, anything else passes", "[weburl]") {
	std::string out, error;
	REQUIRE(WebUrlToGDrivePath("https://drive.google.com/file/d/" + kId + "/view", out, error));
	CHECK(out == "gdrive://id:" + kId);
	REQUIRE(WebUrlToGDrivePath("gdrive://Finance/a.csv", out, error));
	CHECK(out == "gdrive://Finance/a.csv");
	CHECK_FALSE(WebUrlToGDrivePath("https://drive.google.com/drive/my-drive", out, error));
	CHECK_FALSE(error.empty());
}

TEST_CASE("ParseDriveWebUrl: only /file/d/<id>/<more> is writable", "[weburl]") {
	// COPY stages at "tmp_" + the last segment; only here is that not the id.
	CHECK(ParseDriveWebUrl("https://drive.google.com/file/d/" + kId + "/view").writable);
	CHECK(ParseDriveWebUrl("https://drive.google.com/file/d/" + kId + "/view?usp=sharing").writable);
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/file/d/" + kId).writable);
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/open?id=" + kId).writable);
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/drive/folders/" + kId).writable);
	CHECK_FALSE(ParseDriveWebUrl("https://docs.google.com/spreadsheets/d/" + kDocId + "/edit").writable);
	// What COPY makes of the non-writable forms: another id, or no address.
	CHECK(ParseDriveWebUrl("https://drive.google.com/file/d/tmp_" + kId).id == "tmp_" + kId);
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/tmp_open?id=" + kId).ok);
}

TEST_CASE("ParseDriveWebUrl: a version in the query is carried over", "[weburl]") {
	auto p = ParseDriveWebUrl("https://drive.google.com/file/d/" + kId + "/view?usp=sharing&version=0B9fS7-rev");
	REQUIRE(p.ok);
	CHECK(p.version == "0B9fS7-rev");
	std::string out, error;
	REQUIRE(WebUrlToGDrivePath("https://drive.google.com/file/d/" + kId + "/view?version=0B9fS7-rev", out, error));
	CHECK(out == "gdrive://id:" + kId + "?version=0B9fS7-rev");
	REQUIRE(WebUrlToGDrivePath("https://drive.google.com/open?id=" + kId + "&version=42", out, error));
	CHECK(out == "gdrive://id:" + kId + "?version=42");
}

TEST_CASE("ParseDriveWebUrl: a folder address is exactly the folder", "[weburl]") {
	// "/folders/<id>/*.csv" used to read the whole folder, dropping "/*.csv".
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/drive/folders/" + kId + "/*.csv").ok);
	CHECK_FALSE(ParseDriveWebUrl("https://drive.google.com/drive/folders/" + kId + "/sub/x.csv").ok);
	CHECK(ParseDriveWebUrl("https://drive.google.com/drive/folders/" + kId + "/").ok); // a trailing slash
}

TEST_CASE("IsDriveWebUrl: hosts are case-insensitive", "[weburl]") {
	CHECK(IsDriveWebUrl("https://DRIVE.GOOGLE.COM/file/d/" + kId + "/view"));
	auto p = ParseDriveWebUrl("https://Docs.Google.Com/spreadsheets/d/" + kDocId + "/edit");
	REQUIRE(p.ok);
	CHECK(p.is_editor);
}
