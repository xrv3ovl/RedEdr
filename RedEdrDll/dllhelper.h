#pragma once
#include <stdio.h>
#include <winternl.h>  // needs to be on bottom?

// Pipe
void InitDllPipe();
void SendDllPipe(wchar_t* buffer);


// Proc
void LogMyStackTrace();

// Utils
LARGE_INTEGER get_time();
void UnicodeStringToWChar(const UNICODE_STRING* ustr, wchar_t* dest, size_t destSize);
