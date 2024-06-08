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

namespace detail
{
static bool contains(const uint8_t* __restrict haystack, size_t len, const uint8_t* __restrict needle, size_t needle_len)
{
	const uint8_t* last = haystack + len - needle_len;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (len <= 1)
			return true;

		if (haystack[1] != needle[1] || 0 != memcmp(haystack, needle, needle_len))
			continue;

		return true;
	}
	return false;
}

static bool containsCaseInsensitive(const char* haystack, size_t len, const char* needle, size_t needle_len)
{
	const char* last = haystack + len - needle_len;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (0 != strnicmp(haystack, needle, needle_len))
			continue;

		return true;
	}
	return false;
}
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
		std::vector<uint8_t>& littleEndian = searchTerms.emplace_back(baseSearchString.length() * sizeof(baseSearchString[0]), (uint8_t)0);
		memcpy_s(littleEndian.data(), littleEndian.size(), baseSearchString.data(), baseSearchString.length() * sizeof(baseSearchString[0]));

		std::vector<uint8_t>& bigEndian = searchTerms.emplace_back();
		for (int i = 0; i < baseSearchString.length(); ++i)
		{
			// Big endian
			const uint8_t* bytes = reinterpret_cast<const uint8_t*>(baseSearchString.data());
			bigEndian.emplace_back() = bytes[i * 2 + 1];
			bigEndian.emplace_back() = bytes[i * 2 + 0];
		}
	}

	for (int i = 0; i < 128; ++i)
	{
		int codePage = EncodingMapper::getInstance().getEncodingFromIndex(i);
		if (codePage == -1)
			continue;

		int neededSize = WideCharToMultiByte(codePage, 0, baseSearchString.c_str(), (int)baseSearchString.size(), NULL, 0, NULL, NULL);
		if (neededSize <= 0)
			continue;

		std::vector<uint8_t> multiByteSearchString;
		multiByteSearchString.resize(neededSize, 0);
		WideCharToMultiByte(codePage, 0, baseSearchString.c_str(), (int)baseSearchString.size(), (char*)multiByteSearchString.data(), neededSize, NULL, NULL);
		if (multiByteSearchString.empty())
			continue;

		if (std::find(searchTerms.begin(), searchTerms.end(), multiByteSearchString) != searchTerms.end())
			continue;

		searchTerms.emplace_back(std::move(multiByteSearchString));
	}

	for (const std::vector<uint8_t>& s : searchTerms)
	{
		if (maxSearchTermLength < s.size())
			maxSearchTermLength = s.size();
	}

	if (maxSearchTermLength > upperLimitSearchTermLength)
		enabled = false;
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

	bool not_found = true;
	if (matchCase)
	{
		int i = 0;
		const int stride = 4096;
		for (; i + stride + maxSearchTermLength < fileSize; i += stride)
		{
			for (const std::vector<uint8_t>& searchTerm : searchTerms)
			{
				if (!detail::contains(fileContents + i, stride, searchTerm.data(), searchTerm.size()))
					continue;

				not_found = false;
				break;
			}
		}

		for (const std::vector<uint8_t>& searchTerm : searchTerms)
		{
			if (!detail::contains(fileContents + i, static_cast<size_t>(fileSize) - i, searchTerm.data(), searchTerm.size()))
				continue;

			not_found = false;
			break;
		}
	}
	else
	{
		UniMode enc = Utf8_16_Read::determineEncoding(fileContents, fileSize);
		if (enc == uniUTF8)
		{
			for (const std::vector<uint8_t>& searchTerm : searchTerms)
			{
				if (!detail::containsCaseInsensitive((const char*)fileContents, fileSize, (const char*)searchTerm.data(), searchTerm.size()))
					continue;

				not_found = false;
				break;
			}
		}
		else
		{
			not_found = false;
		}
	}

	UnmapViewOfFile(fileContents);
	CloseHandle(hMapping);
	CloseHandle(hFile);

	return not_found;
}
