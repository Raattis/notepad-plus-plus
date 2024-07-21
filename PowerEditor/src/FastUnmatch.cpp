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
inline static bool matches(const uint8_t* __restrict haystack, size_t len, const std::vector<uint8_t>& needle)
{
	if (len < needle.size())
		return false;

	for (size_t i = 0; i < needle.size(); ++i)
	{
		if (haystack[i] == needle[i])
			continue;

		return false;
	}
	return true;
}

inline static bool matchesWide(const uint16_t* __restrict haystack, [[maybe_unused]] size_t len, const std::vector<uint16_t>& needle)
{
	if (len < needle.size())
		return false;

	for (size_t i = 0; i < needle.size(); ++i)
	{
		if (haystack[i] == needle[i])
			continue;

		return false;
	}
	return true;
}

inline static bool matchesCaseInsensitive(const uint8_t* haystack, [[maybe_unused]] size_t len, const std::vector<uint8_t>& lower, const std::vector<uint8_t>& upper)
{
	if (len < lower.size())
		return false;

	for (size_t i = 0; i < lower.size(); ++i)
	{
		if (haystack[i] == lower[i] || haystack[i] == upper[i])
			continue;

		return false;
	}
	return true;
}

inline static bool matchesCaseInsensitiveWide(const uint16_t* haystack, [[maybe_unused]] size_t len, const std::vector<uint16_t>& lower, const std::vector<uint16_t>& upper)
{
	if (len < lower.size())
		return false;

	for (size_t i = 0; i < lower.size(); ++i)
	{
		if (haystack[i] == lower[i] || haystack[i] == upper[i])
			continue;

		return false;
	}
	return true;
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

	static volatile bool disableTweak = false;
	if (disableTweak)
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
		enabled = false;
		return;
		// TODO: Check if the regex search term contains a simple string and do unmatching using that or alternatively
		// TODO: Save a regex to the context and use it for matching.
	}

	if (baseSearchString.length() < minSearchTermLength)
		return;

	enabled = true;

	if (matchCase)
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
			if (!enabled)
				return false;

			std::vector<uint8_t> multiByteSearchString;
			if (!convertCodePage(codePage, baseSearchString, multiByteSearchString))
				return false;

			for (const auto& searchTerm : searchTerms)
			{
				if (searchTerm == multiByteSearchString)
					return false;
			}
			searchTerms.emplace_back(std::move(multiByteSearchString));
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
			searchTermsWideCaseInsensitiveBE.upper.emplace_back() = (wc >> 8) | (wc << 8);
		}

		auto addCodePageCaseInsensitive = [&](int codePage)
			{
				std::vector<uint8_t> resultLower;
				std::vector<uint8_t> resultUpper;
				if (!convertCodePage(codePage, lower, resultLower))
					return false;
				if (!convertCodePage(codePage, upper, resultUpper))
					return false;

				size_t shorterLength = resultLower.size() < resultUpper.size() ? resultLower.size() : resultUpper.size();
				if (shorterLength == 0)
				{
					return false;
				}

				if (shorterLength == 1)
				{
					// All valid search terms must be at least 2 bytes to enable fast unmatching
					enabled = false;
					return false;
				}

				resultLower.resize(shorterLength);
				resultUpper.resize(shorterLength);

				if (resultLower == resultUpper)
				{
					for (const auto& searchTerm : searchTerms)
					{
						if (searchTerm == resultLower)
							return false;
					}
					searchTerms.emplace_back(std::move(resultLower));
				}
				else
				{
					for (const auto& searchTerm : searchTermsCaseInsensitive)
					{
						if (searchTerm.lower == resultLower && searchTerm.upper == resultUpper)
							return false;
					}
					searchTermsCaseInsensitive.emplace_back(UpperAndLower8{ std::move(resultLower), std::move(resultUpper) });
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

	auto insertUniqueTwoBytes = [&](const void* twoBytesPtr)
	{
		const uint16_t twoBytes = *(const uint16_t*)twoBytesPtr;
		if (std::find(firstTwoBytes.begin(), firstTwoBytes.end(), twoBytes) == firstTwoBytes.end())
			firstTwoBytes.emplace_back(twoBytes);
	};
	
	auto insertUniqueFourBytes = [&](const void* fourBytesPtr)
	{
		const uint32_t fourBytes = *(const uint32_t*)fourBytesPtr;
		if (std::find(firstFourBytes.begin(), firstFourBytes.end(), fourBytes) == firstFourBytes.end())
			firstFourBytes.emplace_back(fourBytes);
	};

	for (const auto& searchTerm : searchTerms)
	{
		if (searchTerm.size() >= 4)
			insertUniqueFourBytes(searchTerm.data());
		else
			insertUniqueTwoBytes(searchTerm.data());
	}

	for (const auto& searchTermCaseInsensitive : searchTermsCaseInsensitive)
	{
		const auto& lower = searchTermCaseInsensitive.lower;
		const auto& upper = searchTermCaseInsensitive.upper;
		uint16_t twoBytes;
		twoBytes = (uint16_t)lower[0] + ((uint16_t)lower[1] << 8); insertUniqueTwoBytes(&twoBytes);
		twoBytes = (uint16_t)lower[0] + ((uint16_t)upper[1] << 8); insertUniqueTwoBytes(&twoBytes);
		twoBytes = (uint16_t)upper[0] + ((uint16_t)lower[1] << 8); insertUniqueTwoBytes(&twoBytes);
		twoBytes = (uint16_t)upper[0] + ((uint16_t)upper[1] << 8); insertUniqueTwoBytes(&twoBytes);
	}
	
	if (searchTermsWideBE.size() > 0)
	{
		if (searchTermsWideBE.size() >= 2)
		{
			insertUniqueFourBytes(searchTermsWideBE.data());
			insertUniqueFourBytes(searchTermsWideLE.data());
		}
		else
		{
			insertUniqueTwoBytes(searchTermsWideBE.data());
			insertUniqueTwoBytes(searchTermsWideLE.data());
		}
	}
	else
	{
		if (searchTermsWideCaseInsensitiveBE.lower.size() >= 2)
		{
			uint32_t fourBytes;
			fourBytes = searchTermsWideCaseInsensitiveLE.lower[0] + (searchTermsWideCaseInsensitiveLE.lower[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveLE.lower[0] + (searchTermsWideCaseInsensitiveLE.upper[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveLE.upper[0] + (searchTermsWideCaseInsensitiveLE.lower[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveLE.upper[0] + (searchTermsWideCaseInsensitiveLE.upper[1] << 16);
			insertUniqueFourBytes(&fourBytes);

			fourBytes = searchTermsWideCaseInsensitiveBE.lower[0] + (searchTermsWideCaseInsensitiveBE.lower[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveBE.lower[0] + (searchTermsWideCaseInsensitiveBE.upper[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveBE.upper[0] + (searchTermsWideCaseInsensitiveBE.lower[1] << 16);
			insertUniqueFourBytes(&fourBytes);
			fourBytes = searchTermsWideCaseInsensitiveBE.upper[0] + (searchTermsWideCaseInsensitiveBE.upper[1] << 16);
			insertUniqueFourBytes(&fourBytes);
		}
		else
		{
			insertUniqueTwoBytes(searchTermsWideCaseInsensitiveLE.lower.data());
			insertUniqueTwoBytes(searchTermsWideCaseInsensitiveLE.upper.data());
			insertUniqueTwoBytes(searchTermsWideCaseInsensitiveBE.lower.data());
			insertUniqueTwoBytes(searchTermsWideCaseInsensitiveBE.upper.data());
		}
	}
}

bool FastUnmatch::doesMatch([[maybe_unused]] const TCHAR* filename, const uint8_t* fileContents, size_t fileSize) const
{
	for (const auto& searchTerm : searchTerms)
	{
		if (detail::matches(fileContents, fileSize, searchTerm))
			return true;
	}

	for (const auto& searchTermCaseInsensitive : searchTermsCaseInsensitive)
	{
		if (detail::matchesCaseInsensitive(fileContents, fileSize, searchTermCaseInsensitive.lower, searchTermCaseInsensitive.upper))
			return true;
	}

	if (matchCase)
	{
		if (detail::matchesWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideBE)
			|| detail::matchesWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideLE))
			return true;
	}
	else
	{
		if (detail::matchesCaseInsensitiveWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideCaseInsensitiveBE.lower, searchTermsWideCaseInsensitiveBE.upper)
			|| detail::matchesCaseInsensitiveWide(reinterpret_cast<const uint16_t*>(fileContents), fileSize / 2, searchTermsWideCaseInsensitiveLE.lower, searchTermsWideCaseInsensitiveLE.upper))
			return true;
	}
	return false;
}

bool FastUnmatch::fileDoesNotContainString(const TCHAR* filename) const
{
	HANDLE hFile = ::CreateFile(filename, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_READONLY, NULL);
	if (hFile == INVALID_HANDLE_VALUE)
		return false;

	const DWORD fileSize = GetFileSize(hFile, NULL);
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

	bool not_found = true;
	static volatile size_t maxChunkSizeTweak = 4096;
	const size_t maxChunkSize = (size_t)maxChunkSizeTweak;
	size_t head = 0;
	while (head < fileSize)
	{
		const uint8_t* __restrict f = fileContents;

		bool found = false;
		for ( ; head < fileSize; head += maxChunkSize)
		{
			for (const uint16_t twoBytes : firstTwoBytes)
			{
				for (size_t i = 0; head + i + 1 < fileSize && i < maxChunkSize; ++i)
				{
					if (*(uint16_t*)(f + head + i) != twoBytes)
						continue;

					head += i;
					found = true;
					break;
				}
				if (found)
					break;
			}
			if (found)
				break;
			for (const uint32_t fourBytes : firstFourBytes)
			{
				for (size_t i = 0; head + i + 3 < fileSize && i < maxChunkSize; ++i)
				{
					if (*(uint32_t*)(f + head + i) != fourBytes)
						continue;

					head += i;
					found = true;
					break;
				}
				if (found)
					break;
			}
			if (found)
				break;
		}

		if (!found)
			break;

		size_t length = std::min(fileSize - head, maxChunkSize);
		if (length == 0)
			break;

		DEBUG_LOGF("Does %d MB file '%S' match?", fileSize / 1024 / 1024, filename);

		if (doesMatch(filename, fileContents + head, length))
		{
			DEBUG_LOGF("-----> yes %d MB file '%S' does match! <---------------------------------", fileSize / 1024 / 1024, filename);

			not_found = false;
			hits += 1;
			break;
		}

		head += 1;
	}

	UnmapViewOfFile(fileContents);
	CloseHandle(hMapping);
	CloseHandle(hFile);


	return not_found;
}
