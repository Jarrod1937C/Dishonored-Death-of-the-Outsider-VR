#include "pch.h"
#include "Seeds.h"
#include "GameInfo.h"

namespace
{
#include "SeedsData.inc"

    void WriteIfMissing(const wchar_t* file, const char* text, size_t len)
    {
        const std::wstring path = GameInfo::GameDir() + L"\\" + file;
        const std::string name(path.begin() + path.find_last_of(L'\\') + 1, path.end());
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) { LOGI("Seeds: {} already there: kept (learned on this machine)", name); return; }
        FILE* f = _wfopen(path.c_str(), L"wb");
        if (!f) { LOGI("Seeds: {} could not be written to the game folder (read-only?): the hands will be learned in play", name); return; }
        fwrite(text, 1, len, f);
        fclose(f);
        LOGI("Seeds: {} written from the mod's defaults ({} bytes)", name, len);
    }
}

void Seeds::Install()
{
    WriteIfMissing(L"DVR_grips.txt", kGripsSeed, sizeof kGripsSeed - 1);
    WriteIfMissing(L"DVR_hands.txt", kHandsSeed, sizeof kHandsSeed - 1);
}
