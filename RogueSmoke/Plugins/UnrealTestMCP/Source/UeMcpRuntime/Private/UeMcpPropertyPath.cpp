// Copyright (c) 2026 Staffan J. Olsson. Licensed under the MIT License.
// Path DSL parser inspired by Incurian/AgentBridge (MIT) — clean-room.
//
// Parser is a hand-rolled character cursor rather than a tokenizer class.
// The grammar is small enough (five token kinds: identifier, dot, bracket,
// quoted-string, integer) that the tokenizer abstraction pays less than the
// fixed overhead of threading it through every error path. Error positions
// are byte offsets into the original string so the caller can surface
// "position: N" without a second pass.

#include "UeMcpPropertyPath.h"

#include "Misc/CString.h"

namespace UeMcpPropertyPathPrivate
{
	/**
	 * Canonical synthesized tokens. Kept as a flat TArray<FString> not a map
	 * because there's only three of them and case-insensitive compare against
	 * this list is cheaper than building an FName keyset on every call.
	 */
	static const TArray<FString>& GetSynthesizedNames()
	{
		static const TArray<FString> Names = {
			TEXT("Class"),
			TEXT("Components"),
			TEXT("CDO"),
		};
		return Names;
	}

	/** Valid identifier start. `@` is handled separately as the synthesized marker. */
	static bool IsIdentStart(TCHAR C)
	{
		return FChar::IsAlpha(C) || C == TEXT('_');
	}

	/** Valid identifier continuation. Digits are allowed inside identifiers, not at the start. */
	static bool IsIdentCont(TCHAR C)
	{
		return FChar::IsAlnum(C) || C == TEXT('_');
	}

	/** Small helper that stamps an error-info struct — not named MakeError to
	 *  avoid the TValueOrError::MakeError variadic template collision (see
	 *  `docs/ue-api-gotchas.md §7`). */
	static void FillParseError(
		FUeMcpPathParseError* OutError,
		const FString& Code,
		const FString& Message,
		int32 Position)
	{
		if (OutError)
		{
			OutError->Code = Code;
			OutError->Message = Message;
			OutError->Position = Position;
		}
	}
}

bool FUeMcpPropertyPath::IsKnownSynthesizedName(const FString& Name)
{
	using namespace UeMcpPropertyPathPrivate;
	for (const FString& Known : GetSynthesizedNames())
	{
		if (Name.Equals(Known, ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

bool FUeMcpPropertyPath::ParsePath(
	const FString& PathString,
	TArray<FUeMcpPathSegment>& OutSegments,
	FUeMcpPathParseError* OutError)
{
	using namespace UeMcpPropertyPathPrivate;

	if (PathString.IsEmpty())
	{
		FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
			TEXT("Path is empty"), 0);
		return false;
	}

	TArray<FUeMcpPathSegment> Segments;
	int32 Pos = 0;
	const int32 Len = PathString.Len();
	bool bExpectSegment = true; // true at start and after every '.'

	while (Pos < Len)
	{
		const TCHAR C = PathString[Pos];

		// Skip whitespace between segments (grammar is permissive).
		if (FChar::IsWhitespace(C))
		{
			Pos++;
			continue;
		}

		// Bracket access: either `[N]` (array/set index) or `["K"]` (map key).
		// Bracket access is a MODIFIER on the previous segment — it can only
		// follow a Property/Synthesized segment, never appear at top level or
		// after a dot.
		if (C == TEXT('['))
		{
			if (Segments.Num() == 0 || bExpectSegment)
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					TEXT("Unexpected '[' — bracket access must follow an identifier"),
					Pos);
				return false;
			}

			// Brackets attach a new segment (ArrayIndex or MapKey). The
			// preceding Property segment stands as-is; the index/key is a
			// distinct segment because the resolver walks segments linearly.
			Pos++; // consume '['

			if (Pos >= Len)
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					TEXT("Unclosed '[' — expected index or quoted key"),
					Pos);
				return false;
			}

			if (PathString[Pos] == TEXT('"'))
			{
				// Quoted string key: `["K"]`. Handle \" and \\ escapes; everything
				// else passes through. We do not decode \n/\t etc. — map keys are
				// FName/FString and whitespace is rare; consistent "pass through"
				// is simpler and does not break any realistic test.
				Pos++; // consume opening quote
				const int32 QuoteStart = Pos;
				FString Key;
				bool bClosed = false;
				while (Pos < Len)
				{
					const TCHAR K = PathString[Pos];
					if (K == TEXT('\\') && Pos + 1 < Len)
					{
						const TCHAR Next = PathString[Pos + 1];
						if (Next == TEXT('"') || Next == TEXT('\\'))
						{
							Key.AppendChar(Next);
							Pos += 2;
							continue;
						}
					}
					if (K == TEXT('"'))
					{
						bClosed = true;
						Pos++; // consume closing quote
						break;
					}
					Key.AppendChar(K);
					Pos++;
				}

				if (!bClosed)
				{
					FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
						TEXT("Unclosed quoted map key"),
						QuoteStart);
					return false;
				}

				if (Pos >= Len || PathString[Pos] != TEXT(']'))
				{
					FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
						TEXT("Expected ']' after quoted map key"),
						Pos);
					return false;
				}
				Pos++; // consume ']'

				FUeMcpPathSegment Seg;
				Seg.Kind = EUeMcpPathSegmentKind::MapKey;
				Seg.Key = Key;
				Segments.Add(MoveTemp(Seg));
			}
			else
			{
				// Numeric array/set index: `[N]`. Negative indices are parseable
				// (for "last element" semantics) — but the resolver currently
				// rejects them as out-of-bounds, deliberately. Numeric parse
				// uses FCString::Atoi which ignores trailing non-digits, so we
				// verify the bracket contents are strict [-]?digits+.
				const int32 IndexStart = Pos;
				FString IndexStr;
				bool bSawDigit = false;
				while (Pos < Len && PathString[Pos] != TEXT(']'))
				{
					const TCHAR D = PathString[Pos];
					if (D == TEXT('-') && IndexStr.Len() == 0)
					{
						IndexStr.AppendChar(D);
					}
					else if (FChar::IsDigit(D))
					{
						IndexStr.AppendChar(D);
						bSawDigit = true;
					}
					else
					{
						FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
							FString::Printf(TEXT("Expected numeric index, got '%c'"), D),
							Pos);
						return false;
					}
					Pos++;
				}

				if (Pos >= Len || PathString[Pos] != TEXT(']'))
				{
					FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
						TEXT("Unclosed '[' — expected ']'"),
						IndexStart);
					return false;
				}
				if (!bSawDigit)
				{
					FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
						TEXT("Empty bracket — expected index or quoted key"),
						IndexStart);
					return false;
				}
				Pos++; // consume ']'

				FUeMcpPathSegment Seg;
				Seg.Kind = EUeMcpPathSegmentKind::ArrayIndex;
				Seg.Index = FCString::Atoi(*IndexStr);
				Segments.Add(MoveTemp(Seg));
			}

			bExpectSegment = false;
			continue;
		}

		// Dot separator between segments. Trailing / leading / duplicate dots
		// are errors caught by the `bExpectSegment` flag.
		if (C == TEXT('.'))
		{
			if (bExpectSegment)
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					TEXT("Unexpected '.' — expected an identifier"),
					Pos);
				return false;
			}
			Pos++;
			bExpectSegment = true;
			continue;
		}

		// Identifier segment, possibly preceded by `@` (synthesized marker).
		// After the identifier, an optional `:enum_name` suffix requests
		// display-string rendering for enum leaves.
		const bool bIsSynthesized = (C == TEXT('@'));
		const int32 IdentStart = Pos;

		if (bIsSynthesized)
		{
			Pos++; // consume '@'
			if (Pos >= Len || !IsIdentStart(PathString[Pos]))
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					TEXT("Expected synthesized token name after '@'"),
					Pos);
				return false;
			}
		}
		else if (!IsIdentStart(C))
		{
			FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
				FString::Printf(TEXT("Unexpected character '%c'"), C),
				Pos);
			return false;
		}

		FString Ident;
		while (Pos < Len && IsIdentCont(PathString[Pos]))
		{
			Ident.AppendChar(PathString[Pos]);
			Pos++;
		}

		if (Ident.IsEmpty())
		{
			FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
				TEXT("Empty identifier"),
				IdentStart);
			return false;
		}

		FUeMcpPathSegment Seg;
		if (bIsSynthesized)
		{
			if (!IsKnownSynthesizedName(Ident))
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("Unknown synthesized token '@%s'"), *Ident),
					IdentStart);
				return false;
			}
			Seg.Kind = EUeMcpPathSegmentKind::Synthesized;
		}
		else
		{
			Seg.Kind = EUeMcpPathSegmentKind::Property;
		}
		Seg.Name = Ident;

		// `:enum_name` suffix — optional, only on Property segments. It tells
		// the resolver "if this terminates at an enum, return the display
		// string rather than the numeric form". The suffix is consumed here
		// and stored on the segment; a stray `:` after anything else is an
		// error.
		if (Pos < Len && PathString[Pos] == TEXT(':'))
		{
			if (Seg.Kind != EUeMcpPathSegmentKind::Property)
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					TEXT("':enum_name' suffix is only valid on property segments"),
					Pos);
				return false;
			}
			Pos++; // consume ':'
			FString Suffix;
			while (Pos < Len && IsIdentCont(PathString[Pos]))
			{
				Suffix.AppendChar(PathString[Pos]);
				Pos++;
			}
			if (!Suffix.Equals(TEXT("enum_name"), ESearchCase::IgnoreCase))
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("Unknown segment suffix ':%s'"), *Suffix),
					IdentStart);
				return false;
			}
			Seg.bEnumNameDisplay = true;
		}

		// `@CDO` is the only synthesized token that accepts further path
		// segments after it. `@Class` and `@Components` are terminal leaves
		// with no bracket / dot continuation. Enforce this at parse time so
		// downstream can treat remaining segments as a CDO walk.
		if (Seg.Kind == EUeMcpPathSegmentKind::Synthesized
			&& !Seg.Name.Equals(TEXT("CDO"), ESearchCase::IgnoreCase))
		{
			Segments.Add(MoveTemp(Seg));
			// Consume trailing whitespace and ensure no more non-whitespace.
			while (Pos < Len && FChar::IsWhitespace(PathString[Pos]))
			{
				Pos++;
			}
			if (Pos < Len)
			{
				FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
					FString::Printf(TEXT("'@%s' must be the terminal segment"), *Segments.Last().Name),
					Pos);
				return false;
			}
			// Path is complete. The final `bExpectSegment` check below would
			// otherwise reject us as a trailing-dot error.
			bExpectSegment = false;
			break;
		}

		Segments.Add(MoveTemp(Seg));
		bExpectSegment = false;
	}

	// Trailing dot: we're still expecting a segment but ran out of input.
	if (bExpectSegment)
	{
		FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
			TEXT("Trailing '.' — expected an identifier"),
			Len);
		return false;
	}

	// `@CDO` must be the FIRST segment if used. Catches `Health.@CDO.X`.
	for (int32 i = 0; i < Segments.Num(); ++i)
	{
		if (Segments[i].Kind == EUeMcpPathSegmentKind::Synthesized
			&& Segments[i].Name.Equals(TEXT("CDO"), ESearchCase::IgnoreCase)
			&& i != 0)
		{
			FillParseError(OutError, TEXT("INVALID_PAYLOAD"),
				TEXT("'@CDO' must be the first segment"),
				0);
			return false;
		}
	}

	OutSegments = MoveTemp(Segments);
	return true;
}

bool FUeMcpPropertyPath::ValidatePath(
	const FString& PathString,
	FUeMcpPathParseError* OutError)
{
	TArray<FUeMcpPathSegment> Throwaway;
	return ParsePath(PathString, Throwaway, OutError);
}
