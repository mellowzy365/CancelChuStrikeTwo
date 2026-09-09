#pragma once

#include <windows.h>
#include <cstdint>

namespace scan
{
    void* Pattern(HMODULE module, const char* signature);
}
