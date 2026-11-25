#include "common.h"

#include "FileMgr.h"
#ifdef MORE_LANGUAGES
#include "Game.h"
#endif
#include "Frontend.h"
#include "Messages.h"
#include "Text.h"

wchar WideErrorString[25];
#ifdef __PS3__
#include "ps3_textfallback.h"
#endif

CText TheText;

#ifdef __PS3__
// PS3: the .gxt of each language (TEXT\JAPAN.GXT too, the name some packs
// use). Polish and Russian use MODELS\FONTS_P.TXD / FONTS_R.TXD when they're
// there and FONTS.TXD if not (Font.cpp); Japanese can't go without
// FONTS_J.TXD (its letters are a texture of their own). When the language
// isn't usable (a saved Polish whose polish.gxt is gone, no french.gxt...)
// the game goes back to American: an empty text table leaves the menus blank.
static bool
FileThere(const char *name)	// relative to CFileMgr's dir
{
	int f = CFileMgr::OpenFile(name, "rb");
	if (f <= 0)
		return false;
	CFileMgr::CloseFile(f);
	return true;
}

// A .gxt of another game (a Vice City russian.gxt copied to GTA III's TEXT
// loads only its first table: every key the game asks for is "missing").
// GTA III's start with TKEY; the others aren't offered.
static bool
GxtOfThisGame(const char *name)	// CFileMgr's dir is TEXT
{
	char magic[4];
	int f = CFileMgr::OpenFile(name, "rb");
	if (f <= 0)
		return false;
	bool ok = CFileMgr::Read(f, magic, 4) == 4 && memcmp(magic, "TKEY", 4) == 0;
	CFileMgr::CloseFile(f);
	if (!ok)
		printf("CText: TEXT\\%s isn't GTA III's (another game's .gxt?): not used\n", name);
	return ok;
}

static const char *
GxtFileName(int32 lang)	// CFileMgr's dir is TEXT; nil if the language can't be used
{
	const char *name;
	switch (lang) {
	case CMenuManager::LANGUAGE_FRENCH: name = "FRENCH.GXT"; break;
	case CMenuManager::LANGUAGE_GERMAN: name = "GERMAN.GXT"; break;
	case CMenuManager::LANGUAGE_ITALIAN: name = "ITALIAN.GXT"; break;
	case CMenuManager::LANGUAGE_SPANISH: name = "SPANISH.GXT"; break;
#ifdef MORE_LANGUAGES
	case CMenuManager::LANGUAGE_POLISH: name = "POLISH.GXT"; break;
	case CMenuManager::LANGUAGE_RUSSIAN: name = "RUSSIAN.GXT"; break;
	case CMenuManager::LANGUAGE_JAPANESE: {
		CFileMgr::SetDir("");
		bool fonts = FileThere("MODELS/FONTS_J.TXD");
		CFileMgr::SetDir("TEXT");
		if (!fonts)
			return nil;
		name = FileThere("JAPANESE.GXT") ? "JAPANESE.GXT" : "JAPAN.GXT";
		break;
	}
#endif
	default: name = "AMERICAN.GXT"; break;
	}
	// the added languages only: the others are the game's own files
	if (lang > CMenuManager::LANGUAGE_SPANISH)
		return GxtOfThisGame(name) ? name : nil;
	return FileThere(name) ? name : nil;
}

bool
CText::PS3HasLanguage(int32 lang)
{
	CFileMgr::SetDir("TEXT");
	bool found = GxtFileName(lang) != nil;
	CFileMgr::SetDir("");
	return found;
}

static void
PS3PickGxt(char *filename)	// CFileMgr's dir is TEXT
{
	const char *name = GxtFileName(CMenuManager::m_PrefsLanguage);
	if (name == nil) {
		if (CMenuManager::m_PrefsLanguage != CMenuManager::LANGUAGE_AMERICAN)
			printf("CText: language %d can't be used (no .gxt in TEXT\\, or no MODELS\\FONTS_J.TXD), using AMERICAN.GXT\n",
				(int)CMenuManager::m_PrefsLanguage);
		CMenuManager::m_PrefsLanguage = CMenuManager::LANGUAGE_AMERICAN;
		name = "AMERICAN.GXT";
	}
	strcpy(filename, name);
}
#endif

CText::CText(void)
{
	encoding = 'e';
	memset(WideErrorString, 0, sizeof(WideErrorString));
}

void
CText::Load(void)
{
	uint8 *filedata;
	char filename[32], type[4];
	ssize_t offset, length;
	size_t sectlen;

	Unload();
#ifdef __PS3__
	// PS3: room for the translations' .gxt files (a russian.gxt doesn't fit
	// in the original 256 KB). Only while loading.
	const int gxtMax = 0x100000;
#else
	const int gxtMax = 0x40000;
#endif
	filedata = new uint8[gxtMax];

	CFileMgr::SetDir("TEXT");
#ifdef __PS3__
	PS3PickGxt(filename);
#else
	switch(CMenuManager::m_PrefsLanguage){
	case CMenuManager::LANGUAGE_AMERICAN:
		sprintf(filename, "AMERICAN.GXT");
		break;
	case CMenuManager::LANGUAGE_FRENCH:
		sprintf(filename, "FRENCH.GXT");
		break;
	case CMenuManager::LANGUAGE_GERMAN:
		sprintf(filename, "GERMAN.GXT");
		break;
	case CMenuManager::LANGUAGE_ITALIAN:
		sprintf(filename, "ITALIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_SPANISH:
		sprintf(filename, "SPANISH.GXT");
		break;
#ifdef MORE_LANGUAGES
	case CMenuManager::LANGUAGE_POLISH:
		sprintf(filename, "POLISH.GXT");
		break;
	case CMenuManager::LANGUAGE_RUSSIAN:
		sprintf(filename, "RUSSIAN.GXT");
		break;
	case CMenuManager::LANGUAGE_JAPANESE:
		sprintf(filename, "JAPANESE.GXT");
		break;
#endif
	}
#endif

	length = CFileMgr::LoadFile(filename, filedata, gxtMax, "rb");
	CFileMgr::SetDir("");

	offset = 0;
	while(offset < length){
		type[0] = filedata[offset++];
		type[1] = filedata[offset++];
		type[2] = filedata[offset++];
		type[3] = filedata[offset++];
		sectlen = (int)filedata[offset+3]<<24 | (int)filedata[offset+2]<<16 |
			(int)filedata[offset+1]<<8 | (int)filedata[offset+0];
		offset += 4;
#ifdef __PS3__
		if(offset + (ssize_t)sectlen > length){
			printf("CText: %s: section %.4s goes past the end of the file\n", filename, type);
			break;
		}
#endif
		if(sectlen != 0){
			if(strncmp(type, "TKEY", 4) == 0)
				keyArray.Load(sectlen, filedata, &offset);
			else if(strncmp(type, "TDAT", 4) == 0)
				data.Load(sectlen, filedata, &offset);
			else
				offset += sectlen;
		}
	}

	keyArray.Update(data.chars);

	delete[] filedata;
}

void
CText::Unload(void)
{
	CMessages::ClearAllMessagesDisplayedByGame();
	data.Unload();
	keyArray.Unload();
}

wchar*
CText::Get(const char *key)
{
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	wchar *text = keyArray.Search(key, data.chars);
#else
	wchar *text = keyArray.Search(key);
#endif
#ifdef __PS3__
	{
		// the pad's buttons where the PC text says keyboard or mouse
		static wchar *ocache[ARRAY_SIZE(ps3TextOverride)];
		for (int i = 0; i < ARRAY_SIZE(ps3TextOverride); i++) {
			if (strcmp(ps3TextOverride[i].key, key) != 0)
				continue;
			if (ocache[i] == nil) {
				size_t len = strlen(ps3TextOverride[i].text);
				ocache[i] = new wchar[len + 1];
				for (size_t j = 0; j <= len; j++)
					ocache[i][j] = (uint8)ps3TextOverride[i].text[j];
			}
			return ocache[i];
		}
	}
	if (text == WideErrorString) {
		// re3's own menu keys aren't in the PC american.gxt
		static wchar *cache[ARRAY_SIZE(ps3TextFallback)];
		for (int i = 0; i < ARRAY_SIZE(ps3TextFallback); i++) {
			if (strcmp(ps3TextFallback[i].key, key) != 0)
				continue;
			if (cache[i] == nil) {
				size_t len = strlen(ps3TextFallback[i].text);
				cache[i] = new wchar[len + 1];
				for (size_t j = 0; j <= len; j++)
					cache[i][j] = (uint8)ps3TextFallback[i].text[j];
			}
			return cache[i];
		}
	}
#endif
	return text;
}

wchar UpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
	138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148,
	149, 173, 173, 175, 176, 177, 178, 179, 180, 181, 182,
	183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193,
	194, 195, 196, 197, 198, 199, 200, 201, 202, 203, 204,
	205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215,
	216, 217, 218, 219, 220, 221, 222, 223, 224, 225, 226,
	227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237,
	238, 239, 240, 241, 242, 243, 244, 245, 246, 247, 248,
	249, 250, 251, 252, 253, 254, 255
};

wchar FrenchUpperCaseTable[128] = {
	128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138,
	139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
	150, 65, 65, 65, 65, 132, 133, 69, 69, 69, 69, 73, 73,
	73, 73, 79, 79, 79, 79, 85, 85, 85, 85, 173, 173, 175,
	176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186,
	187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
	198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208,
	209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
	220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230,
	231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
	242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252,
	253, 254, 255
};

wchar
CText::GetUpperCase(wchar c)
{
	switch (encoding)
	{
	case 'e':
		if (c >= 'a' && c <= 'z')
			return c - 32;
		break;
	case 'f':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return FrenchUpperCaseTable[c-128];
		break;
	case 'g':
	case 'i':
	case 's':
		if (c >= 'a' && c <= 'z')
			return c - 32;

		if (c >= 128 && c <= 255)
			return UpperCaseTable[c-128];
		break;
	default:
		break;
	}
	return c;
}

void
CText::UpperCase(wchar *s)
{
	while(*s){
		*s = GetUpperCase(*s);
		s++;
	}
}


void
CKeyArray::Load(size_t length, uint8 *data, ssize_t *offset)
{
	size_t i;
	uint8 *rawbytes;

	// You can make numEntries size_t if you want to exceed 32-bit boundaries, everything else should be ready.
	numEntries = (int)(length / sizeof(CKeyEntry));
	entries = new CKeyEntry[numEntries];
	rawbytes = (uint8*)entries;

	for(i = 0; i < length; i++)
		rawbytes[i] = data[(*offset)++];
#ifdef BIGENDIAN
	// .gxt is little-endian
	for(i = 0; i < (size_t)numEntries; i++)
		entries[i].valueOffset = LittleU32(entries[i].valueOffset);
#endif
}

void
CKeyArray::Unload(void)
{
	delete[] entries;
	entries = nil;
	numEntries = 0;
}

void
CKeyArray::Update(wchar *chars)
{
#if !defined(FIX_BUGS) && !defined(FIX_BUGS_64)
	int i;
	for(i = 0; i < numEntries; i++)
		entries[i].value = (wchar*)((uint8*)chars + (uintptr)entries[i].value);
#endif
}

CKeyEntry*
CKeyArray::BinarySearch(const char *key, CKeyEntry *entries, int16 low, int16 high)
{
	int mid;
	int diff;

	if(low > high)
		return nil;

	mid = (low + high)/2;
	diff = strcmp(key, entries[mid].key);
	if(diff == 0)
		return &entries[mid];
	if(diff < 0)
		return BinarySearch(key, entries, low, mid-1);
	if(diff > 0)
		return BinarySearch(key, entries, mid+1, high);
	return nil;
}

wchar*
#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
CKeyArray::Search(const char *key, wchar *data)
#else
CKeyArray::Search(const char *key)
#endif
{
	CKeyEntry *found;
	char errstr[25];
	int i;

#if defined (FIX_BUGS) || defined(FIX_BUGS_64)
	found = BinarySearch(key, entries, 0, numEntries-1);
	if(found)
		return (wchar*)((uint8*)data + found->valueOffset);
#else
	found = BinarySearch(key, entries, 0, numEntries-1);
	if(found)
		return found->value;
#endif
	sprintf(errstr, "%s missing", key);
	for(i = 0; i < 25; i++)
		WideErrorString[i] = errstr[i];
	return WideErrorString;
}


void
CData::Load(size_t length, uint8 *data, ssize_t *offset)
{
	size_t i;
	uint8 *rawbytes;

	// You can make numChars size_t if you want to exceed 32-bit boundaries, everything else should be ready.
	numChars = (int)(length / sizeof(wchar));
	chars = new wchar[numChars];
	rawbytes = (uint8*)chars;

	for(i = 0; i < length; i++)
		rawbytes[i] = data[(*offset)++];
	LittleSwap16(chars, numChars);	// UCS-2 little-endian
}

void
CData::Unload(void)
{
	delete[] chars;
	chars = nil;
	numChars = 0;
}

char*
UnicodeToAscii(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256-1; len++, src++)
#ifdef MORE_LANGUAGES
		if(*src < 128 || ((CGame::russianGame || CGame::japaneseGame) && *src < 256))
#else
		if(*src < 128)
#endif
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForSaveLoad(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

char*
UnicodeToAsciiForMemoryCard(wchar *src)
{
	static char aStr[256];
	int len;
	for(len = 0; *src != '\0' && len < 256; len++, src++)
		if(*src < 256)
			aStr[len] = *src;
		else
			aStr[len] = '#';
	aStr[len] = '\0';
	return aStr;
}

void
TextCopy(wchar *dst, const wchar *src)
{
	while((*dst++ = *src++) != '\0');
}
