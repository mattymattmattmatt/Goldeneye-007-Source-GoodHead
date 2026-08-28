#pragma once
#include <Windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <vector>
#include <sstream>
#include <string>

class SigScanner
{
public:
	static std::vector<int> ParsePattern(const std::string &signature)
	{
		std::vector<int> pattern;
		std::stringstream ss(signature);
		std::string sigByte;
		while (ss >> sigByte)
		{
			if (sigByte == "?" || sigByte == "??")
				pattern.push_back(-1);
			else
				pattern.push_back(static_cast<int>(strtoul(sigByte.c_str(), nullptr, 16)));
		}
		return pattern;
	}

	static bool PatternMatches(const uint8_t *bytes, int offset, const std::vector<int> &pattern)
	{
		for (size_t i = 0; i < pattern.size(); ++i)
		{
			if (pattern[i] != -1 && bytes[offset + i] != pattern[i])
				return false;
		}
		return true;
	}

	// Returns 0 if current offset matches, -1 if no matches found.
	// A value > 0 is the new offset.
	static int VerifyOffset(std::string moduleName, int currentOffset, std::string signature, int sigOffset = 0)
	{
		HMODULE hModule = GetModuleHandleA(moduleName.c_str());
		if (!hModule)
			return -1;

		MODULEINFO moduleInfo{};
		if (!GetModuleInformation(GetCurrentProcess(), hModule, &moduleInfo, sizeof(moduleInfo)))
			return -1;

		auto *bytes = static_cast<uint8_t *>(moduleInfo.lpBaseOfDll);
		auto pattern = ParsePattern(signature);
		if (pattern.empty())
			return -1;

		const int patternLen = static_cast<int>(pattern.size());

		if (currentOffset > 0 &&
			(currentOffset - sigOffset) >= 0 &&
			(currentOffset - sigOffset + patternLen) <= static_cast<int>(moduleInfo.SizeOfImage) &&
			PatternMatches(bytes, currentOffset - sigOffset, pattern))
		{
			return 0;
		}

		const int last = static_cast<int>(moduleInfo.SizeOfImage) - patternLen;
		for (int i = 0; i < last; ++i)
		{
			if (PatternMatches(bytes, i, pattern))
				return i + sigOffset;
		}
		return -1;
	}

	static int ScanFirst(const std::string &moduleName, const std::vector<std::string> &signatures, int sigOffset = 0)
	{
		for (const auto &sig : signatures)
		{
			int result = VerifyOffset(moduleName, 0, sig, sigOffset);
			if (result > 0)
				return result;
			if (result == 0)
				return sigOffset;
		}
		return -1;
	}
};
