#include <stdlib.h>
#include <cyh/filesys.hpp>
#include <cyh/container.hpp>
#include <cyh/text.hpp>
#include <cyh/odbc.hpp>
#include <cyh/time.hpp>
#include <cyh/console.hpp>
#include <cyh/gui.hpp>
#ifdef _DEBUG
#pragma comment (lib, "Cyhd.lib")
#pragma comment (lib, "CyhODBCd.lib")
#pragma comment (lib, "CyhGuid.lib")
#elif defined(_STATIC)
#pragma comment (lib, "Cyh_mt.lib")
#pragma comment (lib, "CyhODBC_mt.lib")
#pragma comment (lib, "CyhGui_mt.lib")
#else
#pragma comment (lib, "Cyh.lib")
#pragma comment (lib, "CyhODBC.lib")
#pragma comment (lib, "CyhGui.lib")
#endif // DEBUG
using namespace cyh;
namespace fs = std::filesystem;
namespace cfs = cyh::filesys;
using std::string;
using std::string_view;
using std::wstring;
using std::wstring_view;
using std::u8string;
using std::u8string_view;
using std::filesystem::path;
using cyh::filesys::new_path;
#include "LocalGit.h"

int main() {
	auto driverPath = new_path(fs::current_path(), "sqlite3odbcnw.dll");
	if (!fs::exists(driverPath)) {
		cyh::console::print("SQLite ODBC driver not found: ", driverPath.string());
		return -1;
	}
	cyh::odbc::odbc_init("sqlite3odbcnw.dll");
	cyh::extensions::LocalGit git{ R"(D:\Web\eHIS)" };
	// LocalGit git{ fs::current_path() };
	git.InitRepo();
	//git.BackupFiles(u8"Initial commit", u8"Tester");
	auto res = git.CompareCommit(1);
	//auto map = git.CompareCommits(14,15);
	auto a = git.RestoreFiles(1);
	system("pause");
};
