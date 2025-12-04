#pragma once
#include <iostream>
#include <vector>
#include <filesystem>
#include <cyh/odbc.hpp>
#include <cyh/reference.hpp>
namespace cyh::extensions {
	class LocalGit {
	public:
		using string = std::string;
		using u8string = std::u8string;
	private:
		static constexpr const char* GIT_DIR = ".localgit";
		static bool s_sqlite_odbc_initialized;
		std::filesystem::path m_path;
		std::filesystem::path m_gitpath;
		cyh::ref<cyh::odbc::connection> m_gitdb;
	public:
		struct CommitInfo {
			int64 id;
			u8string message;
			u8string author;
			cyh::datetime time;
		};
		struct CommitCompareResult {
			u8string file_path;
			union {
				uint64 _placeholder{};
				struct {
					char is_created;
					char is_deleted;
					char is_modified;
					char reserved;
				} details;
			};
		};
		static bool InitSQLiteODBCDriver();
		LocalGit(const std::filesystem::path& _path);
		~LocalGit();
		bool IsValidRepo() const;
		void InitRepo() ;
		std::vector<CommitInfo> GetCommitList(int begin = 0, int count = 10);
		std::vector<CommitCompareResult> CompareCommits(int64 commitIdA, int64 commitIdB);
		std::vector<CommitCompareResult> CompareCommit(int64 commitID) const;
		std::vector<u8string> RestoreFiles(int64 commitID) const;
		int64 BackupFiles(const u8string& message, const u8string& author = {});
	};
};