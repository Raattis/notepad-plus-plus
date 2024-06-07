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
#include <windows.h>

namespace detail
{
static bool contains(const char* haystack, size_t len, const char* needle, size_t needle_len)
{
	const char* last = haystack + len - needle_len;
	for (; haystack <= last; ++haystack)
	{
		if (haystack[0] != needle[0])
			continue;

		if (0 != strncmp(haystack, needle, needle_len))
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
}

FastUnmatch::FastUnmatch(size_t filesCount, const FindOption& findOptions)
	: enabled{ false }
	, matchCase{ findOptions._isMatchCase }
{
	const int searchTermLength = (int)findOptions._str2Search.length();
	const int minSearchTermLength = 1;
	const size_t minFileCount = 100;

	if (filesCount < minFileCount
		|| searchTermLength <= minSearchTermLength)
		return;

	if (findOptions._searchType == FindExtended)
	{
		std::wstring buffer;
		buffer.resize(searchTermLength);
		Searching::convertExtendedToString(findOptions._str2Search.c_str(), buffer.data(), searchTermLength);

		int utf8StrSize = WideCharToMultiByte(CP_UTF8, 0, buffer.data(), searchTermLength, NULL, 0, NULL, NULL);
		searchTerm.resize(utf8StrSize);
		WideCharToMultiByte(CP_UTF8, 0, buffer.data(), searchTermLength, searchTerm.data(), utf8StrSize, NULL, NULL);
	}
	else if (findOptions._searchType == FindNormal)
	{
		int utf8StrSize = WideCharToMultiByte(CP_UTF8, 0, findOptions._str2Search.c_str(), searchTermLength, NULL, 0, NULL, NULL);
		searchTerm.resize(utf8StrSize);
		WideCharToMultiByte(CP_UTF8, 0, findOptions._str2Search.c_str(), searchTermLength, searchTerm.data(), utf8StrSize, NULL, NULL);
	}
	else if (findOptions._searchType == FindRegex)
	{
		// TODO: Check if the regex search term contains a simple string and do unmatching using that or alternatively
		// TODO: Save a regex to the context and use it for matching.
	}

	if (searchTerm.length() > minSearchTermLength)
		enabled = true;
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

	char* fileContents = static_cast<char*>(::MapViewOfFile(hMapping, FILE_MAP_READ, 0, 0, 0));
	if (fileContents == NULL)
	{
		CloseHandle(hMapping);
		CloseHandle(hFile);
		return false;
	}

	bool not_found = false;
	if (matchCase)
		not_found = !detail::contains(fileContents, fileSize, searchTerm.data(), searchTerm.length());
	else
		not_found = !detail::containsCaseInsensitive(fileContents, fileSize, searchTerm.data(), searchTerm.length());

	UnmapViewOfFile(fileContents);
	CloseHandle(hMapping);
	CloseHandle(hFile);

	return not_found;
}
