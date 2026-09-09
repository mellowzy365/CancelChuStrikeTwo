#include "scan.h"
#include <cstdio>
#include <cstring>

namespace
{
    uint8_t HexNib(char c)
    {
        if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
        if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
        if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
        return 0xFF;
    }
}

void* scan::Pattern(HMODULE module, const char* sig)
{
    if (!module || !sig)
        return nullptr;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(
        reinterpret_cast<uintptr_t>(module) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    auto* section = IMAGE_FIRST_SECTION(nt);

    uint8_t bytes[256];
    uint8_t mask[256];
    int len = 0;

    const char* p = sig;
    while (*p && len < 256)
    {
        if (*p == ' ') { p++; continue; }
        if (*p == '?')
        {
            bytes[len] = 0; mask[len] = 0; len++;
            if (p[1] == '?') p++;
            p++;
            continue;
        }
        uint8_t hi = HexNib(p[0]);
        uint8_t lo = HexNib(p[1]);
        if (hi == 0xFF || lo == 0xFF) { p++; continue; }
        bytes[len] = (uint8_t)((hi << 4) | lo);
        mask[len] = 0xFF;
        len++;
        p += 2;
    }

    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++)
    {
        if (!(section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE))
            continue;

        auto* base = reinterpret_cast<uint8_t*>(module) + section[i].VirtualAddress;
        DWORD size = section[i].Misc.VirtualSize;
        if (size == 0 || len == 0)
            continue;

        for (DWORD j = 0; j <= size - (DWORD)len; j++)
        {
            bool ok = true;
            for (int k = 0; k < len; k++)
            {
                if (mask[k] && base[j + k] != bytes[k])
                {
                    ok = false;
                    break;
                }
            }
            if (ok)
                return base + j;
        }
    }
    return nullptr;
}
