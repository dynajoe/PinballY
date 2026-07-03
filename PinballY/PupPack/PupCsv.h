// This file is part of PinballY
// Copyright 2026 Joe Andaverde | GPL v3 or later | NO WARRANTY
//
// Minimal, portable CSV reader for .pup files.
//
// PuP .pup files are CSV with a header row naming the columns. Crucially,
// the column SET and ORDER vary between PuP versions and between packs, so we
// parse by HEADER NAME rather than fixed index. That is the single most
// important robustness decision in this port: it lets one parser ingest the
// whole existing pack library despite years of format drift.
//
// Header-only so the test harness and PinballY can share it without a build
// dependency.

#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace pup
{
	// Case-insensitive ASCII string equality.  Shared across the portable
	// core (the loader's .pup filename matching, the engine's PlayAction
	// tests); it lives in this header-only layer so there's exactly one
	// definition and no extra build dependency.
	inline bool IEquals(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size()) return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
		return true;
	}

	class CsvTable
	{
	public:
		// Parse CSV text. The first non-empty line is treated as the header.
		bool Parse(const std::string& text)
		{
			rows.clear();
			colIndex.clear();
			std::vector<std::vector<std::string>> all;
			std::stringstream ss(text);
			std::string line;
			while (std::getline(ss, line))
			{
				if (!line.empty() && line.back() == '\r') line.pop_back();
				// skip fully blank lines
				if (line.find_first_not_of(" \t") == std::string::npos) continue;
				all.push_back(SplitLine(line));
			}
			if (all.empty()) return false;

			// header -> normalized lowercase name -> column index
			for (size_t i = 0; i < all[0].size(); ++i)
				colIndex[Norm(all[0][i])] = i;
			rows.assign(all.begin() + 1, all.end());
			return true;
		}

		size_t RowCount() const { return rows.size(); }

		// Whether a named column exists (case/space-insensitive).
		bool Has(const std::string& name) const { return colIndex.count(Norm(name)) != 0; }

		// Fetch a cell by row index and column name, trying each alias in
		// order (PuP versions rename columns; aliases absorb that).
		std::string Get(size_t row, std::initializer_list<const char*> names,
		                const std::string& dflt = "") const
		{
			if (row >= rows.size()) return dflt;
			for (auto n : names)
			{
				auto it = colIndex.find(Norm(n));
				if (it != colIndex.end() && it->second < rows[row].size())
				{
					const std::string& v = rows[row][it->second];
					if (!v.empty()) return v;
				}
			}
			return dflt;
		}

		int GetInt(size_t row, std::initializer_list<const char*> names, int dflt = 0) const
		{
			std::string s = Get(row, names, "");
			if (s.empty()) return dflt;
			try { return std::stoi(s); } catch (...) { return dflt; }
		}

		bool GetBool(size_t row, std::initializer_list<const char*> names, bool dflt = false) const
		{
			std::string s = Get(row, names, "");
			if (s.empty()) return dflt;
			s = Norm(s);
			return s == "1" || s == "true" || s == "yes" || s == "on";
		}

	private:
		std::vector<std::vector<std::string>> rows;
		std::map<std::string, size_t> colIndex;

		static std::string Norm(const std::string& s)
		{
			std::string o;
			for (char c : s)
				if (!std::isspace((unsigned char)c) && c != '_')
					o += (char)std::tolower((unsigned char)c);
			return o;
		}

		// Split a CSV line on commas, honoring double-quoted fields.
		static std::vector<std::string> SplitLine(const std::string& line)
		{
			std::vector<std::string> out;
			std::string cur;
			bool inQ = false;
			for (size_t i = 0; i < line.size(); ++i)
			{
				char c = line[i];
				if (inQ)
				{
					if (c == '"')
					{
						if (i + 1 < line.size() && line[i + 1] == '"') { cur += '"'; ++i; }
						else inQ = false;
					}
					else cur += c;
				}
				else if (c == '"') inQ = true;
				else if (c == ',') { out.push_back(Trim(cur)); cur.clear(); }
				else cur += c;
			}
			out.push_back(Trim(cur));
			return out;
		}

		static std::string Trim(std::string s)
		{
			size_t a = s.find_first_not_of(" \t");
			size_t b = s.find_last_not_of(" \t");
			if (a == std::string::npos) return "";
			return s.substr(a, b - a + 1);
		}
	};
}
