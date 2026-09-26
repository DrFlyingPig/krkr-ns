/* SPDX-License-Identifier: MIT */
#include "tjsCommHead.h"
#include "LauncherArtworkStorage.h"
#include "KrkrNSPaths.h"
#include "KrkrNSLog.h"
#include "CharacterSet.h"
#include "XP3Archive.h"
#include "tjsArray.h"
#include "tjsDictionary.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

#ifdef __SWITCH__
extern bool krkrsdl2_game_mode;
#endif

namespace krkrns_launcher_artwork {
namespace {
const char *const ArtworkBase = KRKRNS_BASE_A "/launcher-artwork";
const char *const CachePrefix = KRKRNS_BASE_A "/launcher-artwork/cache/";
const char *const CustomPrefix = KRKRNS_BASE_A "/Artwork/";
const size_t MaxCandidates = 512, MaxVisited = 8192, MaxArchives = 24;
const unsigned MaxDepth = 4;
const size_t MaxIndexBytes = 8 * 1024 * 1024, MaxPreferencesBytes = 512 * 1024;
const size_t MaxPreferences = 2048;

[[noreturn]] void Fail(const char *message)
{
    tjs_string text;
    TVPUtf8ToUtf16(text, std::string(message));
    throw eTJSError(text.c_str());
}

std::string Utf8(const ttstr &value)
{
    // CharacterSet's helpers report false for an empty (valid) string.
    // Empty paths represent the default artwork role and must round-trip.
    if (value.IsEmpty()) return std::string();
    std::string text;
    if (!TVPUtf16ToUtf8(text, value.AsStdString()) ||
        text.find('\0') != std::string::npos) Fail("Invalid artwork UTF-8 path");
    return text;
}

ttstr Wide(const std::string &value)
{
    if (value.empty()) return ttstr();
    tjs_string text;
    if (!TVPUtf8ToUtf16(text, value)) Fail("Invalid artwork UTF-8 text");
    return ttstr(text);
}

std::string Lower(std::string value)
{
    for (char &c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return value;
}

bool SafeComponent(const std::string &name)
{
    if (name.empty() || name.size() > 256 || name == "." || name == ".." ||
        name.front() == '.') return false;
    for (unsigned char c : name)
        if (c < 32 || c == 127 || c == '/' || c == '\\' || c == ':' || c == '>')
            return false;
    return true;
}

bool SafeRelative(const std::string &path)
{
    if (path.empty() || path.size() > 1024) return false;
    size_t begin = 0;
    while (begin < path.size()) {
        const size_t end = path.find('/', begin);
        if (!SafeComponent(path.substr(begin, end == std::string::npos ? end : end - begin)))
            return false;
        if (end == std::string::npos) return true;
        begin = end + 1;
    }
    return false;
}

bool StartsWith(const std::string &value, const std::string &prefix)
{
    return value.size() >= prefix.size() &&
        Lower(value.substr(0, prefix.size())) == Lower(prefix);
}

std::string NativePath(std::string path)
{
    if (StartsWith(path, "file://?/")) path.erase(0, 9);
    else if (StartsWith(path, "file://")) path.erase(0, 7);
    if (StartsWith(path, "/sdmc:/")) path.erase(0, 1);
    return path;
}

bool ImageExtension(const std::string &name)
{
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos) return false;
    const std::string ext = Lower(name.substr(dot));
    return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".jif" ||
        ext == ".bmp" || ext == ".dib" ||
        ext == ".webp" || ext == ".tlg" || ext == ".tlg5" || ext == ".tlg6";
}

bool CachePath(const std::string &path, const std::string &role)
{
    if (!StartsWith(path, CachePrefix)) return false;
    const std::string name = Lower(path.substr(std::strlen(CachePrefix)));
    const std::string prefix = role + "-";
    if (name.size() != prefix.size() + 16 + 4 || name.compare(0, prefix.size(), prefix) ||
        name.substr(name.size() - 4) != ".png") return false;
    for (size_t i = prefix.size(); i < prefix.size() + 16; ++i)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'a' && name[i] <= 'f'))) return false;
    return true;
}

void RequireLauncher()
{
    if (!IsLauncherArtworkMode()) Fail("Artwork is only available in the launcher");
}

void MakeDirectory(const std::string &path)
{
    if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST)
        Fail("Cannot create launcher artwork directory");
    // EEXIST can also name a regular file. Opening the directory checks it
    // without relying on fsdev's stat implementation.
    DIR *dir = opendir(path.c_str());
    if (!dir) Fail("Launcher artwork path is not a directory");
    closedir(dir);
}

int Priority(const std::string &name)
{
    // The displayed archive name is "archive.xp3 > member/path". Rank the
    // member, including its first component, just like a loose image path.
    const size_t member = name.find(" > ");
    const std::string lower = "/" + Lower(member == std::string::npos ? name : name.substr(member + 3));
    if (lower.find("/cg") != std::string::npos || lower.find("/ev") != std::string::npos)
        return 0;
    if (lower.find("/bg") != std::string::npos || lower.find("/background") != std::string::npos)
        return 1;
    if (lower.find("/title") != std::string::npos || lower.find("/cover") != std::string::npos)
        return 2;
    return 3;
}

struct Candidate { std::string name, source; };
struct Scan {
    std::vector<Candidate> items;
    std::vector<std::string> warnings;
    size_t visited = 0, archives = 0;
    bool truncated = false;
    void Warn(const std::string &text) {
        if (warnings.size() < 24) warnings.push_back(text);
    }
    void Add(const std::string &name, const std::string &source) {
        if (items.size() < MaxCandidates) items.push_back({name, source});
        else truncated = true;
    }
};

uint64_t Read64(const unsigned char *p)
{
    uint64_t value = 0;
    for (unsigned i = 0; i < 8; ++i) value |= uint64_t(p[i]) << (i * 8);
    return value;
}

std::string NormalizedMember(const ttstr &value)
{
    ttstr name = value;
    tTVPArchive::NormalizeInArchiveStorageName(name);
    return Utf8(name);
}

bool SafeImageSegments(const unsigned char *info, const unsigned char *segments,
                       size_t segmentBytes, uint64_t archiveBytes)
{
    const uint64_t limit = 32 * 1024 * 1024;
    const uint64_t original = Read64(info + 4);
    if (!original || original > limit) return false;
    uint64_t originalTotal = 0, packedTotal = 0;
    for (size_t at = 0; at < segmentBytes; at += 28) {
        const unsigned char *segment = segments + at;
        const unsigned flags = unsigned(segment[0]) | (unsigned(segment[1]) << 8) |
            (unsigned(segment[2]) << 16) | (unsigned(segment[3]) << 24);
        const uint64_t start = Read64(segment + 4), size = Read64(segment + 12), packed = Read64(segment + 20);
        if ((flags & 7) > 1 || !size || size > limit - originalTotal || !packed || packed > limit - packedTotal ||
            start > archiveBytes || packed > archiveBytes - start || (!(flags & 7) && size != packed)) return false;
        originalTotal += size;
        packedTotal += packed;
    }
    return originalTotal == original;
}

// Bound index allocations before invoking the existing XP3 parser. Validate
// its chunk lengths as well: its normal game path trusts these index fields.
bool SafeIndexChunks(const unsigned char *data, size_t size, bool nested,
                     size_t &entryCount, uint64_t archiveBytes,
                     const std::string *member, bool &memberFound)
{
    size_t at = 0;
    bool info = false, segments = false, hash = false;
    const unsigned char *firstInfo = nullptr, *firstSegments = nullptr;
    size_t firstSegmentBytes = 0;
    while (at < size) {
        if (size - at < 12) return false;
        const uint64_t count = Read64(data + at + 4);
        if (count > size - at - 12) return false;
        const unsigned char *payload = data + at + 12;
        if (!nested && std::memcmp(data + at, "File", 4) == 0) {
            if (++entryCount > 32768 || !SafeIndexChunks(payload, size_t(count), true, entryCount,
                    archiveBytes, member, memberFound))
                return false;
        } else if (nested && std::memcmp(data + at, "info", 4) == 0) {
            if (count < 22) return false;
            const unsigned nameLength = unsigned(payload[20]) | (unsigned(payload[21]) << 8);
            if (nameLength >= 32768 || uint64_t(nameLength) * 2 + 22 > count) return false;
            if (!info) firstInfo = payload;
            info = true;
        } else if (nested && std::memcmp(data + at, "segm", 4) == 0) {
            if (!count || count % 28) return false;
            if (!segments) { firstSegments = payload; firstSegmentBytes = size_t(count); }
            segments = true;
        } else if (nested && std::memcmp(data + at, "adlr", 4) == 0) {
            if (count < 4) return false;
            hash = true;
        }
        at += 12 + size_t(count);
    }
    if (!nested) return true;
    if (!(info && segments && hash)) return false;
    if (member) {
        const unsigned length = unsigned(firstInfo[20]) | (unsigned(firstInfo[21]) << 8);
        tjs_string name;
        name.reserve(length);
        for (unsigned i = 0; i < length; ++i)
            name.push_back(tjs_char(unsigned(firstInfo[22 + i * 2]) | (unsigned(firstInfo[23 + i * 2]) << 8)));
        if (NormalizedMember(ttstr(name)) == *member) {
            memberFound = true;
            if (!SafeImageSegments(firstInfo, firstSegments, firstSegmentBytes, archiveBytes)) return false;
        }
    }
    return true;
}

bool SafeArchiveIndex(const std::string &path, const std::string *member = nullptr)
{
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(path.c_str(), "rb"), fclose);
    if (!file) return false;
    if (fseek(file.get(), 0, SEEK_END) != 0) return false;
    const long end = ftell(file.get());
    if (end < 0 || fseek(file.get(), 0, SEEK_SET) != 0) return false;
    const uint64_t archiveBytes = uint64_t(end);
    unsigned char magic[11], next[8];
    const unsigned char expected[11] = {'X','P','3',13,10,32,10,26,139,103,1};
    if (fread(magic, 1, 11, file.get()) != 11 || std::memcmp(magic, expected, 11)) return false;
    size_t total = 0, entries = 0;
    bool memberFound = false;
    std::set<uint64_t> offsets;
    for (unsigned block = 0; block < 32; ++block) {
        if (fread(next, 1, 8, file.get()) != 8) return false;
        const uint64_t offset = Read64(next);
        if (!offsets.insert(offset).second || offset > uint64_t(std::numeric_limits<long>::max()) ||
            fseek(file.get(), long(offset), SEEK_SET) != 0) return false;
        const int flag = fgetc(file.get());
        if (flag == EOF || fread(next, 1, 8, file.get()) != 8) return false;
        const uint64_t packed = Read64(next);
        uint64_t original = packed;
        if ((flag & 7) == 1) {
            if (fread(next, 1, 8, file.get()) != 8) return false;
            original = Read64(next);
        } else if ((flag & 7) != 0) return false;
        if (!original || original > MaxIndexBytes - total || packed > MaxIndexBytes) return false;
        total += size_t(original);
        std::vector<unsigned char> input(size_t(packed), 0), decoded;
        if (fread(input.data(), 1, input.size(), file.get()) != input.size()) return false;
        const unsigned char *data = input.data();
        if ((flag & 7) == 1) {
            decoded.resize(size_t(original));
            uLongf outputSize = uLongf(original);
            if (uncompress(decoded.data(), &outputSize, input.data(), uLong(input.size())) != Z_OK ||
                outputSize != original) return false;
            data = decoded.data();
        }
        if (!SafeIndexChunks(data, size_t(original), false, entries, archiveBytes, member, memberFound)) return false;
        if (!(flag & 0x80)) return !member || memberFound;
    }
    return false;
}

void ScanArchive(const std::string &path, const std::string &display, Scan &scan)
{
    if (scan.archives++ >= MaxArchives) { scan.truncated = true; return; }
    if (!SafeArchiveIndex(path)) {
        scan.Warn(display + ": unsupported, damaged, or excessive XP3 index");
        return;
    }
    try {
        std::unique_ptr<tTVPXP3Archive> archive(new tTVPXP3Archive(Wide(path)));
        std::vector<std::string> names;
        for (tjs_uint i = 0; i < archive->GetCount(); ++i) {
            if (++scan.visited > MaxVisited) { scan.truncated = true; break; }
            const std::string name = Utf8(archive->GetName(i));
            if (SafeRelative(name) && ImageExtension(name)) names.push_back(name);
        }
        std::stable_sort(names.begin(), names.end(), [](const auto &a, const auto &b) {
            return Priority(a) != Priority(b) ? Priority(a) < Priority(b) : Lower(a) < Lower(b);
        });
        for (const auto &name : names) scan.Add(display + " > " + name, path + ">" + name);
    } catch (...) {
        scan.Warn(display + ": XP3 artwork index could not be read");
    }
}

void ScanDirectory(const std::string &root, const std::string &relative,
                   unsigned depth, bool archivesAllowed, Scan &scan)
{
    if (scan.visited >= MaxVisited) { scan.truncated = true; return; }
    const std::string directory = root + relative;
    DIR *dir = opendir(directory.c_str());
    if (!dir) { scan.Warn(relative.empty() ? "Artwork folder is unavailable" : relative + ": cannot read directory"); return; }
    std::vector<std::string> names;
    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (!SafeComponent(entry->d_name)) continue;
#ifdef DT_LNK
        if (entry->d_type == DT_LNK) continue;
#endif
        if (++scan.visited > MaxVisited) { scan.truncated = true; break; }
        tjs_string check;
        if (TVPUtf8ToUtf16(check, std::string(entry->d_name))) names.emplace_back(entry->d_name);
    }
    closedir(dir);
    std::stable_sort(names.begin(), names.end(), [](const auto &a, const auto &b) {
        return Priority(a) != Priority(b) ? Priority(a) < Priority(b) : Lower(a) < Lower(b);
    });
    for (const auto &name : names) {
        if (scan.items.size() >= MaxCandidates || scan.visited >= MaxVisited) { scan.truncated = true; break; }
        const std::string part = relative + name, path = root + part;
        DIR *subdir = opendir(path.c_str());
        if (subdir) {
            closedir(subdir);
            if (depth < MaxDepth) ScanDirectory(root, part + "/", depth + 1, archivesAllowed, scan);
            else scan.truncated = true;
        } else if (ImageExtension(name)) {
            scan.Add(part, path);
        } else if (archivesAllowed && name.size() >= 4 && Lower(name.substr(name.size() - 4)) == ".xp3") {
            ScanArchive(path, part, scan);
        }
    }
}

struct Choice { std::string preview, avatar; };
std::map<std::string, Choice> Choices;
bool ChoicesLoaded = false;
uint64_t ChoicesGeneration = 0;
int ActiveSlot = -1;

std::string Escape(const std::string &value)
{
    std::string result;
    for (char c : value) {
        if (c == '\\') result += "\\\\";
        else if (c == '\t') result += "\\t";
        else if (c == '\n') result += "\\n";
        else if (c == '\r') result += "\\r";
        else result += c;
    }
    return result;
}

bool Unescape(const std::string &value, std::string &result)
{
    result.clear();
    for (size_t i = 0; i < value.size(); ++i) {
        char c = value[i];
        if (c == '\\') {
            if (++i == value.size()) return false;
            c = value[i];
            if (c == 't') c = '\t';
            else if (c == 'n') c = '\n';
            else if (c == 'r') c = '\r';
            else if (c != '\\') return false;
        }
        result += c;
    }
    return true;
}

uint64_t Checksum(const std::string &payload)
{
    uint64_t sum = UINT64_C(14695981039346656037);
    for (unsigned char c : payload) { sum ^= c; sum *= UINT64_C(1099511628211); }
    return sum;
}

std::string SlotPath(int slot) { return std::string(ArtworkBase) + "/choices." + std::to_string(slot) + ".tsv"; }

bool ParseUnsigned(const std::string &text, unsigned base, uint64_t &value)
{
    if (text.empty()) return false;
    value = 0;
    for (unsigned char c : text) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
        else return false;
        if (digit >= base || value > (std::numeric_limits<uint64_t>::max() - digit) / base)
            return false;
        value = value * base + digit;
    }
    return true;
}

bool ReadSlot(int slot, uint64_t &generation, std::map<std::string, Choice> &values)
{
    std::unique_ptr<FILE, decltype(&fclose)> file(fopen(SlotPath(slot).c_str(), "rb"), fclose);
    if (!file) return false;
    std::string contents;
    char buffer[4096];
    size_t count;
    while ((count = fread(buffer, 1, sizeof buffer, file.get())) != 0) {
        if (contents.size() + count > MaxPreferencesBytes) return false;
        contents.append(buffer, count);
    }
    if (ferror(file.get())) return false;
    const size_t newline = contents.find('\n');
    if (newline == std::string::npos || newline > 100) return false;
    const std::string header = contents.substr(0, newline), payload = contents.substr(newline + 1);
    const std::string prefix = "KRKRNS_ARTWORK_1\t";
    const size_t separator = header.find('\t', prefix.size());
    uint64_t gen = 0, checksum = 0;
    if (header.compare(0, prefix.size(), prefix) || separator == std::string::npos ||
        header.size() - separator - 1 != 16 ||
        !ParseUnsigned(header.substr(prefix.size(), separator - prefix.size()), 10, gen) || !gen ||
        !ParseUnsigned(header.substr(separator + 1), 16, checksum) || Checksum(payload) != checksum) return false;
    std::map<std::string, Choice> parsed;
    size_t start = 0;
    while (start < payload.size()) {
        const size_t end = payload.find('\n', start);
        if (end == std::string::npos || end - start > 4096 || parsed.size() >= MaxPreferences) return false;
        const std::string line = payload.substr(start, end - start);
        const size_t tab = line.find('\t'), tab2 = tab == std::string::npos ? tab : line.find('\t', tab + 1);
        if (tab == std::string::npos || tab2 == std::string::npos || line.find('\t', tab2 + 1) != std::string::npos) return false;
        std::string folder;
        Choice choice;
        if (!Unescape(line.substr(0, tab), folder) || !Unescape(line.substr(tab + 1, tab2 - tab - 1), choice.preview) ||
            !Unescape(line.substr(tab2 + 1), choice.avatar) || !SafeComponent(folder) ||
            (!choice.preview.empty() && !CachePath(choice.preview, "preview")) ||
            (!choice.avatar.empty() && !CachePath(choice.avatar, "avatar"))) return false;
        tjs_string check;
        if (!TVPUtf8ToUtf16(check, folder) ||
            (!choice.preview.empty() && !TVPUtf8ToUtf16(check, choice.preview)) ||
            (!choice.avatar.empty() && !TVPUtf8ToUtf16(check, choice.avatar)) ||
            !parsed.emplace(folder, choice).second) return false;
        start = end + 1;
    }
    generation = gen;
    values.swap(parsed);
    return true;
}

void LoadChoices()
{
    if (ChoicesLoaded) return;
    ChoicesLoaded = true;
    for (int slot = 0; slot < 2; ++slot) {
        uint64_t generation = 0;
        std::map<std::string, Choice> values;
        if (ReadSlot(slot, generation, values) && generation > ChoicesGeneration) {
            ChoicesGeneration = generation;
            Choices.swap(values);
            ActiveSlot = slot;
        }
    }
}

void CommitChoices(const std::map<std::string, Choice> &values)
{
    if (ChoicesGeneration == std::numeric_limits<uint64_t>::max()) Fail("Artwork preference generation exhausted");
    ArtworkCacheDirectory();
    std::string payload;
    for (const auto &entry : values)
        payload += Escape(entry.first) + "\t" + Escape(entry.second.preview) + "\t" + Escape(entry.second.avatar) + "\n";
    char header[128];
    const uint64_t generation = ChoicesGeneration + 1;
    std::snprintf(header, sizeof header, "KRKRNS_ARTWORK_1\t%llu\t%016llx\n",
        static_cast<unsigned long long>(generation), static_cast<unsigned long long>(Checksum(payload)));
    const std::string contents = std::string(header) + payload;
    if (contents.size() > MaxPreferencesBytes) Fail("Artwork preferences exceed the storage limit");
    const int slot = ActiveSlot == 0 ? 1 : 0;
    const std::string target = SlotPath(slot), temporary = target + ".tmp";
    FILE *file = fopen(temporary.c_str(), "wb");
    if (!file) Fail("Cannot write artwork preferences");
    const bool written = fwrite(contents.data(), 1, contents.size(), file) == contents.size();
    const bool flushed = fflush(file) == 0;
    const bool closed = fclose(file) == 0;
    if (!written || !flushed || !closed) {
        std::remove(temporary.c_str());
        Fail("Artwork preference write failed; previous choices were preserved");
    }
    // fsdev does not need to support POSIX replace-over-existing here. Only
    // retire the inactive slot; the active, checksum-valid generation remains.
    if (std::remove(target.c_str()) != 0 && errno != ENOENT) {
        std::remove(temporary.c_str());
        Fail("Cannot retire the inactive artwork preference slot");
    }
    if (std::rename(temporary.c_str(), target.c_str()) != 0) {
        std::remove(temporary.c_str());
        Fail("Cannot commit artwork preferences; previous choices were preserved");
    }
    ChoicesGeneration = generation;
    ActiveSlot = slot;
}

void SetValue(iTJSDispatch2 *object, const tjs_char *name, const tTJSVariant &value)
{
    object->PropSet(TJS_MEMBERENSURE, name, nullptr, &value, object);
}
} // namespace

bool IsLauncherArtworkMode()
{
#ifdef __SWITCH__
    return !krkrsdl2_game_mode;
#else
    return false;
#endif
}

bool ValidateArtworkSource(const ttstr &folderValue, const ttstr &sourceValue)
{
    try {
        const std::string folder = Utf8(folderValue), source = NativePath(Utf8(sourceValue));
        if (!SafeComponent(folder) || source.size() > 2048) return false;
        const std::string gamePrefix = std::string(KRKRNS_BASE_A) + "/Game/" + folder + "/";
        if (StartsWith(source, CustomPrefix))
            return SafeRelative(source.substr(std::strlen(CustomPrefix))) && ImageExtension(source);
        if (!StartsWith(source, gamePrefix)) return false;
        const std::string relative = source.substr(gamePrefix.size());
        const size_t member = relative.find('>');
        if (member == std::string::npos) return SafeRelative(relative) && ImageExtension(relative);
        const std::string archive = relative.substr(0, member), image = relative.substr(member + 1);
        return SafeRelative(archive) && archive.size() >= 4 && Lower(archive.substr(archive.size() - 4)) == ".xp3" &&
            SafeRelative(image) && ImageExtension(image);
    } catch (...) { return false; }
}

void ValidateArtworkArchiveIndex(const ttstr &archivePath, const ttstr &memberValue)
{
    RequireLauncher();
    bool valid = false;
    try {
        const std::string path = NativePath(Utf8(archivePath));
        if (memberValue.IsEmpty()) valid = SafeArchiveIndex(path);
        else {
            const std::string member = NormalizedMember(memberValue);
            valid = SafeRelative(member) && ImageExtension(member) && SafeArchiveIndex(path, &member);
        }
    } catch (...) { valid = false; }
    if (!valid) Fail("Artwork XP3 index or image segments are damaged, unsupported, or excessive");
}

ttstr ArtworkCacheDirectory()
{
    RequireLauncher();
    MakeDirectory(KRKRNS_BASE_A);
    MakeDirectory(ArtworkBase);
    MakeDirectory(std::string(ArtworkBase) + "/cache");
    return Wide(CachePrefix);
}

iTJSDispatch2 *ScanGameArtwork(const ttstr &folderValue, const ttstr &modeValue)
{
    RequireLauncher();
    const std::string folder = Utf8(folderValue), mode = Utf8(modeValue);
    if (!SafeComponent(folder)) Fail("Invalid artwork game folder");
    if (mode != "game" && mode != "custom") Fail("Artwork source mode must be game or custom");
    Scan scan;
    ScanDirectory(mode == "custom" ? CustomPrefix : std::string(KRKRNS_BASE_A) + "/Game/" + folder + "/",
        "", 0, mode == "game", scan);
    std::stable_sort(scan.items.begin(), scan.items.end(), [](const Candidate &a, const Candidate &b) {
        return Priority(a.name) != Priority(b.name) ? Priority(a.name) < Priority(b.name) : Lower(a.name) < Lower(b.name);
    });
    iTJSDispatch2 *result = TJSCreateDictionaryObject(), *items = TJSCreateArrayObject(), *warnings = TJSCreateArrayObject();
    try {
        for (size_t i = 0; i < scan.items.size(); ++i) {
            iTJSDispatch2 *item = TJSCreateDictionaryObject();
            try {
                SetValue(item, TJS_W("name"), tTJSVariant(Wide(scan.items[i].name)));
                SetValue(item, TJS_W("source"), tTJSVariant(Wide(scan.items[i].source)));
                tTJSVariant value(item, item);
                items->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &value, items);
            } catch (...) { item->Release(); throw; }
            item->Release();
        }
        for (size_t i = 0; i < scan.warnings.size(); ++i) {
            tTJSVariant value(Wide(scan.warnings[i]));
            warnings->PropSetByNum(TJS_MEMBERENSURE, tjs_int(i), &value, warnings);
        }
        SetValue(result, TJS_W("items"), tTJSVariant(items, items));
        SetValue(result, TJS_W("warnings"), tTJSVariant(warnings, warnings));
        SetValue(result, TJS_W("truncated"), tTJSVariant(tjs_int(scan.truncated)));
    } catch (...) { result->Release(); items->Release(); warnings->Release(); throw; }
    items->Release(); warnings->Release();
    KRKRNS_LOG("[artwork] explicit scan mode=%s candidates=%zu archives=%zu truncated=%d",
        mode.c_str(), scan.items.size(), scan.archives, scan.truncated);
    return result;
}

iTJSDispatch2 *GetGameArtworkChoices()
{
    RequireLauncher();
    LoadChoices();
    iTJSDispatch2 *result = TJSCreateDictionaryObject();
    try {
        for (const auto &entry : Choices) {
            iTJSDispatch2 *item = TJSCreateDictionaryObject();
            try {
                SetValue(item, TJS_W("preview"), tTJSVariant(Wide(entry.second.preview)));
                SetValue(item, TJS_W("avatar"), tTJSVariant(Wide(entry.second.avatar)));
                SetValue(result, Wide(entry.first).c_str(), tTJSVariant(item, item));
            } catch (...) { item->Release(); throw; }
            item->Release();
        }
    } catch (...) { result->Release(); throw; }
    return result;
}

bool SetGameArtworkChoice(const ttstr &folderValue, const ttstr &roleValue, const ttstr &pathValue)
{
    RequireLauncher();
    const std::string folder = Utf8(folderValue), role = Utf8(roleValue), path = NativePath(Utf8(pathValue));
    if (!SafeComponent(folder)) Fail("Invalid artwork game folder");
    if (role != "preview" && role != "avatar") Fail("Artwork preference role must be preview or avatar");
    if (!path.empty() && !CachePath(path, role)) Fail("Artwork choice must use the matching launcher thumbnail cache role");
    LoadChoices();
    auto updated = Choices;
    Choice &choice = updated[folder];
    (role == "preview" ? choice.preview : choice.avatar) = path;
    if (choice.preview.empty() && choice.avatar.empty()) updated.erase(folder);
    if (updated.size() > MaxPreferences) Fail("Too many artwork preferences");
    CommitChoices(updated);
    Choices.swap(updated);
    return true;
}

} // namespace krkrns_launcher_artwork
