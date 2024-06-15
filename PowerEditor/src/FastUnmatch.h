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


#pragma once

#include <tchar.h>
#include <vector>

struct FindOption;

/** Used to discard files from "Find in Files" before they get loaded by MainFileManager.
 * Avoiding file loading can have huge performance benefits when the number of files being
 * searched is large and the number of files with search hits is low.
 *
 * Only enabled when
 *  - file count to be searched is >=100,
 *  - search term is >=4 bytes in utf-8 and utf-16 and
 *  - search type is Normal or Extended.
 */
class FastUnmatch
{
public:
	FastUnmatch(size_t fileCount, const FindOption& findOptions);
	bool fileDoesNotContainString(const TCHAR* filename) const;
	bool isEnabled() const {
		return enabled;
	}

private:
	bool enabled;
	std::vector<std::vector<uint8_t>> searchTerms;
	std::vector<std::vector<uint16_t>> searchTermsWide;

	std::vector<std::vector<uint8_t>> searchTermsUpper;
	std::vector<std::vector<uint8_t>> searchTermsLower;

	std::vector<std::vector<uint16_t>> searchTermsWideUpper;
	std::vector<std::vector<uint16_t>> searchTermsWideLower;
	size_t maxSearchTermLength = 0;
};
