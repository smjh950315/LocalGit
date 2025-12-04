#include <list>
#include <cyh/filesys.hpp>
#include "LocalGit.h"
namespace fs = std::filesystem;
namespace cfs = cyh::filesys;
using std::string;
using std::u8string;
using CommitCompareResult = cyh::extensions::LocalGit::CommitCompareResult;
using CommitInfo = cyh::extensions::LocalGit::CommitInfo;
using cyh::ref;
namespace cyh::extensions {
	namespace git_dbmodel {
		struct entry {
			// key
			int64 id{};
			u8string relative_path;
		};
		struct entry_history {
			// key
			int64 id{};
			int64 entry_id{};
			int64 is_directory{};
			int64 file_size{};
			int64 timestamp{};
		};
		struct commit {
			// key
			int64 id{};
			u8string message;
			u8string author;
			int64 timestamp{};
		};
		struct commit_snapshot {
			// key
			int64 id{};
			int64 commit_id{};
			int64 entry_id{};
			int64 history_id{};
		};
	};
	static std::unordered_map<fs::path, fs::path> GetCommitFileMaps(ref<odbc::connection> gitdb, const fs::path& gitpath, int64 _commitId)
	{
		auto sql_get_count = R"(
SELECT COUNT(*)
FROM commit_snapshot cs 
JOIN entry e ON cs.entry_id = e.id
JOIN entry_history eh ON cs.history_id = eh.id 
WHERE cs.commit_id = ?;
)";
		auto sql_get_pairs = R"(
SELECT e.relative_path, eh."timestamp" 
FROM commit_snapshot cs 
JOIN entry e ON cs.entry_id = e.id
JOIN entry_history eh ON cs.history_id = eh.id 
WHERE cs.commit_id = ?;
)";
		auto count = *gitdb->execute_for_result<int>(sql_get_count, _commitId).take_first();
		auto pairs = gitdb->execute_for_tuple_result<u8string, int64>(sql_get_pairs, _commitId).take(count);
		std::unordered_map<fs::path, fs::path> result;
		for (auto& t : pairs) {
			auto& rp = std::get<0>(t);
			auto p = cfs::new_path(gitpath, to_string(std::get<1>(t)), rp);
			result[rp] = p;
		}
		return result;
	}

	bool LocalGit::s_sqlite_odbc_initialized = false;
	bool LocalGit::InitSQLiteODBCDriver() 
	{
		if (!s_sqlite_odbc_initialized) {
			auto driverPath = cfs::new_path(fs::current_path(), "sqlite3odbcnw.dll");
			if (!(fs::exists(driverPath) && fs::is_regular_file(driverPath))) 
				return false;			
			if (!cyh::odbc::odbc_init("sqlite3odbcnw.dll")) 
				return false;			
			s_sqlite_odbc_initialized = true;
		}
		return s_sqlite_odbc_initialized;
	}

	LocalGit::LocalGit(const std::filesystem::path& _path) : m_path(_path)
	{
		if (!InitSQLiteODBCDriver()) {
			throw std::runtime_error("Failed to initialize SQLite ODBC Driver.");
		}
		m_gitdb = cyh::odbc::connection{};
		this->m_path = fs::absolute(_path);
		this->m_gitpath = this->m_path / GIT_DIR;
	}
	LocalGit::~LocalGit()
	{
	}
	bool LocalGit::IsValidRepo() const
	{
		if (fs::exists(this->m_gitpath) && fs::is_directory(this->m_gitpath)) {
			return fs::exists(this->m_gitpath / ".db") && fs::is_regular_file(this->m_gitpath / ".db");
		}
		return false;
	}
	void LocalGit::InitRepo()
	{
		auto dbPath = this->m_gitpath / ".db";
		if (!(fs::exists(dbPath) && fs::is_regular_file(dbPath))) {
			cfs::create_file_ex(dbPath, false, true);
		}
		this->m_gitdb = cyh::odbc::connection{};
		string connStr = "Driver=SQLite3 ODBC Driver;Database=";
		connStr += dbPath.string();
		connStr += ';';
		this->m_gitdb->connect(connStr.c_str());

		auto initSql = R"(
CREATE TABLE IF NOT EXISTS "entry" (
	id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
	relative_path TEXT NOT NULL
);
CREATE TABLE IF NOT EXISTS "entry_history" (
	id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
	entry_id INTEGER DEFAULT(0) NOT NULL,
	is_directory INTEGER DEFAULT(0) NOT NULL,
	file_size INTEGER DEFAULT(0) NOT NULL,
	"timestamp" INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS "commit" (
	id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
	message TEXT DEFAULT ('') NOT NULL,
	author TEXT DEFAULT ('anonymous') NOT NULL,	
	"timestamp" INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS "commit_snapshot" (
	id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,
	commit_id INTEGER DEFAULT(0) NOT NULL,
	entry_id INTEGER DEFAULT(0) NOT NULL,
	history_id INTEGER DEFAULT(0) NOT NULL
);)";
		this->m_gitdb->execute_directly(initSql);
	}

	std::vector<CommitInfo> LocalGit::GetCommitList(int begin, int count)
	{
		auto command = cyh::text::concat_by<char>(" ", R"(SELECT * FROM "commit" ORDER BY "commit".id DESC)", "LIMIT", to_string(count), "OFFSET", to_string(begin));
		auto dbData = this->m_gitdb->execute_for_model_result<cyh::extensions::git_dbmodel::commit>(command.c_str()).take(count);
		std::vector<CommitInfo> commits{};
		for (auto& data : dbData) {
			commits.push_back(
				CommitInfo{
				.id = data.id,
				.message = data.message,
				.author = data.author,
				.time = cyh::ctimestamp(data.timestamp).get_datetime(cyh::time::TIME_TYPE_LOCAL)
				});
		}
		return commits;
	}

	struct MetaCompareResult {
		u8string relative_path;
		uint64 action{};
		// int64 timestamp1;
		// int64 timestamp2;
	};

	struct ModFlagMapper {
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

	std::vector<CommitCompareResult> LocalGit::CompareCommits(int64 commitIdA, int64 commitIdB)
	{
		auto addFlag = ModFlagMapper{ .details = {1,1,0,0} }._placeholder;
		auto delFlag = ModFlagMapper{ .details = {0,1,1,0} }._placeholder;
		auto modFlag = ModFlagMapper{ .details = {0,1,0,0} }._placeholder;

		auto sql = R"(
SELECT 
e.relative_path, 
CASE 
	WHEN ccs1."timestamp" IS NULL THEN ?
	WHEN ccs2."timestamp" IS NULL THEN ?
	WHEN ccs1."timestamp" <> ccs2."timestamp" THEN ?
	ELSE 0
END AS "action",
ccs1."timestamp" ,ccs2."timestamp" 
FROM entry e 
LEFT JOIN 
(
	SELECT cs1.entry_id, eh1."timestamp" 
	FROM commit_snapshot cs1
	JOIN entry_history eh1 ON cs1.history_id = eh1.id 
	WHERE cs1.commit_id = ?
) AS ccs1
ON e.id = ccs1.entry_id 
LEFT JOIN 
(
	SELECT cs2.entry_id, eh2."timestamp" 
	FROM commit_snapshot cs2
	JOIN entry_history eh2 ON cs2.history_id = eh2.id 
	WHERE cs2.commit_id = ?
) AS ccs2
ON e.id = ccs2.entry_id 
WHERE ccs1."timestamp" IS NOT NULL OR ccs2."timestamp" IS NOT NULL)";

		auto sql_count = string("SELECT COUNT(*) FROM (") + sql + ") WHERE action <> 0;";
		auto sql_fetch = string("SELECT * FROM (") + sql + ") WHERE action <> 0;";

		auto count = *this->m_gitdb->execute_for_result<int>(sql_count.c_str(), addFlag, delFlag, modFlag, commitIdA, commitIdB).take_first();
		auto res = this->m_gitdb->execute_for_model_result<MetaCompareResult>(sql_fetch.c_str(), addFlag, delFlag, modFlag, commitIdA, commitIdB).take(count);

		std::vector<CommitCompareResult> results;
		results.reserve(res.size());
		for(auto& r : res) {
			CommitCompareResult ccr;
			ccr.file_path = std::move(r.relative_path);
			ccr._placeholder = r.action;
			results.push_back(ccr);
		}
		return results;
	}
	std::vector<u8string> LocalGit::RestoreFiles(int64 commitID) const
	{
		auto fileMaps = GetCommitFileMaps(this->m_gitdb, this->m_gitpath, commitID);
		std::list<fs::path> ents;
		cfs::get_subentries_path(this->m_path, ents, true, true,
								 [&](const fs::directory_entry& p) {
									 return !p.path().native().starts_with(this->m_gitpath.native());
								 });
		std::vector<u8string> existedFiles;
		for (auto& p : ents) {
			auto m = fileMaps.find(p);
			auto resPath = cfs::new_path(this->m_path, p);
			if (m == fileMaps.end()) {
				// not found in commit, delete it
				cfs::remove_entry(resPath);
			} else {
				// found, restore it
				auto bakpath = fs::path(m->second);				
				if (fs::exists(bakpath)) {
					if (fs::is_directory(bakpath)) {
						cfs::create_directory_ex(p, true, true);
					} else if (fs::is_regular_file(bakpath)) {
						auto cmp = cfs::entry_compare_result::compare(bakpath, resPath);
						if (cmp.details.is_modified) {
							// modified, copy
							cfs::copy_file_ex(bakpath, resPath, true, true);
						} else {
							// not modified, do nothing
						}
					}
					existedFiles.push_back(resPath.u8string());
				} else {
					// backup file missing, skip
				}
				fileMaps.erase(m->first);
			}
		}
		for (auto& m : fileMaps) {
			// not existed, restore it
			auto bakpath = fs::path(m.second);
			if (fs::exists(bakpath)) {
				if (fs::is_directory(bakpath)) {
					cfs::create_directory_ex(cfs::new_path(this->m_path, m.first), true, true);
				} else if (fs::is_regular_file(bakpath)) {
					cfs::copy_file_ex(bakpath, cfs::new_path(this->m_path, m.first), true, true);
				}
				existedFiles.push_back(m.first.u8string());
			} else {
				// backup file missing, skip
			}
		}
		return existedFiles;
	}
	std::vector<CommitCompareResult> LocalGit::CompareCommit(int64 commitID) const
	{
		auto fileMaps = GetCommitFileMaps(this->m_gitdb, this->m_gitpath, commitID);
		std::list<fs::path> ents;
		cfs::get_subentries_path(this->m_path, ents, true, true,
								 [&](const fs::directory_entry& p) {
									 return !p.path().native().starts_with(this->m_gitpath.native());
								 });
		std::vector<CommitCompareResult> diffRecords;
		for (auto& p : ents) {
			auto m = fileMaps.find(p);
			if (m == fileMaps.end()) {
				diffRecords.push_back(CommitCompareResult
					{
					.file_path = p.u8string(),
					.details = { .is_created = 1, .is_modified = 1 }
					}
				);
			} else {
				// found, restore it
				auto bakpath = fs::path(m->second);
				auto resPath = cfs::new_path(this->m_path, p);
				auto cmp = cfs::entry_compare_result::compare(bakpath, resPath);
				if (cmp.details.is_modified) {
					// modified
					diffRecords.push_back(CommitCompareResult
						{
						.file_path = p.u8string(),
						.details = { .is_modified = 1 }
						}
					);
				} else {
					// not modified, do nothing
				}
				fileMaps.erase(m->first);
			}
		}
		for (auto& m : fileMaps) {
			diffRecords.push_back(CommitCompareResult
				{
				.file_path = m.first.u8string(),
				.details = { .is_deleted = 1, .is_modified = 1 }
				}
			);
		}
		return diffRecords;
	}
	struct file_index {
		std::string relative_path;
	};
	struct inserting_entry_history {
		int64 entry_id{};
		int64 is_directory{};
		int64 file_size{};
		int64 timestamp{};
	};
	struct inserting_commit_snapshot {
		int64 commit_id{};
		int64 entry_id{};
		int64 history_id{};
	};
	int64 LocalGit::BackupFiles(const u8string& message, const u8string& author)
	{
		auto currentTimestamp = cyh::ctimestamp::now().timestamp;
		auto currentTimestamps = to_string(currentTimestamp);
		auto idptr = this->m_gitdb->execute_for_result<int64>(R"(INSERT INTO "commit"(message, author, timestamp) VALUES(?,?,?) RETURNING id;)",
															  message, author, currentTimestamp).take_first();
		if (!idptr)
			return 0;
		int64 id = *idptr;
		if (!id)
			return 0;

		std::list<fs::path> ents;
		cfs::get_subentries_path(this->m_path, ents, true, false,
								 [&](const fs::directory_entry& p) {
									 return !p.path().native().starts_with(this->m_gitpath.native());
								 });
		auto sql_find_last_entry_id_ts = R"(
SELECT e.id as eid, eh.id as ehid, eh."timestamp" 
FROM commit_snapshot cs 
JOIN entry e ON cs.entry_id = e.id 
JOIN entry_history eh ON cs.history_id = eh.id
WHERE e.relative_path = ?
ORDER BY eh."timestamp" DESC
LIMIT 1;)";
		auto sql_insert_file_index_get_id = R"(
INSERT INTO entry(relative_path) VALUES(?) RETURNING id;
)";

		auto tryGetLastRecord = [&](const fs::path& relpath, int64 isdir, int64& eid, int64& ehid, int64& ts)
			{
				auto pair = this->m_gitdb->execute_for_tuple_result<int64, int64, int64>(sql_find_last_entry_id_ts, relpath.u8string(), isdir).take_first();
				if (pair) {
					auto& r = *pair;
					eid = std::get<0>(r);
					ehid = std::get<1>(r);
					ts = std::get<2>(r);
					return true;
				} else {
					return false;
				}				
			};
		auto tryInsertEntry = [&](const fs::path& relpath, int64& eid)
			{
				auto newid = this->m_gitdb->execute_for_result<int64>(sql_insert_file_index_get_id, relpath.u8string()).take_first();
				if (newid) {
					eid = *newid;
					return true;
				} else {
					return false;
				}
			};
		auto tryInsertEntryHistory = [&](const inserting_entry_history& eh, int64& ehid)
			{
				auto newid = this->m_gitdb->execute_for_result<int64>(
					R"(INSERT INTO entry_history(entry_id, is_directory, file_size, "timestamp") VALUES(?,?,?,?) RETURNING id;)",
					eh.entry_id, eh.is_directory, eh.file_size, eh.timestamp).take_first();
				if (newid) {
					ehid = *newid;
					return true;
				} else {
					return false;
				}
			};

		bool newdir_created = false;
		fs::path newdir_path = cfs::new_path(this->m_gitpath, currentTimestamps);
		for (auto& p : ents) {
			auto rps = fs::relative(p, this->m_path);
			auto isdir = fs::is_directory(p) ? 1 : 0;
			auto fsize = !isdir ? fs::file_size(p) : 0;
			int64 eid, ehid;
			inserting_entry_history eh{
				.entry_id = 0,
				.is_directory = isdir,
				.file_size = (int64)fsize,
			};
			bool should_clone = false;
			if (tryGetLastRecord(rps, isdir, eid, ehid, eh.timestamp)) {
				eh.entry_id = eid;
				auto bakpath = cfs::new_path(this->m_gitpath, to_string(eh.timestamp), rps);
				auto cmp = cfs::entry_compare_result::compare(bakpath, p);
				if (cmp.details.is_modified) {
					eh.timestamp = currentTimestamp;
					if (!tryInsertEntryHistory(eh, ehid)) {
						console::println("create file history failed: ", rps.u8string());
						continue;
					}
					should_clone = true;
				} else {
					// no change
				}
			} else {
				if (tryInsertEntry(rps, eid)) {
					eh.entry_id = eid;
					eh.timestamp = currentTimestamp;
					if (!tryInsertEntryHistory(eh, ehid)) {
						console::println("create file history failed: ", rps.u8string());
						continue;
					}
					should_clone = true;
				} else {
					console::println("create file index failed: ", rps.u8string());
					continue;
				}
			}
			inserting_commit_snapshot cs{
				.commit_id = id,
				.entry_id = eid,
				.history_id = ehid,
			};
			this->m_gitdb->execute_directly(
				R"(INSERT INTO commit_snapshot(commit_id, entry_id, history_id) VALUES(?,?,?);)",
				cs.commit_id, cs.entry_id, cs.history_id);
			if (should_clone) {
				if (!newdir_created) {
					cfs::create_directory_ex(newdir_path, true, true);
					newdir_created = true;
				}
				if (isdir) {
					cfs::create_directory_ex(cfs::new_path(newdir_path, rps), true, true);
				} else {
					auto destpath = cfs::new_path(newdir_path, rps);
					cfs::copy_file_ex(p, destpath, true, true);
				}
			}
		}
		return id;
	}
};
