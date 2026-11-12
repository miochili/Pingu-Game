// Pingu - player names in a LAN game: cleaning and lookalike checks. Header: PinguNames.h.

#include "PinguNames.h"

namespace PinguNamesPrivate
{
	bool IsDropped(TCHAR C)
	{
		return (C < 0x20 && C != 9 && C != 10 && C != 13) || (C >= 0x7F && C <= 0x9F)             // control
			|| C == 0x00AD || C == 0x034F || C == 0x061C || C == 0x180E // soft hyphen, joiner marks
			|| (C >= 0x200B && C <= 0x200F)                          // zero-width space, joiners, direction marks
			|| (C >= 0x202A && C <= 0x202E)                          // direction overrides
			|| (C >= 0x2060 && C <= 0x206F)                          // word joiner, invisible operators, deprecated format marks
			|| (C >= 0xFE00 && C <= 0xFE0F) || C == 0xFEFF          // variation selectors, byte order mark
			|| (C >= 0xFFF0 && C <= 0xFFFF)                          // specials
			|| (C >= 0xD800 && C <= 0xDFFF)                          // surrogates (emoji, tag characters): the font has none anyway
			|| C == TEXT('?');
	}
	bool IsSpace(TCHAR C)
	{
		return C == TEXT(' ') || C == 9 || C == 10 || C == 13 || C == 0x00A0 || (C >= 0x2000 && C <= 0x200A) || C == 0x2028 || C == 0x2029 || C == 0x202F || C == 0x205F || C == 0x3000
			|| C == 0x1680;
	}
	/** One lower-case lookalike letter for C (0 = none). */
	TCHAR Lookalike(TCHAR C)
	{
		// full-width forms -> ASCII
		if (C >= 0xFF01 && C <= 0xFF5E)
		{
			C = static_cast<TCHAR>(C - 0xFEE0);
		}
		// Latin-1 accents -> the base letter
		static const TCHAR* const Latin1 = TEXT("aaaaaaaceeeeiiiidnooooo/ouuuuyty"); // U+00E0 .. U+00FF (as a lower-case row)
		if (C >= 0x00C0 && C <= 0x00DE)
		{
			C = static_cast<TCHAR>(C + 0x20); // upper -> lower row
		}
		if (C >= 0x00E0 && C <= 0x00FF)
		{
			const TCHAR Base = Latin1[C - 0x00E0];
			return Base == TEXT('/') ? C : Base;
		}
		switch (C)
		{
		// Cyrillic
		case 0x0430: case 0x0410: return TEXT('a');
		case 0x0432: return TEXT('b');
		case 0x0435: case 0x0415: case 0x0451: return TEXT('e');
		case 0x043E: case 0x041E: return TEXT('o');
		case 0x0440: case 0x0420: return TEXT('p');
		case 0x0441: case 0x0421: return TEXT('c');
		case 0x0443: case 0x0423: return TEXT('y');
		case 0x0445: case 0x0425: return TEXT('x');
		case 0x043A: case 0x041A: return TEXT('k');
		case 0x043C: case 0x041C: return TEXT('m');
		case 0x043D: case 0x041D: return TEXT('h');
		case 0x0442: case 0x0422: return TEXT('t');
		case 0x0456: case 0x0406: case 0x0457: return TEXT('l'); // i
		case 0x0458: case 0x0408: return TEXT('j');
		case 0x0455: case 0x0405: return TEXT('s');
		case 0x0501: return TEXT('d');
		// Greek
		case 0x03B1: case 0x0391: return TEXT('a');
		case 0x03B2: case 0x0392: return TEXT('b');
		case 0x03B5: case 0x0395: return TEXT('e');
		case 0x03B9: case 0x0399: return TEXT('l');
		case 0x03BA: case 0x039A: return TEXT('k');
		case 0x03BC: case 0x039C: return TEXT('m');
		case 0x03BD: case 0x039D: return TEXT('v');
		case 0x03BF: case 0x039F: return TEXT('o');
		case 0x03C1: case 0x03A1: return TEXT('p');
		case 0x03C4: case 0x03A4: return TEXT('t');
		case 0x03C5: case 0x03A5: return TEXT('u');
		case 0x03C7: case 0x03A7: return TEXT('x');
		case 0x03B7: case 0x0397: return TEXT('n');
		case 0x2160: return TEXT('l'); // Roman numeral one
		case 0x2024: case 0x00B7: return TEXT('.');
		default: return 0;
		}
	}
}

FString PinguNames::Sanitize(const FString& Name)
{
	using namespace PinguNamesPrivate;
	FString Out;
	bool bSpacePending = false;
	for (const TCHAR C : Name)
	{
		if (IsDropped(C))
		{
			continue;
		}
		if (IsSpace(C))
		{
			bSpacePending = !Out.IsEmpty();
			continue;
		}
		if (bSpacePending)
		{
			Out.AppendChar(TEXT(' '));
			bSpacePending = false;
		}
		Out.AppendChar(C);
	}
	Out = Out.Left(MaxLength);
	Out.TrimEndInline();
	return Out;
}

FString PinguNames::Fold(const FString& Name)
{
	using namespace PinguNamesPrivate;
	FString Out;
	for (TCHAR C : Name)
	{
		if (IsDropped(C) || IsSpace(C) || C == TEXT('_') || C == TEXT('-') || C == TEXT('.') || C == TEXT('\'') || C == 0x2010 || C == 0x2011 || C == 0x2212)
		{
			continue;
		}
		if (const TCHAR Alike = Lookalike(C))
		{
			C = Alike;
		}
		else if (C >= 0xFF01 && C <= 0xFF5E)
		{
			C = static_cast<TCHAR>(C - 0xFEE0);
		}
		C = FChar::ToLower(C);
		switch (C)
		{
		case TEXT('0'): C = TEXT('o'); break;
		case TEXT('1'): case TEXT('i'): case TEXT('|'): case TEXT('!'): C = TEXT('l'); break;
		case TEXT('5'): case TEXT('$'): C = TEXT('s'); break;
		case TEXT('3'): C = TEXT('e'); break;
		case TEXT('4'): case TEXT('@'): C = TEXT('a'); break;
		case TEXT('7'): C = TEXT('t'); break;
		case TEXT('8'): C = TEXT('b'); break;
		case TEXT('9'): C = TEXT('g'); break;
		default: break;
		}
		Out.AppendChar(C);
	}
	Out.ReplaceInline(TEXT("rn"), TEXT("m"), ESearchCase::CaseSensitive);
	Out.ReplaceInline(TEXT("vv"), TEXT("w"), ESearchCase::CaseSensitive);
	return Out;
}

FString PinguNames::MakeUnique(const FString& Name, const TArray<FString>& Others)
{
	TArray<FString> Taken;
	for (const FString& Other : Others)
	{
		Taken.Add(Fold(Other));
	}
	FString Result = Name;
	for (int32 Number = 2; Number <= Others.Num() + 2 && Taken.Contains(Fold(Result)); ++Number)
	{
		const FString Suffix = FString::Printf(TEXT(" %d"), Number);
		Result = Name.Left(MaxLength - Suffix.Len()) + Suffix;
		Result.TrimEndInline();
	}
	return Result;
}
