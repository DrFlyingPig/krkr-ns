/* SPDX-License-Identifier: MIT */
#pragma once

#include <string>

struct RendererSettings
{
    std::string renderer = "software";
    std::string memoryUsage = "unlimited";
    int fpsLimit = 60;
    std::string rendererSource = "default";
    std::string memorySource = "default";
    std::string fpsSource = "default";
};

// Read the reference's public preferences without creating or modifying them.
// Each game's keys override the global keys individually. Unknown renderer
// names remain intact so the renderer selector can report its own fallback.
RendererSettings ReadReferenceRendererSettings(const char* gameDir);
