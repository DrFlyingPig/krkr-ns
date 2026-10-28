/* SPDX-License-Identifier: MIT */
#include "ReferenceRendererConfig.h"
#include "tinyxml2.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace
{
void ReadFile(const std::string& path, RendererSettings& settings)
{
    // Match iSysConfigManager::Initialize: ignore the entire file on a failed
    // TinyXML load, then consume only direct Item children of its root element.
    std::unique_ptr<FILE, int (*)(FILE*)> file(std::fopen(path.c_str(), "rb"), std::fclose);
    if (!file) return;
    tinyxml2::XMLDocument document;
    if (document.LoadFile(file.get()) != tinyxml2::XML_SUCCESS) return;
    const auto* root = document.RootElement();
    if (!root) return;
    for (const auto* item = root->FirstChildElement("Item"); item;
         item = item->NextSiblingElement("Item"))
    {
        const char* key = item->Attribute("key");
        const char* value = item->Attribute("value");
        if (!key || !value) continue;
        if (std::strcmp(key, "renderer") == 0)
        {
            settings.renderer = value;
            settings.rendererSource = path;
        }
        else if (std::strcmp(key, "memusage") == 0)
        {
            settings.memoryUsage = value;
            settings.memorySource = path;
        }
        else if (std::strcmp(key, "fps_limit") == 0)
        {
            // The reference's integer preference conversion is atoi. Retain
            // that interpretation, with a safe execution fallback for <= 0.
            settings.fpsLimit = std::atoi(value);
            settings.fpsSource = path;
            if (settings.fpsLimit <= 0)
            {
                settings.fpsLimit = 60;
                settings.fpsSource += " (fps_limit=" + std::string(value) + "; using 60)";
            }
        }
    }
}
}

RendererSettings ReadReferenceRendererSettings(const char* gameDir)
{
    RendererSettings settings;
    ReadFile("sdmc:/switch/kirikiroid2/.preference/GlobalPreference.xml", settings);
    if (gameDir && *gameDir)
    {
        std::string path(gameDir);
        if (path.back() != '/' && path.back() != '\\') path += '/';
        path += "Kirikiroid2Preference.xml";
        ReadFile(path, settings);
    }
    return settings;
}
