bool fnl_start(void* self_module); // fnl_entry.cpp: the fue core and the FuserNative features (C++20)

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
namespace fs = std::filesystem;

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <Psapi.h>

// Extended proxy APIs used by the supplied Windows 10 WinHTTP's internal proxy
// resolver. The definitions use the SDK signatures in a separate translation unit.
void load_extended_winhttp_functions(HMODULE dll);

// Controls whether PrintString/console etc output is produced at all
#ifndef FNL_DEBUG
#define FNL_DEBUG 0
#endif

#if FNL_DEBUG
#define DBG_PRINT(...) printf(__VA_ARGS__)
#else
#define DBG_PRINT(...)
#endif

#define ADD_ORIGINAL(i, name) originalFunctions[i] = GetProcAddress(dll, #name)

#define PROXY(i, name) \
	__declspec(dllexport) INT_PTR __stdcall name() \
	{ \
		return originalFunctions[i](); \
	}

FARPROC originalFunctions[50] = { 0 };

void loadFunctions(HMODULE dll)
{
	ADD_ORIGINAL(0, WinHttpAddRequestHeaders);
	ADD_ORIGINAL(1, WinHttpAutoProxySvcMain);
	ADD_ORIGINAL(2, WinHttpCheckPlatform);
	ADD_ORIGINAL(3, WinHttpCloseHandle);
	ADD_ORIGINAL(4, WinHttpConnect);
	ADD_ORIGINAL(5, WinHttpConnectionDeleteProxyInfo);
	ADD_ORIGINAL(6, WinHttpConnectionFreeNameList);
	ADD_ORIGINAL(7, WinHttpConnectionFreeProxyInfo);
	ADD_ORIGINAL(8, WinHttpConnectionFreeProxyList);
	ADD_ORIGINAL(9, WinHttpConnectionGetNameList);
	ADD_ORIGINAL(10, WinHttpConnectionGetProxyInfo);
	ADD_ORIGINAL(11, WinHttpConnectionGetProxyList);
	ADD_ORIGINAL(12, WinHttpConnectionSetProxyInfo);
	ADD_ORIGINAL(13, WinHttpCrackUrl);
	ADD_ORIGINAL(14, WinHttpCreateProxyResolver);
	ADD_ORIGINAL(15, WinHttpCreateUrl);
	ADD_ORIGINAL(16, WinHttpDetectAutoProxyConfigUrl);
	ADD_ORIGINAL(17, WinHttpFreeProxyResult);
	ADD_ORIGINAL(18, WinHttpGetDefaultProxyConfiguration);
	ADD_ORIGINAL(19, WinHttpGetIEProxyConfigForCurrentUser);
	ADD_ORIGINAL(20, WinHttpGetProxyForUrl);
	ADD_ORIGINAL(21, WinHttpGetProxyForUrlEx);
	ADD_ORIGINAL(22, WinHttpGetProxyResult);
	ADD_ORIGINAL(23, WinHttpGetTunnelSocket);
	ADD_ORIGINAL(24, WinHttpOpen);
	ADD_ORIGINAL(25, WinHttpOpenRequest);
	ADD_ORIGINAL(26, WinHttpProbeConnectivity);
	ADD_ORIGINAL(27, WinHttpQueryAuthSchemes);
	ADD_ORIGINAL(28, WinHttpQueryDataAvailable);
	ADD_ORIGINAL(29, WinHttpQueryHeaders);
	ADD_ORIGINAL(30, WinHttpQueryOption);
	ADD_ORIGINAL(31, WinHttpReadData);
	ADD_ORIGINAL(32, WinHttpReceiveResponse);
	ADD_ORIGINAL(33, WinHttpResetAutoProxy);
	ADD_ORIGINAL(34, WinHttpSaveProxyCredentials);
	ADD_ORIGINAL(35, WinHttpSendRequest);
	ADD_ORIGINAL(36, WinHttpSetCredentials);
	ADD_ORIGINAL(37, WinHttpSetDefaultProxyConfiguration);
	ADD_ORIGINAL(38, WinHttpSetOption);
	ADD_ORIGINAL(39, WinHttpSetStatusCallback);
	ADD_ORIGINAL(40, WinHttpSetTimeouts);
	ADD_ORIGINAL(41, WinHttpTimeFromSystemTime);
	ADD_ORIGINAL(42, WinHttpTimeToSystemTime);
	ADD_ORIGINAL(43, WinHttpWebSocketClose);
	ADD_ORIGINAL(44, WinHttpWebSocketCompleteUpgrade);
	ADD_ORIGINAL(45, WinHttpWebSocketQueryCloseStatus);
	ADD_ORIGINAL(46, WinHttpWebSocketReceive);
	ADD_ORIGINAL(47, WinHttpWebSocketSend);
	ADD_ORIGINAL(48, WinHttpWebSocketShutdown);
	ADD_ORIGINAL(49, WinHttpWriteData);
	load_extended_winhttp_functions(dll);

}

extern "C" {
	PROXY(0, WinHttpAddRequestHeaders);
	PROXY(1, WinHttpAutoProxySvcMain);
	PROXY(2, WinHttpCheckPlatform);
	PROXY(3, WinHttpCloseHandle);
	PROXY(4, WinHttpConnect);
	PROXY(5, WinHttpConnectionDeleteProxyInfo);
	PROXY(6, WinHttpConnectionFreeNameList);
	PROXY(7, WinHttpConnectionFreeProxyInfo);
	PROXY(8, WinHttpConnectionFreeProxyList);
	PROXY(9, WinHttpConnectionGetNameList);
	PROXY(10, WinHttpConnectionGetProxyInfo);
	PROXY(11, WinHttpConnectionGetProxyList);
	PROXY(12, WinHttpConnectionSetProxyInfo);
	PROXY(13, WinHttpCrackUrl);
	PROXY(14, WinHttpCreateProxyResolver);
	PROXY(15, WinHttpCreateUrl);
	PROXY(16, WinHttpDetectAutoProxyConfigUrl);
	PROXY(17, WinHttpFreeProxyResult);
	PROXY(18, WinHttpGetDefaultProxyConfiguration);
	PROXY(19, WinHttpGetIEProxyConfigForCurrentUser);
	PROXY(20, WinHttpGetProxyForUrl);
	PROXY(21, WinHttpGetProxyForUrlEx);
	PROXY(22, WinHttpGetProxyResult);
	PROXY(23, WinHttpGetTunnelSocket);
	PROXY(24, WinHttpOpen);
	PROXY(25, WinHttpOpenRequest);
	PROXY(26, WinHttpProbeConnectivity);
	PROXY(27, WinHttpQueryAuthSchemes);
	PROXY(28, WinHttpQueryDataAvailable);
	PROXY(29, WinHttpQueryHeaders);
	PROXY(30, WinHttpQueryOption);
	PROXY(31, WinHttpReadData);
	PROXY(32, WinHttpReceiveResponse);
	PROXY(33, WinHttpResetAutoProxy);
	PROXY(34, WinHttpSaveProxyCredentials);
	PROXY(35, WinHttpSendRequest);
	PROXY(36, WinHttpSetCredentials);
	PROXY(37, WinHttpSetDefaultProxyConfiguration);
	PROXY(38, WinHttpSetOption);
	PROXY(39, WinHttpSetStatusCallback);
	PROXY(40, WinHttpSetTimeouts);
	PROXY(41, WinHttpTimeFromSystemTime);
	PROXY(42, WinHttpTimeToSystemTime);
	PROXY(43, WinHttpWebSocketClose);
	PROXY(44, WinHttpWebSocketCompleteUpgrade);
	PROXY(45, WinHttpWebSocketQueryCloseStatus);
	PROXY(46, WinHttpWebSocketReceive);
	PROXY(47, WinHttpWebSocketSend);
	PROXY(48, WinHttpWebSocketShutdown);
	PROXY(49, WinHttpWriteData);
}

HMODULE mHinstDLL = 0;
bool hooked = false;

// Loads the original DLL from the default system directory
//	Function originally written by Michael Koch
void load_original_dll()
{
	char buffer[MAX_PATH];

	// Get path to system dir and to winhttp.dll
	GetSystemDirectoryA(buffer, MAX_PATH);

	// Append DLL name
	strcat_s(buffer, "\\winhttp.dll");

	// Try to load the system's winhttp.dll, if pointer empty
	if (!mHinstDLL) {
		mHinstDLL = LoadLibraryA(buffer);
	}

	// Debug
	if (!mHinstDLL) {
		OutputDebugStringA("PROXYDLL: Original winhttp.dll not loaded ERROR ****\r\n");
		ExitProcess(0); // Exit the hard way
	}
}

#include <funchook.h>
#include "hook_range.hpp"

#define CHECK_FUNCHOOK_ERR(arg, message) \
	if (arg != FUNCHOOK_ERROR_SUCCESS) printf("%s%s: %s", message, "fn", funchook_error_message(handle))

using byte = uint8_t;

// The pak-signature bypass for *_P paks: the engine functions it hooks or calls, found by byte pattern (FUSER_HOOK)
static void*(*FPakFile_Ctor)(void *ths, void *LowerLevel, const wchar_t *FileName, bool bIsSigned, bool bLoadIndex);
static void(*GetMasterHash)(void *p1, void *p2, void* outHash);
static void*(*GetPakSignatureFile)(void *ths, TCHAR* InFilename);
static uint64_t(*HashAndGetSize)(void*p1, void*p2, void*p3);
static void(*DoSignatureCheck)(void *ths, byte bWasCanceled, void *Request, int Index);
static void(*DoSignatureCheck_ContinueFn)(void *ths, byte bWasCanceled, void *Request, int Index);

static byte* Base;
size_t ModuleSize = 0;

// Per-thread state of a pak being opened: whether it is one of ours (*_P) and the hash to report for it.
// thread_local, not a shared map: GetMasterHash_Hook runs for every SHA-1 in the game on any thread (each new
// FChunkCacheWorker thread included), and unlocked inserts into a shared std::unordered_map race.
struct ThreadData {
	bool do_unsigned = false;
	byte* neededHash = nullptr;
};

static thread_local ThreadData PAK_THREAD_DATA;


void* FPakFile_Ctor_Hook(void *ths, void *LowerLevel, const wchar_t *FileName, bool bIsSigned, bool bLoadIndex) {
	auto &&data = PAK_THREAD_DATA;
	data.do_unsigned = wcsstr(FileName, L"_P") != nullptr;

	void *ret = FPakFile_Ctor(ths, LowerLevel, FileName, bIsSigned, bLoadIndex);
	printf("FPakFile - %ls (%s | %s)\n", FileName, bIsSigned ? "signed" : "not signed", bLoadIndex ? "load index" : "don't load index");
	data.do_unsigned = false;

	return ret;
}

void *GetPakSignatureFile_Hook(void *ths, TCHAR *InFilename) {
	auto &&data = PAK_THREAD_DATA;
	printf("Pak Signature for %ls\n", InFilename);
	data.do_unsigned = wcsstr(InFilename, L"_P") != nullptr;
	void* ret = GetPakSignatureFile(ths, InFilename);
	data.do_unsigned = false;

	return ret;
}

uint64_t HashAndGetSize_Hook(void*p1, void**p2, void*p3) {
	auto ret = HashAndGetSize(p1, p2, p3);
	if (ret == 0xffffffff) {
		return ret;
	}

	printf("Size: %zd\n", ret);
	auto &&data = PAK_THREAD_DATA;
	data.neededHash = reinterpret_cast<byte*>(*p2);

	printf("Copying Bytes: ");
	for (int i = 0; i < ret; i++)
		printf("%02x ", reinterpret_cast<byte*>(*p2)[i]);
	printf("\n");

	return ret;
}

void GetMasterHash_Hook(void *p1, void *p2, void* outHash) {
	GetMasterHash(p1, p2, outHash);
	auto &&data = PAK_THREAD_DATA;
	if (data.do_unsigned) {
		auto sz = 0x14;

		printf("Injecting Hash!\n");
		printf("Prev Hash: ");
		for (int i = 0; i < sz; i++)
			printf("%02x ", reinterpret_cast<byte*>(outHash)[i]);
		printf("\n");

		memcpy(outHash, data.neededHash, sz);

		printf("New Hash: ");
		for (int i = 0; i < sz; i++)
			printf("%02x ", reinterpret_cast<byte*>(outHash)[i]);
		printf("\n");
	}
}

void DoSignatureCheck_Hook(void *ths, byte bWasCanceled, void *Request, int Index) {
	//Just skip the whole signature check
	DoSignatureCheck_ContinueFn(ths, bWasCanceled, Request, Index);
}

std::vector<byte> HexToBytes(const std::string& hex) {
	std::vector<byte> bytes;

	for (unsigned int i = 0; i < hex.length(); i += 2) {
		std::string byteString = hex.substr(i, 2);
		byte b = (byte)strtol(byteString.c_str(), NULL, 16);
		bytes.push_back(b);
	}

	return bytes;
}

byte* find_function(const char *name, const char *byteSequence) {
	DBG_PRINT("Searching for %s.\n", name);
	auto data = HexToBytes(byteSequence);

	DBG_PRINT("	Byte Sequence: ");
	for (int i = 0; i < data.size(); i++)
		DBG_PRINT("%02x ", reinterpret_cast<byte*>(data.data())[i]);
	DBG_PRINT("\n");

	for (size_t i = 0; i < ModuleSize; ++i) {
		bool done = true;
		for (size_t j = 0; j < data.size(); ++j) {
			if ((*(Base + i + j) != *(data.data() + j)) && byteSequence[j * 2] != '?') {
				done = false;
				break;
			}
		}

		if (done) {
			DBG_PRINT("	Found! RVA: 0x%x\n", (unsigned int)i);
			return Base + i;
		}
	}

	DBG_PRINT("	No function found with that pattern!\n");
	return nullptr;
}

void setup_hook(void **target, void *hook) {
	printf("Preparing to hook function...\n");
	void *function = *target;
	auto handle = funchook_create();
	CHECK_FUNCHOOK_ERR(funchook_prepare(handle, target, hook), "funchook_prepare returned error: ");
	if (!hook_in_range(function, *target)) {
		printf("funchook's trampoline %p is out of jump range of %p - NOT hooked\n", *target, function);
		funchook_destroy(handle);
		*target = function;
		return;
	}
	CHECK_FUNCHOOK_ERR(funchook_install(handle, 0), "funchook_install returned error:");
	printf("Sucessfully hooked function!\n");
}

static void PatchByte(uintptr_t rva, uint8_t value) {
	uintptr_t addr = (uintptr_t)Base + rva;
	DWORD old;
	VirtualProtect((LPVOID)addr, 1, PAGE_EXECUTE_READWRITE, &old);
	*(uint8_t *)addr = value;
	VirtualProtect((LPVOID)addr, 1, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (LPVOID)addr, 1);
}

static void PatchPatternNear(uintptr_t startRva, size_t scanSize,
	const uint8_t *pattern, size_t patLen,
	size_t offset, uint8_t newByte) {
	byte *start = Base + startRva;
	for (size_t i = 0; i <= scanSize - patLen; i++) {
		if (memcmp(start + i, pattern, patLen) == 0) {
			PatchByte(startRva + i + offset, newByte);
			return;
		}
	}
}

#if FNL_DEBUG
// ---- the PrintString debug hook (FNL_DEBUG builds only) ----

// Reserves+commits a small RWX page as close as possible to `nearAddr`, searching outward in both
// directions in allocation-granularity steps until one succeeds or the +/-2GB search range (minus
// a safety margin) is exhausted. Used by PatchCallTarget below to build a landing pad for hook
// targets too far away for a direct `call rel32`/`jmp rel32` to reach.
static void *AllocateNear(uintptr_t nearAddr, size_t size) {
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t granularity = si.dwAllocationGranularity;
	const uintptr_t maxRange = 0x7FF00000; // stay safely under the 2GB rel32 limit
	uintptr_t base = nearAddr & ~(uintptr_t)(granularity - 1);
	for (uintptr_t offset = 0; offset < maxRange; offset += granularity) {
		void *mem = VirtualAlloc((LPVOID)(base + offset), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
		if (mem) return mem;
		if (offset != 0 && offset <= base) {
			mem = VirtualAlloc((LPVOID)(base - offset), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
			if (mem) return mem;
		}
	}
	return nullptr;
}

// Redirects a single `call rel32` (E8 xx xx xx xx) instruction's target, in place, without
// touching anything else. Used for the PrintString hook below: rather than hooking a whole
// function's entry point, this patches the ONE call site inside that thunk that calls into
// PrintString's own real implementation Since this only rewrites the 4 target bytes of ONE
// specific call instruction, it cannot affect any of that shared stub's other, unrelated
// callers elsewhere in the game.
//
// `rel32` only spans +/-2GB. Windows ASLR routinely places a loaded DLL and the main EXE much
// farther apart than that - a plain `(int32_t)(newTarget - callSite)` cast in that
// case doesn't overflow/clamp, it silently truncates to an unrelated small displacement, which
// crashes the game. Fixed by routing through a tiny indirect-jump trampoline :
//     (`jmp qword ptr [rip+0]` + the absolute 8-byte target)
// allocated within range whenever the direct displacement doesn't fit in an int32.
static void PatchCallTarget(uintptr_t rva, void *newTarget) {
	byte *addr = Base + rva;
	if (addr[0] != 0xE8) {
		printf("PatchCallTarget: expected E8 (call rel32) opcode at RVA 0x%llx, found 0x%02x - ABORTING, not patching\n", (unsigned long long)rva, addr[0]);
		return;
	}

	uintptr_t callSiteVA = (uintptr_t)(addr + 5);
	int64_t delta = (int64_t)(uintptr_t)newTarget - (int64_t)callSiteVA;
	void *finalTarget = newTarget;

	if (delta < -2147483648LL || delta > 2147483647LL) {
		void *stub = AllocateNear(callSiteVA, 16);
		if (!stub) {
			printf("PatchCallTarget: target %p is %lld bytes from call site (too far for rel32) and no nearby page could be reserved for a trampoline - ABORTING, not patching\n", newTarget, (long long)delta);
			return;
		}
		uint8_t *s = (uint8_t*)stub;
		s[0] = 0xFF; s[1] = 0x25; s[2] = 0; s[3] = 0; s[4] = 0; s[5] = 0; // jmp qword ptr [rip+0]
		*(uint64_t*)(s + 6) = (uint64_t)newTarget;
		FlushInstructionCache(GetCurrentProcess(), stub, 16);
		printf("PatchCallTarget: target %p is %lld bytes from call site (too far for rel32) - routed through trampoline at %p\n", newTarget, (long long)delta, stub);
		finalTarget = stub;
	}

	int32_t rel32 = (int32_t)((uintptr_t)finalTarget - callSiteVA);
	DWORD old;
	VirtualProtect(addr + 1, 4, PAGE_EXECUTE_READWRITE, &old);
	memcpy(addr + 1, &rel32, 4);
	VirtualProtect(addr + 1, 4, old, &old);
	FlushInstructionCache(GetCurrentProcess(), addr, 5);
	printf("PatchCallTarget: redirected call at RVA 0x%llx to %p\n", (unsigned long long)rva, newTarget);
}

static void LogToPrintStringLog(const wchar_t *line) {
	auto logPath = fs::current_path().parent_path().parent_path() / "Content" / "Paks" / "printstring_log.txt";
	FILE* f = nullptr;
	_wfopen_s(&f, logPath.c_str(), L"a");
	if (f) {
		fwprintf(f, L"%ls\n", line);
		fclose(f);
	}
}

// Diagnostic-only: mirrors every Blueprint PrintString call to the console/printstring_log.txt.
// Not needed for any shipped feature (see its install in FUSER_HOOK).
// UE4's FString on 64-bit Windows: {TCHAR* Data; int32 ArrayNum; int32 ArrayMax;} - 16 bytes.
// TCHAR is wchar_t on Windows.
struct FUE4String {
	wchar_t *Data;
	int32_t ArrayNum;
	int32_t ArrayMax;
};

void PrintString_Hook(void *WorldContextObject, FUE4String *InString, bool bPrintToScreen, bool bPrintToLog, void *TextColor, float Duration) {
	if (InString && InString->Data) {
		wprintf(L"[PrintString] %ls\n", InString->Data);
		fflush(stdout);

		// Mirrored to disk, so the trace can be read back after the fact without needing the
		// console window's scrollback.
		LogToPrintStringLog(InString->Data);
	}
}
#endif

static void ApplyBpmPatches() {
	// EOS
	if (*(uint8_t*)(Base + 0x594F51) == 0x9D) {
		PatchByte(0x594F51, 0xC8); // GetMaxBpm 157 -> 200
		PatchByte(0x594F81, 0x3C); // GetMinBpm 90 -> 60

		const uint8_t lower_cmp_cur[] = { 0x83, 0xFA, 0x5A };
		const uint8_t lower_mov[] = { 0xB8, 0x5A, 0x00, 0x00, 0x00 };
		const uint8_t upper_mov[] = { 0xB8, 0x9D, 0x00, 0x00, 0x00 };

		PatchPatternNear(0x5A1EB0, 0x120, lower_cmp_cur, 3, 2, 0x3C);
		PatchPatternNear(0x5A1EB0, 0x120, lower_mov, 5, 1, 0x3C);
		PatchPatternNear(0x5A1EB0, 0x120, upper_mov, 5, 1, 0xC8);

		const uint8_t lower_cmp_bpm[] = { 0x83, 0xFB, 0x5A };

		PatchPatternNear(0x59ADB0, 0x120, lower_cmp_bpm, 3, 2, 0x3C);
		PatchPatternNear(0x59ADB0, 0x120, lower_mov, 5, 1, 0x3C);
		PatchPatternNear(0x59ADB0, 0x120, upper_mov, 5, 1, 0xC8);
		return;
	}

	// Steam
	if (*(uint8_t*)(Base + 0x5CC581) == 0x9D) {
		PatchByte(0x5CC581, 0xC8); // GetMaxBpm 157 -> 200
		PatchByte(0x5CC5B1, 0x3C); // GetMinBpm 90 -> 60

		const uint8_t lower_cmp_cur[] = { 0x83, 0xFA, 0x5A };
		const uint8_t lower_cmp_bpm[] = { 0x83, 0xFB, 0x5A };
		const uint8_t lower_mov[] = { 0xB8, 0x5A, 0x00, 0x00, 0x00 };
		const uint8_t upper_mov[] = { 0xB8, 0x9D, 0x00, 0x00, 0x00 };

		PatchPatternNear(0x5D9454, 0x80, lower_cmp_cur, 3, 2, 0x3C);
		PatchPatternNear(0x5D9454, 0x80, lower_mov, 5, 1, 0x3C);
		PatchPatternNear(0x5D9454, 0x80, upper_mov, 5, 1, 0xC8);

		PatchPatternNear(0x5D23E4, 0x80, lower_cmp_bpm, 3, 2, 0x3C);
		PatchPatternNear(0x5D23E4, 0x80, lower_mov, 5, 1, 0x3C);
		PatchPatternNear(0x5D23E4, 0x80, upper_mov, 5, 1, 0xC8);
		return;
	}
}

// The community's tenth genre. Custom songs use EGenre::Num (9) as "Indie/Alternative" (its display text comes from
// a Hype.locres override, "Fuser Genres Rework"). The game reads genre 9 by name and display text like any other,
// except the multiplayer live-requests screen: its song gather (FUN_1406440b0) builds one entry per genre for
// i < 9 and then dereferences a missing entry, so a crate song of genre 9 crashed it (ISSUES.md, the genre section).
// Widening that loop to i < 10 gives genre 9 its own entry - a requestable genre with its display text. Each build's
// site is checked byte for byte before the bound is changed; values >= 10 are still clamped by FuserNative, and
// genre 9 too when the patch did not apply (fnl_genre9_requestable, read by fusernative.cpp's clamp_song_genres).
bool fnl_genre9_requestable = false;
static void ApplyGenreRequestsPatch() {
	// inc r14d; cmp r14d,9; jl <loop top>  (identical in all three builds)
	static const uint8_t site[] = { 0x41, 0xFF, 0xC6, 0x41, 0x83, 0xFE, 0x09, 0x0F, 0x8C, 0x12, 0xFF, 0xFF, 0xFF };
	const uintptr_t candidates[] = {
		0x644381, // EGS v1.4.2 and EGS CL1317808
		0x67A3E1, // Steam CL1317755
	};
	for (uintptr_t rva : candidates) {
		if (rva + sizeof(site) > ModuleSize || memcmp(Base + rva, site, sizeof(site)) != 0) continue;
		PatchByte(rva + 6, 0x0A); // cmp r14d,10
		fnl_genre9_requestable = Base[rva + 6] == 0x0A;
		printf("Genre patch: live-requests genre loop at RVA 0x%llx %s\n", (unsigned long long)(rva + 3),
			fnl_genre9_requestable ? "now covers genre 9" : "could NOT be written - genre 9 stays clamped");
		return;
	}
	printf("Genre patch: live-requests genre loop not found - genre 9 stays clamped\n");
}

void FUSER_HOOK() {
	Base = (byte*)GetModuleHandleA("Fuser-Win64-Shipping.exe");
	if (Base == 0) {
		Base = (byte*)GetModuleHandleA("FuserEOS-Win64-Shipping.exe");

		if (Base == 0) {
			printf("ERROR! Base wasn't found!\n");
			return;
		}
	}
	
	MODULEINFO moduleInfo;
	GetModuleInformation(GetCurrentProcess(), (HMODULE)Base, &moduleInfo, sizeof(moduleInfo));
	ModuleSize = moduleInfo.SizeOfImage;

	printf("Searching %zd bytes for the pak-signature functions...\n\n", ModuleSize);

	FPakFile_Ctor = (decltype(FPakFile_Ctor)) find_function("FPakFile_Ctor", "48895c2410555657415641574883ec304533ff410fb6e94c8939498bf04c8979084c8bf2488bd94d85c0743f66453938");
	GetMasterHash = (decltype(GetMasterHash)) find_function("GetMasterHash", "4c8bdc534881ec00010000488b05????????4833c448898424f0000000498d4398498bd8498943d84c8bc233c0c744242001234567");
	GetPakSignatureFile = (decltype(GetPakSignatureFile)) find_function("GetPakSignatureFile", "405556574154488dac2478ffffff4881ec88010000488b05????????4833c4488945784c8be1488bf2488d0d");
	HashAndGetSize = (decltype(HashAndGetSize)) find_function("HashAndGetSize", "48895c24084889742410574883ec30498bd8488bfa488bf1e8530000004885c0742d0f10064c8b10488d5424204c8bcb4c8bc7488bc80f29442420");
	DoSignatureCheck = (decltype(DoSignatureCheck)) find_function("DoSignatureCheck", "4055535657415541564157488d6c24f04881ec10010000488b05????????4833c44889450033c04963f1488d590848894c2438");
	DoSignatureCheck_ContinueFn = (decltype(DoSignatureCheck_ContinueFn)) find_function("DoSignatureCheck_Continue", "48895c240848896c2410488974241857415641574883ec20488bf14963f94883c108498be8440fb6f2");

	setup_hook((void**)&FPakFile_Ctor, FPakFile_Ctor_Hook);
	setup_hook((void**)&GetMasterHash, GetMasterHash_Hook);
	setup_hook((void**)&GetPakSignatureFile, GetPakSignatureFile_Hook);
	setup_hook((void**)&HashAndGetSize, HashAndGetSize_Hook);
	setup_hook((void**)&DoSignatureCheck, DoSignatureCheck_Hook);

	// PrintString's real implementation is a stripped no-op in this Shipping build (raw bytes `ret 0`,
	// confirmed statically and live). The call-site RVAs below are the EGS exes' (v1.4.2, CL1317808); a site
	// is used only if it holds a call to PrintString (RVA 0x33dfb0 in both), otherwise the hook is skipped.
	// Debug builds only.
#if FNL_DEBUG
	{
		const uintptr_t sites[] = { 0x1fe9963, 0x1fe9993 };
		bool hooked = false;
		for (uintptr_t site : sites) {
			const uint8_t* p = Base + site;
			if (p[0] == 0xE8 && site + 5 + *(const int32_t*)(p + 1) == 0x33dfb0) {
				PatchCallTarget(site, (void*)&PrintString_Hook);
				hooked = true;
				break;
			}
		}
		if (!hooked) printf("PrintString hook: no known call site found - skipping (wrong exe build/version?)\n");
	}
#endif
}

// Custom and event songs are unlocked at runtime now (src/unlocks.cpp). The pak this loader used to
// generate - a full DT_UnlocksSongs override - is still mounted by the engine if it is left in Content/Paks, and would
// replace the live table with its stale copy, so it is renamed out of the way once (reversible: drop ".disabled").
static void DisableLegacyUnlockPak() {
	wchar_t buf[MAX_PATH]{};
	GetModuleFileNameW(nullptr, buf, MAX_PATH);
	const auto paks = fs::path(buf).parent_path().parent_path().parent_path() / "Content" / "Paks";
	for (const wchar_t* name : {L"customSongsUnlocked_P.pak", L"customSongsUnlocked_P.sig"}) {
		std::error_code ec;
		const auto from = paks / name;
		if (!fs::exists(from, ec)) continue;
		auto to = from;
		to += L".disabled";
		fs::rename(from, to, ec); // replaces an older .disabled copy
		printf("Legacy unlock pak: %ls -> %ls (%s)\n", from.c_str(), to.c_str(), ec ? ec.message().c_str() : "ok");
	}
}

BOOL WINAPI DllMain(HMODULE hinstDLL, DWORD fdwReason, LPVOID lpvReserved) {
	if (fdwReason == DLL_PROCESS_ATTACH) {
		load_original_dll();
		loadFunctions(mHinstDLL);
	}
	else if (fdwReason == DLL_PROCESS_DETACH) {
		FreeLibrary(mHinstDLL);
	}
	
	if (hooked) {
		return (TRUE);
	}
	
	if (fdwReason == DLL_THREAD_ATTACH) {
		hooked = true;
		
//To Enable Console
#if FNL_DEBUG
		//Use existing console, or create one if needed
		if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
			AllocConsole();
			ShowWindow(GetConsoleWindow(), SW_SHOW);
		}
	
		//Redirect output to console
		FILE* fp;
		freopen_s(&fp, "CONOIN$", "r", stdin);
		freopen_s(&fp, "CONOUT$", "w", stdout);
		freopen_s(&fp, "CONOUT$", "w", stderr);
		
#endif
		DisableLegacyUnlockPak();
		FUSER_HOOK();
		ApplyBpmPatches();
		ApplyGenreRequestsPatch();
		// the UE 4.24 reflection core (fue), the runtime unlocks and the FuserNative features
		fnl_start(hinstDLL);
	}

	return (TRUE);
}
