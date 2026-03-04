#include "hooks_common.h"
#include <cstring>
#include <malloc.h>
#include <cstdio>
#include "logger.h"  // Add ModLoader logger

// ---------------------------------------------------------------------------
// Lightweight x64 instruction length decoder
// Based on length disassembly engine (LDE) principles
// ---------------------------------------------------------------------------

namespace Hooks
{
	// Check if byte is a legacy prefix
	static bool IsLegacyPrefix(uint8_t b)
	{
		return (b == 0xF0 || b == 0xF2 || b == 0xF3 ||  // LOCK, REPNE, REP
			b == 0x2E || b == 0x36 || b == 0x3E ||  // Segment overrides
			b == 0x26 || b == 0x64 || b == 0x65 ||
			b == 0x66 || b == 0x67);      // Operand/Address size
	}

	// Check if byte is a REX prefix (0x40-0x4F)
	static bool IsRex(uint8_t b)
	{
		return (b >= 0x40 && b <= 0x4F);
	}

	size_t GetInstructionLength(const uint8_t* code)
	{
		if (!code)
		{
			ModLoader::LogMessage(L"[Hooks] GetInstructionLength: null code pointer");
			return 0;
		}

		const uint8_t* start = code;
		size_t len = 0;
		bool hasRexW = false;

		// Skip legacy prefixes
		while (len < 15 && IsLegacyPrefix(*code))
		{
			code++;
			len++;
		}

		// Skip REX prefix (x64) and check for REX.W
		if (len < 15 && IsRex(*code))
		{
			hasRexW = (*code & 0x08) != 0; // REX.W bit
			code++;
			len++;
		}

		if (len >= 15)
		{
			ModLoader::LogMessage(L"[Hooks] GetInstructionLength: too many prefixes (>15 bytes)");
			return 15;
		}

		// Read opcode
		uint8_t opcode = *code++;
		len++;

		bool hasModRM = false;
		size_t immSize = 0;
		size_t dispSize = 0;

		// Two-byte opcode (0F xx)
		if (opcode == 0x0F)
		{
			if (len >= 15)
			{
				ModLoader::LogMessage(L"[Hooks] GetInstructionLength: instruction too long");
				return 15;
			}

			opcode = *code++;
			len++;

			// Three-byte opcode (0F 38 xx or 0F 3A xx)
			if (opcode == 0x38 || opcode == 0x3A)
			{
				if (len >= 15)
				{
					ModLoader::LogMessage(L"[Hooks] GetInstructionLength: instruction too long");
					return 15;
				}

				code++;
				len++;
				hasModRM = true;
				if (opcode == 0x3A)
					immSize = 1; // Most 0F 3A instructions have imm8
			}
			else if (opcode >= 0x80 && opcode <= 0x8F)
			{
				// Two-byte Jcc rel32: 0F 80..0F 8F — 4-byte immediate, NO ModRM
				hasModRM = false;
				immSize = 4;
			}
			else
			{
				// Most 0F xx instructions have ModR/M
				hasModRM = true;
			}
		}
		else
		{
			// Single-byte opcode - determine if it has ModR/M
			switch (opcode)
			{
				// Instructions WITHOUT ModR/M
			case 0x50: case 0x51: case 0x52: case 0x53: // PUSH r64
			case 0x54: case 0x55: case 0x56: case 0x57:
			case 0x58: case 0x59: case 0x5A: case 0x5B: // POP r64
			case 0x5C: case 0x5D: case 0x5E: case 0x5F:
			case 0x90: case 0x91: case 0x92: case 0x93: // XCHG, NOP
			case 0x94: case 0x95: case 0x96: case 0x97:
			case 0x98: case 0x99: case 0x9C: case 0x9D: // CBW, CWD, PUSHF, POPF
			case 0x9E: case 0x9F: // SAHF, LAHF
			case 0xC3: case 0xC9: case 0xCC: case 0xF4: // RET, LEAVE, INT3, HLT
			case 0xC2: // RET imm16
				if (opcode == 0xC2)
					immSize = 2;
				hasModRM = false;
				break;

				// Instructions with immediate values (no ModR/M)
			case 0xB0: case 0xB1: case 0xB2: case 0xB3: // MOV r8, imm8
			case 0xB4: case 0xB5: case 0xB6: case 0xB7:
				immSize = 1;
				hasModRM = false;
				break;

			case 0xB8: case 0xB9: case 0xBA: case 0xBB: // MOV r32/r64, imm32/imm64
			case 0xBC: case 0xBD: case 0xBE: case 0xBF:
				// With REX.W: imm64 (8 bytes), without: imm32 (4 bytes)
				immSize = hasRexW ? 8 : 4;
				hasModRM = false;
				break;

			case 0x6A: // PUSH imm8
				immSize = 1;
				hasModRM = false;
				break;

			case 0x68: // PUSH imm32
				immSize = 4;
				hasModRM = false;
				break;

			case 0xE8: case 0xE9: // CALL, JMP rel32
				immSize = 4;
				hasModRM = false;
				break;

			case 0xEB: // JMP rel8
			case 0x70: case 0x71: case 0x72: case 0x73: // Jcc rel8
			case 0x74: case 0x75: case 0x76: case 0x77:
			case 0x78: case 0x79: case 0x7A: case 0x7B:
			case 0x7C: case 0x7D: case 0x7E: case 0x7F:
				immSize = 1;
				hasModRM = false;
				break;

				// Arithmetic/logic with immediate operands (have ModR/M + immediate)
			case 0x80: case 0x82: // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m8, imm8
				hasModRM = true;
				immSize = 1;
				break;

			case 0x81: // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m32/64, imm32
				hasModRM = true;
				immSize = 4;
				break;

			case 0x83: // ADD/OR/ADC/SBB/AND/SUB/XOR/CMP r/m32/64, imm8 (sign-extended)
				hasModRM = true;
				immSize = 1;
				break;

			case 0xC0: case 0xC1: // Shift/rotate r/m by imm8
				hasModRM = true;
				immSize = 1;
				break;

			case 0xC6: // MOV r/m8, imm8
				hasModRM = true;
				immSize = 1;
				break;

			case 0xC7: // MOV r/m32/64, imm32
				hasModRM = true;
				immSize = 4;
				break;

			case 0x69: // IMUL r32/64, r/m32/64, imm32
				hasModRM = true;
				immSize = 4;
				break;

			case 0x6B: // IMUL r32/64, r/m32/64, imm8
				hasModRM = true;
				immSize = 1;
				break;

			case 0xF6: // TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m8
			case 0xF7: // TEST/NOT/NEG/MUL/IMUL/DIV/IDIV r/m32/64
				// reg field of ModRM determines whether there's an immediate:
				// /0 (TEST) and /1 (TEST) have an immediate; /2-/7 do not.
				// We must peek at the ModRM byte to decide.
				hasModRM = true;
				if (len < 15)
				{
					uint8_t modrm_peek = code[0]; // next byte is ModRM
					uint8_t reg_field = (modrm_peek >> 3) & 0x07;
					if (reg_field == 0 || reg_field == 1) // TEST
						immSize = (opcode == 0xF6) ? 1 : 4;
				}
				break;

				// Most other opcodes have ModR/M (no immediate)
			default:
				hasModRM = true;
				break;
			}
		}

		// Process ModR/M byte
		if (hasModRM)
		{
			if (len >= 15)
			{
				ModLoader::LogMessage(L"[Hooks] GetInstructionLength: instruction too long");
				return 15;
			}

			uint8_t modrm = *code++;
			len++;

			uint8_t mod = (modrm >> 6) & 0x03;
			uint8_t rm = modrm & 0x07;

			// Check for SIB byte (when mod != 11 and rm == 100)
			if (mod != 0x03 && rm == 0x04)
			{
				if (len >= 15)
				{
					ModLoader::LogMessage(L"[Hooks] GetInstructionLength: instruction too long");
					return 15;
				}

				uint8_t sib = *code++;
				len++;

				// Special case: SIB with base = 101 and mod = 00 means disp32
				uint8_t base = sib & 0x07;
				if (mod == 0x00 && base == 0x05)
					dispSize = 4;
			}

			// Displacement size based on mod
			if (mod == 0x00 && rm == 0x05)
				dispSize = 4; // [rip+disp32]
			else if (mod == 0x01)
				dispSize = 1; // disp8
			else if (mod == 0x02)
				dispSize = 4; // disp32
		}

		// Add displacement
		len += dispSize;

		// Add immediate
		len += immSize;

		if (len > 15)
		{
			ModLoader::LogMessage(L"[Hooks] GetInstructionLength: instruction exceeds 15 bytes, capping at 15");
			return 15;
		}

		return len;
	}

	size_t CalculateStolenBytes(const uint8_t* code, size_t minBytes)
	{
		size_t totalLen = 0;
		size_t instrCount = 0;

		ModLoader::LogDebug(L"[Hooks] CalculateStolenBytes: calculating bytes needed (minimum: %zu)", minBytes);

		while (totalLen < minBytes && totalLen < 64)
		{
			size_t instrLen = GetInstructionLength(code + totalLen);

			if (instrLen == 0 || instrLen > 15)
			{
				ModLoader::LogDebug(L"[Hooks] CalculateStolenBytes: invalid instruction at offset %zu (length=%zu)",
					totalLen, instrLen);
				return 0;
			}

			ModLoader::LogDebug(L"[Hooks]   Instruction #%zu at offset %zu: %zu bytes",
				instrCount + 1, totalLen, instrLen);

			totalLen += instrLen;
			instrCount++;
		}

		ModLoader::LogDebug(L"[Hooks] CalculateStolenBytes: stealing %zu bytes (%zu instructions) to cover minimum %zu bytes",
			totalLen, instrCount, minBytes);

		return totalLen;
	}

	// ---------------------------------------------------------------------------
	// Memory patching helpers
	// ---------------------------------------------------------------------------

	bool Patch(uintptr_t address, const uint8_t* data, size_t size)
	{
		ModLoader::LogDebug(L"[Hooks] Patch: writing %zu bytes at 0x%llX", size,
			static_cast<unsigned long long>(address));

		// Read the current bytes first for the log
		uint8_t oldBytes[64]{};
		size_t readSize = (size <= sizeof(oldBytes)) ? size : sizeof(oldBytes);
		SIZE_T bytesRead = 0;
		if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
			oldBytes, readSize, &bytesRead))
		{
			ModLoader::LogDebug(L"[Hooks] Bytes before patch: [...]");
		}

		DWORD oldProtect = 0;
		ModLoader::LogDebug(L"[Hooks] Patch: calling VirtualProtect(0x%llX, %zu, PAGE_EXECUTE_READWRITE)",
			static_cast<unsigned long long>(address), size);

		if (!VirtualProtect(reinterpret_cast<void*>(address), size, PAGE_EXECUTE_READWRITE, &oldProtect))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: Patch: VirtualProtect failed at 0x%llX (error %lu)",
				static_cast<unsigned long long>(address), GetLastError());
			return false;
		}

		ModLoader::LogDebug(L"[Hooks] Patch: previous protection was 0x%lX", oldProtect);

		memcpy(reinterpret_cast<void*>(address), data, size);

		DWORD temp = 0;
		VirtualProtect(reinterpret_cast<void*>(address), size, oldProtect, &temp);
		ModLoader::LogDebug(L"[Hooks] Patch: protection restored to 0x%lX", oldProtect);

		FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(address), size);
		ModLoader::LogDebug(L"[Hooks] Patch: instruction cache flushed");

		// Verify the write
		uint8_t verifyBuf[64]{};
		if (readSize <= sizeof(verifyBuf) &&
			ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
				verifyBuf, readSize, &bytesRead))
		{
			bool verified = (memcmp(verifyBuf, data, readSize) == 0);
			if (verified)
				ModLoader::LogDebug(L"[Hooks] Patch: write verified OK at 0x%llX", static_cast<unsigned long long>(address));
			else
			{
				ModLoader::LogMessage(L"[Hooks] ERROR: Patch: VERIFICATION FAILED at 0x%llX – bytes don't match!",
					static_cast<unsigned long long>(address));
			}
		}

		return true;
	}

	bool Nop(uintptr_t address, size_t size)
	{
		ModLoader::LogDebug(L"[Hooks] NOP: filling %zu bytes with 0x90 at 0x%llX",
			size, static_cast<unsigned long long>(address));

		auto* nops = static_cast<uint8_t*>(_alloca(size));
		memset(nops, 0x90, size);
		bool result = Patch(address, nops, size);

		if (result)
			ModLoader::LogDebug(L"[Hooks] NOP: success at 0x%llX (%zu bytes)", static_cast<unsigned long long>(address), size);
		else
			ModLoader::LogMessage(L"[Hooks] ERROR: NOP: failed at 0x%llX", static_cast<unsigned long long>(address));

		return result;
	}

	bool ReadMemory(uintptr_t address, void* buffer, size_t size)
	{
		ModLoader::LogDebug(L"[Hooks] ReadMemory: reading %zu bytes from 0x%llX",
			size, static_cast<unsigned long long>(address));

		SIZE_T bytesRead = 0;
		bool success = ReadProcessMemory(
			GetCurrentProcess(),
			reinterpret_cast<const void*>(address),
			buffer,
			size,
			&bytesRead) && bytesRead == size;

		if (success)
			ModLoader::LogDebug(L"[Hooks] ReadMemory: read %llu bytes successfully", static_cast<unsigned long long>(bytesRead));
		else
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: ReadMemory: failed at 0x%llX (requested %zu, got %llu, error %lu)",
				static_cast<unsigned long long>(address), size,
				static_cast<unsigned long long>(bytesRead), GetLastError());
		}

		return success;
	}

	// ---------------------------------------------------------------------------
	// Inline hook – 14 byte absolute JMP for x64
	// Uses dynamic instruction length detection to steal complete instructions
	// ---------------------------------------------------------------------------

	static constexpr size_t kJmpSize = 14; // 6 (jmp [rip+0]) + 8 (address)

	bool Hook::Install(uintptr_t targetAddr, void* detourFunc, void** originalFunc)
	{
		ModLoader::LogMessage(L"[Hooks] ###################################################################################");
		ModLoader::LogMessage(L"[Hooks] Hook::Install: target=0x%llX  detour=0x%llX",
			static_cast<unsigned long long>(targetAddr),
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(detourFunc)));

		if (installed)
		{
			ModLoader::LogMessage(L"[Hooks] WARN: Hook::Install: hook already installed at 0x%llX – aborting",
				static_cast<unsigned long long>(target));
			return false;
		}

		target = targetAddr;
		detour = reinterpret_cast<uintptr_t>(detourFunc);

		// Read enough bytes to analyze instructions
		uint8_t codeBuffer[64]{};
		if (!ReadMemory(target, codeBuffer, sizeof(codeBuffer)))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: Hook::Install: failed to read code at 0x%llX for analysis",
				static_cast<unsigned long long>(target));
			return false;
		}

		// Calculate how many bytes we need to steal to cover the 14-byte jump
		patchSize = CalculateStolenBytes(codeBuffer, kJmpSize);

		if (patchSize == 0 || patchSize > sizeof(originalBytes))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: Hook::Install: failed to calculate stolen bytes (got %zu, max %zu)",
				patchSize, sizeof(originalBytes));
			return false;
		}

		ModLoader::LogDebug(L"[Hooks] Hook::Install: JMP size=%zu bytes, dynamically calculated stolen bytes=%zu",
			kJmpSize, patchSize);

		// Save original bytes
		memcpy(originalBytes, codeBuffer, patchSize);
		{
			// Log the raw stolen bytes as hex
			char hexBuf[64 * 3 + 1]{};
			size_t pos = 0;
			for (size_t i = 0; i < patchSize && pos + 3 <= sizeof(hexBuf) - 1; i++)
				pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", originalBytes[i]);
			ModLoader::LogDebug(L"[Hooks] Stolen bytes (hex): %S", hexBuf);
		}

		// Allocate trampoline
		// Extra headroom for short-jump expansion:
		//   EB rel8 (2B) -> E9 rel32 (5B): +3 bytes per expansion
		//   7x rel8 (2B) -> 0F 8x rel32 (6B): +4 bytes per expansion
		// Up to ~14 stolen instructions, worst case all are short jumps: 14*4 = 56 bytes extra
		static constexpr size_t kExpansionHeadroom = 64;
		size_t trampolineSize = patchSize + kExpansionHeadroom + kJmpSize;
		ModLoader::LogDebug(L"[Hooks] Hook::Install: allocating trampoline (%zu bytes = %zu stolen + %zu expansion headroom + %zu JMP back)",
			trampolineSize, patchSize, kExpansionHeadroom, kJmpSize);

		trampoline = nullptr;

		// Try to allocate within ±2GB of target using VirtualAlloc with address hints
		// This allows RIP-relative instructions to work correctly
		const size_t maxDistance = 0x7FFFFFFF; // 2GB (max for signed 32-bit offset)
		uintptr_t minAddr = (target > maxDistance) ? (target - maxDistance) : 0;
		uintptr_t maxAddr = target + maxDistance;

		// Align to allocation granularity (64KB on Windows)
		const size_t allocGranularity = 64 * 1024;
		minAddr = (minAddr / allocGranularity) * allocGranularity;
		maxAddr = (maxAddr / allocGranularity) * allocGranularity;

		ModLoader::LogDebug(L"[Hooks] Hook::Install: attempting to allocate trampoline near target");
		ModLoader::LogDebug(L"[Hooks]   Target address:     0x%016llX", static_cast<unsigned long long>(target));
		ModLoader::LogDebug(L"[Hooks]   Acceptable range:   0x%016llX - 0x%016llX",
			static_cast<unsigned long long>(minAddr),
			static_cast<unsigned long long>(maxAddr));
		ModLoader::LogDebug(L"[Hooks]   Max distance:       +/-%zu MB", maxDistance / (1024 * 1024));

		// Try multiple candidate addresses around the target
		// Strategy: scan memory for free regions within ±2GB
		MEMORY_BASIC_INFORMATION mbi{};
		uintptr_t searchAddr = (minAddr > allocGranularity) ? minAddr : allocGranularity;

		ModLoader::LogDebug(L"[Hooks]   Scanning for free memory regions within range...");

		int regionsChecked = 0;
		while (searchAddr < maxAddr && !trampoline && regionsChecked < 1000)
		{
			SIZE_T result = VirtualQuery(reinterpret_cast<void*>(searchAddr), &mbi, sizeof(mbi));

			if (result == 0)
			{
				// Query failed, move to next allocation granularity boundary
				searchAddr += allocGranularity;
				continue;
			}

			regionsChecked++;

			// Check if this region is free and large enough
			if (mbi.State == MEM_FREE && mbi.RegionSize >= trampolineSize)
			{
				// Try to allocate in this free region
				uintptr_t candidateAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress);

				// Align to allocation granularity
				if (candidateAddr % allocGranularity != 0)
				{
					candidateAddr = ((candidateAddr / allocGranularity) + 1) * allocGranularity;
				}

				// Make sure it's still in our acceptable range
				if (candidateAddr >= minAddr && candidateAddr <= maxAddr)
				{
					void* allocResult = VirtualAlloc(
						reinterpret_cast<void*>(candidateAddr),
						trampolineSize,
						MEM_COMMIT | MEM_RESERVE,
						PAGE_EXECUTE_READWRITE);

					if (allocResult)
					{
						trampoline = static_cast<uint8_t*>(allocResult);

						// Calculate actual distance
						int64_t actualDistance = static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline)) -
						          static_cast<int64_t>(target);

						// Verify it's within 32-bit range
						if (actualDistance >= INT32_MIN && actualDistance <= INT32_MAX)
						{
							ModLoader::LogDebug(L"[Hooks]   [OK] Allocated trampoline in free region:");
							ModLoader::LogDebug(L"[Hooks]     Address:    0x%016llX",
								static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(trampoline)));
							ModLoader::LogDebug(L"[Hooks]     Distance:        %+lld bytes (%+.2f MB)",
								actualDistance, actualDistance / (1024.0 * 1024.0));
							ModLoader::LogDebug(L"[Hooks]     Regions checked: %d", regionsChecked);
							break;
						}
						else
						{
							// Somehow got address out of range, free it
							ModLoader::LogDebug(L"[Hooks]   Allocated out of range, freeing and continuing");
							VirtualFree(trampoline, 0, MEM_RELEASE);
							trampoline = nullptr;
						}
					}
				}
			}

			// Move to next region
			searchAddr = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;

			// Align to allocation granularity for next search
			if (searchAddr % allocGranularity != 0)
			{
				searchAddr = ((searchAddr / allocGranularity) + 1) * allocGranularity;
			}
		}

		ModLoader::LogDebug(L"[Hooks]   Memory scan complete: checked %d regions", regionsChecked);

		// Fallback: allocate anywhere if near allocation failed
		if (!trampoline)
		{
			ModLoader::LogMessage(L"[Hooks] WARN: Hook::Install: could not allocate trampoline near target after 1000 attempts");
			ModLoader::LogMessage(L"[Hooks] WARN:   Falling back to system-chosen address");
			ModLoader::LogMessage(L"[Hooks] WARN:   RIP-relative instructions will NOT work correctly!");

			trampoline = static_cast<uint8_t*>(
				VirtualAlloc(nullptr, trampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
		}

		if (!trampoline)
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: Hook::Install: VirtualAlloc for trampoline failed (error %lu)", GetLastError());
			return false;
		}

		uintptr_t trampolineAddr = reinterpret_cast<uintptr_t>(trampoline);
		int64_t finalDistance = static_cast<int64_t>(trampolineAddr) - static_cast<int64_t>(target);

		ModLoader::LogDebug(L"[Hooks] Hook::Install: trampoline final location:");
		ModLoader::LogDebug(L"[Hooks]   Address:  0x%016llX", static_cast<unsigned long long>(trampolineAddr));
		ModLoader::LogDebug(L"[Hooks]   Distance: %+lld bytes (%+.2f MB)",
			finalDistance, finalDistance / (1024.0 * 1024.0));
		ModLoader::LogDebug(L"[Hooks]   In range: %s",
			(finalDistance >= INT32_MIN && finalDistance <= INT32_MAX) ? L"YES" : L"NO");

		// === RIP-RELATIVE INSTRUCTION RELOCATION ===
		// Walk the original stolen bytes, writing relocated instructions into the
		// trampoline via a write cursor.  Short rel8 branches are expanded to rel32
		// so they still reach their targets when the trampoline is far from the origin.
		ModLoader::LogDebug(L"[Hooks] Hook::Install: relocating stolen bytes into trampoline...");

		bool canRelocate = (finalDistance >= INT32_MIN && finalDistance <= INT32_MAX);
		int relocatedCount = 0;

		// writeCursor tracks how many bytes have been written into the trampoline so far.
		// It grows beyond patchSize when short jumps are expanded.
		size_t writeCursor = 0;

		size_t offset = 0; // offset into originalBytes (the source)
		while (offset < patchSize)
		{
			const uint8_t* instrStart = originalBytes + offset;
			size_t instrLen = GetInstructionLength(instrStart);

			if (instrLen == 0 || instrLen > 15)
			{
				ModLoader::LogMessage(L"[Hooks] Hook::Install: relocation scan hit invalid instruction at offset %zu", offset);
				// Copy remaining bytes verbatim and bail
				size_t remaining = patchSize - offset;
				memcpy(trampoline + writeCursor, instrStart, remaining);
				writeCursor += remaining;
				offset += remaining;
				break;
			}

			// Don't process partial instructions
			if (offset + instrLen > patchSize)
			{
				// Partial — copy verbatim
				size_t remaining = patchSize - offset;
				memcpy(trampoline + writeCursor, instrStart, remaining);
				writeCursor += remaining;
				offset += remaining;
				break;
			}

			// Decode the instruction to classify it
			const uint8_t* p = instrStart;

			// Skip legacy prefixes
			while (p < instrStart + instrLen && IsLegacyPrefix(*p))
				p++;

			// Skip REX prefix
			if (p < instrStart + instrLen && IsRex(*p))
				p++;

			if (p >= instrStart + instrLen)
			{
				// Prefix-only? Copy verbatim
				memcpy(trampoline + writeCursor, instrStart, instrLen);
				writeCursor += instrLen;
				offset += instrLen;
				continue;
			}

			uint8_t opcode = *p++;

			// --- Case 1: EB rel8 — short JMP, expand to E9 rel32 ---
			if (opcode == 0xEB)
			{
				int8_t rel8 = static_cast<int8_t>(instrStart[instrLen - 1]);
				uintptr_t origInstrEnd = target + offset + instrLen;
				uintptr_t absTarget = origInstrEnd + rel8;

				ModLoader::LogDebug(L"[Hooks]   Found JMP rel8 at src offset +0x%zX: expanding to JMP rel32", offset);
				ModLoader::LogDebug(L"[Hooks]     Absolute target: 0x%016llX", static_cast<unsigned long long>(absTarget));

				if (!canRelocate)
				{
					ModLoader::LogDebug(L"[Hooks]     [FAIL] CANNOT RELOCATE: trampoline too far from original code!");
					memcpy(trampoline + writeCursor, instrStart, instrLen);
					writeCursor += instrLen;
				}
				else
				{
					// E9 rel32: 5 bytes
					uintptr_t newInstrEnd = trampolineAddr + writeCursor + 5;
					int64_t newDisp64 = static_cast<int64_t>(absTarget) - static_cast<int64_t>(newInstrEnd);
					if (newDisp64 < INT32_MIN || newDisp64 > INT32_MAX)
					{
						ModLoader::LogDebug(L"[Hooks]     [FAIL] RELOCATION FAILED: new displacement doesn't fit in 32 bits!");
						memcpy(trampoline + writeCursor, instrStart, instrLen);
						writeCursor += instrLen;
					}
					else
					{
						int32_t newDisp = static_cast<int32_t>(newDisp64);
						trampoline[writeCursor] = 0xE9;
						memcpy(trampoline + writeCursor + 1, &newDisp, 4);
						writeCursor += 5;
						relocatedCount++;
						ModLoader::LogDebug(L"[Hooks]     [OK] Expanded JMP rel8 -> JMP rel32 (new disp: 0x%08X)", static_cast<uint32_t>(newDisp));
					}
				}
				offset += instrLen;
				continue;
			}

			// --- Case 2: 7x rel8 — short Jcc, expand to 0F 8x rel32 ---
			if (opcode >= 0x70 && opcode <= 0x7F)
			{
				int8_t rel8 = static_cast<int8_t>(instrStart[instrLen - 1]);
				uintptr_t origInstrEnd = target + offset + instrLen;
				uintptr_t absTarget = origInstrEnd + rel8;
				uint8_t nearOpcode = 0x80 + (opcode - 0x70); // 70->80, 71->81, ... 7F->8F

				ModLoader::LogDebug(L"[Hooks]   Found Jcc rel8 (0x%02X) at src offset +0x%zX: expanding to Jcc rel32", opcode, offset);
				ModLoader::LogDebug(L"[Hooks]     Absolute target: 0x%016llX", static_cast<unsigned long long>(absTarget));

				if (!canRelocate)
				{
					ModLoader::LogDebug(L"[Hooks]     [FAIL] CANNOT RELOCATE: trampoline too far from original code!");
					memcpy(trampoline + writeCursor, instrStart, instrLen);
					writeCursor += instrLen;
				}
				else
				{
					// 0F 8x rel32: 6 bytes
					uintptr_t newInstrEnd = trampolineAddr + writeCursor + 6;
					int64_t newDisp64 = static_cast<int64_t>(absTarget) - static_cast<int64_t>(newInstrEnd);
					if (newDisp64 < INT32_MIN || newDisp64 > INT32_MAX)
					{
						ModLoader::LogDebug(L"[Hooks]     [FAIL] RELOCATION FAILED: new displacement doesn't fit in 32 bits!");
						memcpy(trampoline + writeCursor, instrStart, instrLen);
						writeCursor += instrLen;
					}
					else
					{
						int32_t newDisp = static_cast<int32_t>(newDisp64);
						trampoline[writeCursor + 0] = 0x0F;
						trampoline[writeCursor + 1] = nearOpcode;
						memcpy(trampoline + writeCursor + 2, &newDisp, 4);
						writeCursor += 6;
						relocatedCount++;
						ModLoader::LogDebug(L"[Hooks]     [OK] Expanded Jcc rel8 -> 0F 8%X rel32 (new disp: 0x%08X)", nearOpcode & 0x0F, static_cast<uint32_t>(newDisp));
					}
				}
				offset += instrLen;
				continue;
			}

			// --- Case 3: E8 CALL rel32, E9 JMP rel32 ---
			if (opcode == 0xE8 || opcode == 0xE9)
			{
				// Copy instruction verbatim first, then patch displacement in-place
				memcpy(trampoline + writeCursor, instrStart, instrLen);
				size_t dispOff = writeCursor + static_cast<size_t>(p - instrStart);
				const char* name = (opcode == 0xE8) ? "CALL rel32" : "JMP rel32";

				int32_t origDisp;
				memcpy(&origDisp, &trampoline[dispOff], sizeof(origDisp));
				uintptr_t origInstrEnd = target + offset + instrLen;
				uintptr_t absTarget = origInstrEnd + origDisp;

				ModLoader::LogDebug(L"[Hooks]   Found %S at src offset +0x%zX (instr len %zu):", name, offset, instrLen);
				ModLoader::LogDebug(L"[Hooks]     Original disp32: 0x%08X (%+d)", static_cast<uint32_t>(origDisp), origDisp);
				ModLoader::LogDebug(L"[Hooks]     Absolute target: 0x%016llX", static_cast<unsigned long long>(absTarget));

				if (canRelocate)
				{
					uintptr_t newInstrEnd = trampolineAddr + writeCursor + instrLen;
					int64_t newDisp64 = static_cast<int64_t>(absTarget) - static_cast<int64_t>(newInstrEnd);
					if (newDisp64 >= INT32_MIN && newDisp64 <= INT32_MAX)
					{
						int32_t newDisp = static_cast<int32_t>(newDisp64);
						memcpy(&trampoline[dispOff], &newDisp, sizeof(newDisp));
						relocatedCount++;
						ModLoader::LogDebug(L"[Hooks]     [OK] Relocated (new disp: 0x%08X)", static_cast<uint32_t>(newDisp));
					}
					else
					{
						ModLoader::LogDebug(L"[Hooks]     [FAIL] RELOCATION FAILED: new displacement doesn't fit in 32 bits!");
					}
				}
				else
				{
					ModLoader::LogDebug(L"[Hooks]     [FAIL] CANNOT RELOCATE: trampoline too far from original code!");
				}

				writeCursor += instrLen;
				offset += instrLen;
				continue;
			}

			// --- Case 4: 0F 8x — Jcc rel32 ---
			if (opcode == 0x0F && p < instrStart + instrLen)
			{
				uint8_t opcode2 = *p++;
				if (opcode2 >= 0x80 && opcode2 <= 0x8F)
				{
					memcpy(trampoline + writeCursor, instrStart, instrLen);
					size_t dispOff = writeCursor + static_cast<size_t>(p - instrStart);

					int32_t origDisp;
					memcpy(&origDisp, &trampoline[dispOff], sizeof(origDisp));
					uintptr_t origInstrEnd = target + offset + instrLen;
					uintptr_t absTarget = origInstrEnd + origDisp;

					ModLoader::LogDebug(L"[Hooks]   Found Jcc rel32 at src offset +0x%zX (instr len %zu):", offset, instrLen);
					ModLoader::LogDebug(L"[Hooks]     Original disp32: 0x%08X (%+d)", static_cast<uint32_t>(origDisp), origDisp);
					ModLoader::LogDebug(L"[Hooks]     Absolute target: 0x%016llX", static_cast<unsigned long long>(absTarget));

					if (canRelocate)
					{
						uintptr_t newInstrEnd = trampolineAddr + writeCursor + instrLen;
						int64_t newDisp64 = static_cast<int64_t>(absTarget) - static_cast<int64_t>(newInstrEnd);
						if (newDisp64 >= INT32_MIN && newDisp64 <= INT32_MAX)
						{
							int32_t newDisp = static_cast<int32_t>(newDisp64);
							memcpy(&trampoline[dispOff], &newDisp, sizeof(newDisp));
							relocatedCount++;
							ModLoader::LogDebug(L"[Hooks]     [OK] Relocated (new disp: 0x%08X)", static_cast<uint32_t>(newDisp));
						}
						else
						{
							ModLoader::LogDebug(L"[Hooks]     [FAIL] RELOCATION FAILED: new displacement doesn't fit in 32 bits!");
						}
					}
					else
					{
						ModLoader::LogDebug(L"[Hooks]     [FAIL] CANNOT RELOCATE: trampoline too far from original code!");
					}

					writeCursor += instrLen;
					offset += instrLen;
					continue;
				}
			}

			// --- Case 5: Any instruction with ModRM [RIP+disp32] addressing ---
			{
				memcpy(trampoline + writeCursor, instrStart, instrLen);

				// Re-walk to find the ModRM byte in the *copied* instruction
				const uint8_t* q = trampoline + writeCursor;
				while (q < trampoline + writeCursor + instrLen && IsLegacyPrefix(*q))
					q++;
				if (q < trampoline + writeCursor + instrLen && IsRex(*q))
					q++;
				if (q < trampoline + writeCursor + instrLen)
				{
					uint8_t op = *q++;
					if (op == 0x0F && q < trampoline + writeCursor + instrLen)
						q++; // skip second opcode byte
					if (q < trampoline + writeCursor + instrLen)
					{
						uint8_t modrm = *q;
						uint8_t mod = (modrm >> 6) & 0x03;
						uint8_t rm  = modrm & 0x07;
						if (mod == 0x00 && rm == 0x05)
						{
							// RIP-relative addressing
							size_t dispOff = writeCursor + static_cast<size_t>((q + 1) - (trampoline + writeCursor));

							int32_t origDisp;
							memcpy(&origDisp, &trampoline[dispOff], sizeof(origDisp));
							uintptr_t origInstrEnd = target + offset + instrLen;
							uintptr_t absTarget = origInstrEnd + origDisp;

							ModLoader::LogDebug(L"[Hooks]   Found RIP-relative [rip+disp32] at src offset +0x%zX:", offset);
							ModLoader::LogDebug(L"[Hooks]     Original disp32: 0x%08X (%+d)", static_cast<uint32_t>(origDisp), origDisp);
							ModLoader::LogDebug(L"[Hooks]     Absolute target: 0x%016llX", static_cast<unsigned long long>(absTarget));

							if (canRelocate)
							{
								uintptr_t newInstrEnd = trampolineAddr + writeCursor + instrLen;
								int64_t newDisp64 = static_cast<int64_t>(absTarget) - static_cast<int64_t>(newInstrEnd);
								if (newDisp64 >= INT32_MIN && newDisp64 <= INT32_MAX)
								{
									int32_t newDisp = static_cast<int32_t>(newDisp64);
									memcpy(&trampoline[dispOff], &newDisp, sizeof(newDisp));
									relocatedCount++;
									ModLoader::LogDebug(L"[Hooks]     [OK] Relocated (new disp: 0x%08X)", static_cast<uint32_t>(newDisp));
								}
								else
								{
									ModLoader::LogDebug(L"[Hooks]     [FAIL] RELOCATION FAILED: new displacement doesn't fit in 32 bits!");
								}
							}
							else
							{
								ModLoader::LogDebug(L"[Hooks]     [FAIL] CANNOT RELOCATE: trampoline too far from original code!");
							}
						}
					}
				}

				writeCursor += instrLen;
				offset += instrLen;
			}
		}

		if (relocatedCount > 0)
		{
			ModLoader::LogDebug(L"[Hooks] Hook::Install: relocated/expanded %d instruction(s) (%zu bytes written to trampoline)",
				relocatedCount, writeCursor);
		}
		else
		{
			ModLoader::LogDebug(L"[Hooks] Hook::Install: no instructions needed relocation (%zu bytes written to trampoline)", writeCursor);
		}

		// Write JMP back to (target + patchSize) after the relocated instructions
		uintptr_t returnAddr = target + patchSize;
		trampoline[writeCursor + 0] = 0xFF;
		trampoline[writeCursor + 1] = 0x25;
		trampoline[writeCursor + 2] = 0x00;
		trampoline[writeCursor + 3] = 0x00;
		trampoline[writeCursor + 4] = 0x00;
		trampoline[writeCursor + 5] = 0x00;
		memcpy(&trampoline[writeCursor + 6], &returnAddr, sizeof(returnAddr));

		ModLoader::LogDebug(L"[Hooks] Hook::Install: trampoline JMP back at offset +0x%zX to 0x%016llX",
			writeCursor, static_cast<unsigned long long>(returnAddr));

		// Log final trampoline bytes
		{
			size_t dumpLen = writeCursor + kJmpSize;
			char hexBuf[256]{};
			size_t pos = 0;
			for (size_t i = 0; i < dumpLen && pos + 3 <= sizeof(hexBuf) - 1; i++)
				pos += snprintf(hexBuf + pos, sizeof(hexBuf) - pos, "%02X ", trampoline[i]);
			ModLoader::LogDebug(L"[Hooks] Trampoline bytes (hex): %S", hexBuf);
		}

		// Flush instruction cache for trampoline
		FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineSize);

		// Give the caller a pointer to the trampoline so they can call the original
		*originalFunc = trampoline;
		ModLoader::LogDebug(L"[Hooks] Hook::Install: original function pointer set to trampoline at 0x%p",
			static_cast<void*>(trampoline));

		// Build the JMP patch for the target
		uint8_t jmpPatch[kJmpSize];
		jmpPatch[0] = 0xFF;
		jmpPatch[1] = 0x25;
		jmpPatch[2] = 0x00;
		jmpPatch[3] = 0x00;
		jmpPatch[4] = 0x00;
		jmpPatch[5] = 0x00;
		memcpy(&jmpPatch[6], &detour, sizeof(detour));

		// Write the patch
		ModLoader::LogDebug(L"[Hooks] Hook::Install: writing JMP patch at 0x%llX...",
			static_cast<unsigned long long>(target));
		if (!Patch(target, jmpPatch, kJmpSize))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: Hook::Install: failed to write JMP patch at 0x%llX",
				static_cast<unsigned long long>(target));
			VirtualFree(trampoline, 0, MEM_RELEASE);
			trampoline = nullptr;
			return false;
		}

		installed = true;

		ModLoader::LogMessage(L"[Hooks] Hook::Install: SUCCESS");
		ModLoader::LogMessage(L"[Hooks]   Target:       0x%llX", static_cast<unsigned long long>(target));
		ModLoader::LogMessage(L"[Hooks]   Detour:       0x%llX", static_cast<unsigned long long>(detour));
		ModLoader::LogMessage(L"[Hooks]   Trampoline:   0x%p", static_cast<void*>(trampoline));
		ModLoader::LogMessage(L"[Hooks] Stolen bytes: %zu", patchSize);

		return true;
	}

	void Hook::Remove()
	{
		if (!installed)
		{
			ModLoader::LogMessage(L"[Hooks] Hook::Remove: nothing to remove (not installed)");
			return;
		}

		ModLoader::LogMessage(L"[Hooks] Hook::Remove: restoring 0x%llX (%zu bytes)",
			static_cast<unsigned long long>(target), patchSize);

		// Restore original bytes
		Hooks::Patch(target, originalBytes, patchSize);

		// Free trampoline
		if (trampoline)
		{
			ModLoader::LogMessage(L"[Hooks] Hook::Remove: freeing trampoline at 0x%p", static_cast<void*>(trampoline));
			VirtualFree(trampoline, 0, MEM_RELEASE);
			trampoline = nullptr;
		}

		installed = false;
		ModLoader::LogMessage(L"[Hooks] Hook::Remove: hook at 0x%llX removed successfully",
			static_cast<unsigned long long>(target));
	}

	// ---------------------------------------------------------------------------
	// VTable Hook — patches a vtable entry to redirect virtual calls
	// ---------------------------------------------------------------------------

	bool VTableHook::Install(void* objectInstance, size_t vtableSlotIdx, void* detour, void** outOriginal)
	{
		if (!objectInstance)
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: VTableHook::Install: objectInstance is null");
			return false;
		}

		if (installed)
		{
			ModLoader::LogMessage(L"[Hooks] WARN: VTableHook::Install: already installed at vtable 0x%llX slot %zu",
				static_cast<unsigned long long>(vtableAddr), slotIndex);
			return false;
		}

		// Read the vtable pointer from the object (first 8 bytes)
		uintptr_t vtable = *reinterpret_cast<uintptr_t*>(objectInstance);

		if (!vtable)
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: VTableHook::Install: object has null vtable pointer");
			return false;
		}

		ModLoader::LogDebug(L"[Hooks] VTableHook::Install: object=0x%llX vtable=0x%llX slot=%zu",
			static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(objectInstance)),
			static_cast<unsigned long long>(vtable),
			vtableSlotIdx);

		return InstallByVTableAddr(vtable, vtableSlotIdx, detour, outOriginal);
	}

	bool VTableHook::InstallByVTableAddr(uintptr_t vtableAddress, size_t vtableSlotIdx, void* detour, void** outOriginal)
	{
		if (installed)
		{
			ModLoader::LogMessage(L"[Hooks] WARN: VTableHook::InstallByVTableAddr: already installed");
			return false;
		}

		vtableAddr = vtableAddress;
		slotIndex = vtableSlotIdx;

		// Calculate the address of the vtable entry
		uintptr_t slotAddr = vtableAddr + (slotIndex * sizeof(uintptr_t));

		ModLoader::LogDebug(L"[Hooks] VTableHook: vtable=0x%llX slot[%zu]=0x%llX",
			static_cast<unsigned long long>(vtableAddr),
			slotIndex,
			static_cast<unsigned long long>(slotAddr));

		// Read the original function pointer from the vtable slot
		if (!ReadMemory(slotAddr, &originalFunc, sizeof(originalFunc)))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: VTableHook: failed to read vtable slot at 0x%llX",
				static_cast<unsigned long long>(slotAddr));
			return false;
		}

		ModLoader::LogDebug(L"[Hooks] VTableHook: original function at slot[%zu] = 0x%llX",
			slotIndex, static_cast<unsigned long long>(originalFunc));

		// Give the caller the original function pointer so they can call through
		*outOriginal = reinterpret_cast<void*>(originalFunc);

		// Write our detour function pointer into the vtable slot
		uintptr_t detourAddr = reinterpret_cast<uintptr_t>(detour);
		if (!Patch(slotAddr, reinterpret_cast<const uint8_t*>(&detourAddr), sizeof(detourAddr)))
		{
			ModLoader::LogMessage(L"[Hooks] ERROR: VTableHook: failed to patch vtable slot at 0x%llX",
				static_cast<unsigned long long>(slotAddr));
			return false;
		}

		installed = true;

		ModLoader::LogMessage(L"[Hooks] VTableHook::Install: SUCCESS");
		ModLoader::LogMessage(L"[Hooks]   VTable:    0x%llX", static_cast<unsigned long long>(vtableAddr));
		ModLoader::LogMessage(L"[Hooks]   Slot:    %zu", slotIndex);
		ModLoader::LogMessage(L"[Hooks]   Original:  0x%llX", static_cast<unsigned long long>(originalFunc));
		ModLoader::LogMessage(L"[Hooks]   Detour:    0x%llX", static_cast<unsigned long long>(detourAddr));

		return true;
	}

	void VTableHook::Remove()
	{
		if (!installed)
		{
			ModLoader::LogMessage(L"[Hooks] VTableHook::Remove: nothing to remove (not installed)");
			return;
		}

		// Restore the original function pointer in the vtable slot
		uintptr_t slotAddr = vtableAddr + (slotIndex * sizeof(uintptr_t));

		ModLoader::LogMessage(L"[Hooks] VTableHook::Remove: restoring slot[%zu] at 0x%llX to original 0x%llX",
			slotIndex,
			static_cast<unsigned long long>(slotAddr),
			static_cast<unsigned long long>(originalFunc));

		Patch(slotAddr, reinterpret_cast<const uint8_t*>(&originalFunc), sizeof(originalFunc));

		installed = false;
		ModLoader::LogMessage(L"[Hooks] VTableHook::Remove: vtable hook removed successfully");
	}

}
