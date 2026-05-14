#include "RelocTaxonomy.h"

#include <atomic>
#include <cstdio>
#include <mutex>
#include <system_error>

#include "Common/precompiled.h"
#include "config/ActiveSettings.h"

namespace JitRelocTaxonomy
{

namespace
{

// Single mutex guards both the FILE* and the per-function state. PPCRecompiler
// is single-threaded today (one compiler worker), but main-thread JIT entry
// from PPCRecompilerCore can also drive codegen in rare paths -- so be safe.
std::mutex g_mutex;
FILE* g_logFile = nullptr;
bool g_initAttempted = false;
bool g_initFailed = false;
uint32_t g_currentPpcAddr = 0;
size_t g_currentStartOffset = 0;
bool g_inFunction = false;
std::atomic<uint64_t> g_functionsRecorded{0};
std::atomic<uint64_t> g_sitesRecorded{0};

void openLogFileLocked()
{
	if (g_initAttempted)
		return;
	g_initAttempted = true;

	std::error_code ec;
	fs::path dir = ActiveSettings::GetUserDataPath("log");
	fs::create_directories(dir, ec);
	// ignore ec -- if the dir already exists or can't be created we'll fall
	// through and let fopen surface the actual error
	fs::path file = dir / "jit_reloc_taxonomy.csv";

#ifdef _WIN32
	auto pathW = file.wstring();
	g_logFile = _wfopen(pathW.c_str(), L"wb");
#else
	g_logFile = std::fopen(file.string().c_str(), "wb");
#endif

	if (!g_logFile)
	{
		g_initFailed = true;
		return;
	}

	// Header row. value is logged as raw uint64 hex; kind as the enum name
	// string so the CSV is grep-able without a key table.
	std::fputs("ppcAddr,codeOffset,kind,value,hostFuncSize\n", g_logFile);
	std::fflush(g_logFile);
}

} // namespace

const char* kindName(AbsImm64Kind k)
{
	switch (k)
	{
	case AbsImm64Kind::HLE_GLOBAL_PTR:        return "HLE_GLOBAL_PTR";
	case AbsImm64Kind::HLE_FUNCTION_PTR:      return "HLE_FUNCTION_PTR";
	case AbsImm64Kind::RECOMPILER_HELPER_PTR: return "RECOMPILER_HELPER_PTR";
	case AbsImm64Kind::PPC_CALL_IMM_TARGET:   return "PPC_CALL_IMM_TARGET";
	case AbsImm64Kind::LITERAL_CONST:         return "LITERAL_CONST";
	}
	return "?";
}

void beginFunction(uint32_t ppcAddress)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	openLogFileLocked();
	g_currentPpcAddr = ppcAddress;
	g_currentStartOffset = 0; // populated lazily on first record / endFunction
	g_inFunction = true;
}

void endFunction(size_t hostSize)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	g_inFunction = false;
	// Periodically flush so the CSV is readable while a session is live.
	const uint64_t n = g_functionsRecorded.fetch_add(1, std::memory_order_relaxed) + 1;
	if (g_logFile && (n % 256) == 0)
		std::fflush(g_logFile);
	(void)hostSize; // currently unused; kept in the API for a later size column if needed
}

void recordAbsImm64(size_t codeOffset, uint64_t value, AbsImm64Kind kind)
{
	std::lock_guard<std::mutex> lk(g_mutex);
	if (g_initFailed || !g_inFunction)
		return;
	if (!g_logFile)
		openLogFileLocked();
	if (!g_logFile)
		return;
	std::fprintf(g_logFile,
	             "0x%08x,0x%zx,%s,0x%016llx,\n",
	             g_currentPpcAddr,
	             codeOffset,
	             kindName(kind),
	             static_cast<unsigned long long>(value));
	g_sitesRecorded.fetch_add(1, std::memory_order_relaxed);
}

} // namespace JitRelocTaxonomy
