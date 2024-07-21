// This file is part of Notepad++ project
// Copyright (C)2024 Raattis <raattis@flyware.fi>

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// at your option any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "FastUnmatch.h"

#include "FindReplaceDlg.h" // FindOption
#include "Utf8_16.h" // determineEncoding
#include "EncodingMapper.h"
#include <windows.h>
#include "uchardet.h"

#include <stdio.h> // NOCOMMIT

namespace detail
{
inline static bool contains(const uint8_t* __restrict haystack, size_t len, const std::vector<uint8_t>& needle)
{
	const uint8_t* last = haystack + len - needle.size();
	const uint8_t* __restrict ptr = needle.data() + 1;
	const size_t ptrLen = needle.size() - 1;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (0 != memcmp(haystack, ptr, ptrLen))
			continue;

		return true;
	}
	return false;
}

inline static bool containsWide(const uint16_t* __restrict haystack, size_t len, const std::vector<uint16_t>& needle)
{
	const uint16_t* last = haystack + len - needle.size();
	const uint16_t* __restrict ptr = needle.data() + 1;
	const size_t ptrLen = needle.size() - 1;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (0 != memcmp(haystack, ptr, ptrLen))
			continue;

		return true;
	}
	return false;
}

inline static bool containsCaseInsensitive(const uint8_t* haystack, size_t len, const std::vector<uint8_t>& lower, const std::vector<uint8_t>& upper)
{
	const uint8_t lower0 = lower[0];
	const uint8_t upper0 = upper[0];

	const size_t needleLen = lower.size();
	const uint8_t* last = haystack + len - needleLen;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != lower0 && haystack[0] != upper0)
		{
			continue;
		}

		bool no_match = false;
		for (size_t i = 1; i < needleLen; ++i)
		{
			if (haystack[i] == lower[i] || haystack[i] == upper[i])
				continue;

			no_match = true;
			break;
		}
		if (no_match)
			continue;

		return true;
	}
	return false;
}

inline static bool containsCaseInsensitiveWide(const uint16_t* haystack, size_t len, const std::vector<uint16_t>& lower, const std::vector<uint16_t>& upper)
{
	const uint16_t lower0 = lower[0];
	const uint16_t upper0 = upper[0];

	const size_t needleLen = lower.size();
	const uint16_t* last = haystack + len - needleLen;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != lower0 && haystack[0] != upper0)
			continue;

		bool no_match = false;
		for (size_t i = 1; i < needleLen; ++i)
		{
			if (haystack[i] == lower[i] || haystack[i] == upper[i])
				continue;

			no_match = true;
			break;
		}

		if (no_match)
			continue;

		return true;
	}
	return false;
}

/*
static int detectCodepage(const uint8_t* buf, size_t len)
{
	int codepage = -1;
	uchardet_t ud = uchardet_new();
	uchardet_handle_data(ud, reinterpret_cast<const char*>(buf), len);
	uchardet_data_end(ud);
	const char* cs = uchardet_get_charset(ud);
	if (stricmp(cs, "TIS-620") != 0) // TIS-620 detection is disabled here because uchardet detects usually wrongly UTF-8 as TIS-620
		codepage = EncodingMapper::getInstance().getEncodingFromString(cs);
	uchardet_delete(ud);
	return codepage;
}
*/

/*
static bool wcontains(const TCHAR* haystack, size_t len, const TCHAR* needle, size_t needle_len)
{
	const TCHAR* last = haystack + len - needle_len;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (0 != wcsncmp(haystack, needle, needle_len))
			continue;

		return true;
	}
	return false;
}

static bool wcontainsCaseInsensitive(const TCHAR* haystack, size_t len, const TCHAR* needle, size_t needle_len)
{
	const TCHAR* last = haystack + len - needle_len;
	for (; haystack <= last; ++haystack)
	{
		if (0 != wcsnicmp(haystack, needle, needle_len))
			continue;

		return true;
	}
	return false;
}*/
}

#define DEBUG_LOGF(p_fmt, ...) do { char buffer ## __LINE__[4096]; sprintf_s(buffer ## __LINE__, sizeof(buffer ## __LINE__), p_fmt "\n", __VA_ARGS__); OutputDebugStringA(buffer ## __LINE__); } while(false)
//#define DEBUG_LOGF(p_fmt, ...) do {} while(false)

constexpr int upperLimitSearchTermLength = 2048;

FastUnmatch::FastUnmatch(size_t filesCount, const FindOption& findOptions)
	: enabled{ false }
	, matchCase{ findOptions._isMatchCase }
{
	const int searchTermLength = (int)findOptions._str2Search.length();
	const int minSearchTermLength = 1;
	const size_t minFileCount = 1; // NOCOMMIT 100;

	if (filesCount < minFileCount)
		return;

	std::wstring baseSearchString;

	if (findOptions._searchType == FindExtended)
	{
		baseSearchString.resize(searchTermLength);
		Searching::convertExtendedToString(findOptions._str2Search.c_str(), baseSearchString.data(), searchTermLength);

		enabled = false; return; // NOCOMMIT
	}
	else if (findOptions._searchType == FindNormal)
	{
		baseSearchString = findOptions._str2Search;
	}
	else if (findOptions._searchType == FindRegex)
	{
		enabled = false; return; // NOCOMMIT

		// TODO: Check if the regex search term contains a simple string and do unmatching using that or alternatively
		// TODO: Save a regex to the context and use it for matching.
	}

	if (baseSearchString.length() < minSearchTermLength)
		return;

	enabled = true;

	{
		searchTermsWideLE.assign(baseSearchString.begin(), baseSearchString.end());
		std::vector<uint16_t>& bigEndian = searchTermsWideBE;
		for (uint16_t word : baseSearchString)
		{
			bigEndian.emplace_back() = (word >> 8) | (word << 8);
		}
	}

	auto convertCodePage = [&](int codePage, const std::wstring& baseSearchString, std::vector<uint8_t>& out) -> bool
	{
		int neededSize = WideCharToMultiByte(codePage, 0, baseSearchString.c_str(), (int)baseSearchString.size(), NULL, 0, NULL, NULL);
		if (neededSize <= 0)
			return false;

		std::vector<uint8_t> multiByteSearchString;
		multiByteSearchString.resize(neededSize, 0);
		WideCharToMultiByte(codePage, 0, baseSearchString.c_str(), (int)baseSearchString.size(), (char*)multiByteSearchString.data(), neededSize, NULL, NULL);
		if (multiByteSearchString.empty())
			return false;

		out =  std::move(multiByteSearchString);
		return true;
	};

	std::wstring lower = baseSearchString;
	std::wstring upper = baseSearchString;
	if (!matchCase)
	{
		CharLowerW(lower.data());
		CharUpperW(upper.data());
		if (upper == lower)
		{
			matchCase = true;
		}
	}

	if (matchCase)
	{
		auto addCodePage = [&](int codePage) -> bool
		{
			std::vector<uint8_t> multiByteSearchString;
			if (!convertCodePage(codePage, baseSearchString, multiByteSearchString))
				return false;

			for (const auto& [codepage, searchTerm] : searchTerms)
			{
				if (searchTerm == multiByteSearchString)
					return false;
			}
			searchTerms.emplace_back(codePage, std::move(multiByteSearchString));
			return true;
		};

		[[maybe_unused]] bool utf8Added = addCodePage(CP_UTF8);
		assert(utf8Added && "Couldn't convert UTF-16 search term to UTF-8.");

		for (int i = 0; i < 128; ++i)
		{
			int codePage = EncodingMapper::getInstance().getEncodingFromIndex(i);
			if (codePage == -1)
				continue;

			addCodePage(codePage);
		}
	}
	else
	{
		searchTermsWideCaseInsensitiveLE.lower.assign(lower.begin(), lower.end());
		searchTermsWideCaseInsensitiveLE.upper.assign(upper.begin(), upper.end());
		for (uint16_t wc : lower)
		{
			searchTermsWideCaseInsensitiveBE.lower.emplace_back() = (wc >> 8) | (wc << 8);
		}
		for (uint16_t wc : upper)
		{
			searchTermsWideCaseInsensitiveBE.lower.emplace_back() = (wc >> 8) | (wc << 8);
		}

		auto addCodePageCaseInsensitive = [&](int codePage)
			{
				std::vector<uint8_t> resultLower;
				std::vector<uint8_t> resultUpper;

				int neededSize = WideCharToMultiByte(codePage, 0, lower.c_str(), (int)lower.size(), NULL, 0, NULL, NULL);
				if (neededSize > 0)
				{
					resultLower.resize(neededSize, 0);
					WideCharToMultiByte(codePage, 0, lower.c_str(), (int)lower.size(), (char*)resultLower.data(), neededSize, NULL, NULL);
				}

				neededSize = WideCharToMultiByte(codePage, 0, upper.c_str(), (int)upper.size(), NULL, 0, NULL, NULL);
				if (neededSize > 0)
				{
					resultUpper.resize(neededSize, 0);
					WideCharToMultiByte(codePage, 0, upper.c_str(), (int)upper.size(), (char*)resultUpper.data(), neededSize, NULL, NULL);
				}

				size_t shorter = resultLower.size() < resultUpper.size() ? resultLower.size() : resultUpper.size();
				if (shorter == 0)
				{
					return false;
				}

				resultLower.resize(shorter);
				resultUpper.resize(shorter);

				if (resultLower == resultUpper)
				{
					for (const auto& [codepage, searchTerm] : searchTerms)
					{
						if (searchTerm == resultLower)
							return false;
					}
					searchTerms.emplace_back(codePage, std::move(resultLower));
				}
				else
				{
					for (const auto& [codepage, searchTerm] : searchTermsCaseInsensitive)
					{
						if (searchTerm.lower == resultLower && searchTerm.upper == resultUpper)
							return false;
					}
					searchTermsCaseInsensitive.emplace_back(codePage, UpperAndLower8{ std::move(resultLower), std::move(resultUpper) });
				}
				return true;
			};

		bool utf8Added = addCodePageCaseInsensitive(CP_UTF8);
		if (!utf8Added)
		{
			assert(utf8Added && "Couldn't convert UTF-16 search term to case insensitive UTF-8.");
			enabled = false;
			return;
		}

		for (int i = 0; i < 128; ++i)
		{
			int codePage = EncodingMapper::getInstance().getEncodingFromIndex(i);
			if (codePage == -1)
				continue;

			addCodePageCaseInsensitive(codePage);
		}
	}
}

bool FastUnmatch::doesMatch([[maybe_unused]] const TCHAR* filename, const uint8_t* fileContents, size_t fileSize, uint32_t unimode) const
{
	switch (unimode)
	{
	case uni8Bit: [[fallthrough]]; // ANSI
	case uniUTF8: [[fallthrough]]; // UTF-8 with BOM
	case uniCookie: [[fallthrough]]; // UTF-8 without BOM
	case uni7Bit:
		for (const auto& [codepage, searchTerm] : searchTerms)
		{
			if (!detail::contains(fileContents, fileSize, searchTerm))
				continue;

			DEBUG_LOGF("Matched '%S' codepage: %d, unimode: %d, line:%d", filename, codepage, unimode, __LINE__);
			return true;
		}
		for (const auto& [codepage, searchTermCaseInsensitive] : searchTermsCaseInsensitive)
		{
			if (!detail::containsCaseInsensitive(fileContents, fileSize, searchTermCaseInsensitive.lower, searchTermCaseInsensitive.upper))
				continue;

			DEBUG_LOGF("Matched '%S' codepage: %d, unimode: %d, line:%d", filename, codepage, unimode, __LINE__);
			return true;
		}
		break;
	case uni16BE: [[fallthrough]]; // UTF-16 Big Endian with BOM
	case uni16BE_NoBOM: // UTF-16 Big Endian without BOM
		if (matchCase && !detail::containsWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideBE))
			return true;
		if (!matchCase && !detail::containsCaseInsensitiveWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideCaseInsensitiveBE.lower, searchTermsWideCaseInsensitiveBE.upper))
			return true;
		DEBUG_LOGF("Couldn't unmatch '%S' %d, unimode: %d", filename, unimode, __LINE__);
		break;
	case uni16LE: [[fallthrough]]; // UTF-16 Little Endian with BOM
	case uni16LE_NoBOM: // UTF-16 Little Endian without BOM
		if (matchCase && !detail::containsWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideLE))
			return true;
		if (!matchCase && !detail::containsCaseInsensitiveWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideCaseInsensitiveLE.lower, searchTermsWideCaseInsensitiveLE.upper))
			return true;
		break;
	case uniEnd:
		break;
	};

	return false;
}

bool FastUnmatch::fileDoesNotContainString(const TCHAR* filename) const
{
	HANDLE hFile = ::CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	DWORD fileSize = GetFileSize(hFile, NULL);
	if (fileSize == INVALID_FILE_SIZE)
		return false;

	HANDLE hMapping = ::CreateFileMapping(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
	if (hMapping == NULL)
	{
		CloseHandle(hFile);
		return false;
	}

	const uint8_t* fileContents = static_cast<const uint8_t*>(::MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
	if (fileContents == NULL)
	{
		CloseHandle(hMapping);
		CloseHandle(hFile);
		return false;
	}

	UniMode unimode = Utf8_16_Read::determineEncoding(fileContents, fileSize);

	bool not_found = true;
	const size_t maxChunkSize = 4096;
	for (size_t start = 0; start < fileSize; start += maxChunkSize)
	{
		size_t length = std::min(fileSize - start, maxChunkSize);
		if (doesMatch(filename, fileContents, length, unimode))
		{
			not_found = false;
			break;
		}
	}

	UnmapViewOfFile(fileContents);
	CloseHandle(hMapping);
	CloseHandle(hFile);
	return not_found;
}
