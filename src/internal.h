#pragma once
#include <string>

void LogF(const char* fmt, ...);
const std::wstring& RootDir();   // ...\Binaries\Win64\rocketify

void StartInput();               // controller / keyboard polling for MODS navigation
void StopInput();
