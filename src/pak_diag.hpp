#pragma once
// Pak diagnostics (pak_diag.cpp), in every build: FUSER's pak-signature failure handlers (they request a clean engine
// exit, so a bad pak closes the game with no crash dump), the paks opened once the game is running, and the DLC scan's
// progress, all written to FuserNative.log.

// Called once from fnl_start: hooks the signature failure handlers and registers the tick.
auto pak_diag_start() -> void;

// dllmain.cpp's FPakFile constructor hook, after the constructor: logged only once the engine has ticked (startup's
// mounts are not logged).
void pak_diag_opened(const wchar_t* file_name, bool is_signed);
