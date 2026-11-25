// Force-included in every file of the test: MoveFileExW goes through
// TestMoveFileExW, which is the real call unless a test names moves to fail.
#pragma once
#include <windows.h>

BOOL TestMoveFileExW(LPCWSTR from, LPCWSTR to, DWORD flags);

// The 1-based numbers of the moves that fail, counted from this call.
void FailMoves(int first, int second);

#define MoveFileExW TestMoveFileExW
