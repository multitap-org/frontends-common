#include "net_content_folders.h"

#include <algorithm>
#include <cstring>

#include "archive.h"
#include "host_fs.h"
#include "host_log.h"

// ---------------------------------------------------------------------------
// Game folders
// ---------------------------------------------------------------------------

// The folder's own name: "/home/ana/roms/" -> "roms"; a root ("/", "C:/")
// has none, so it's shown whole.
static std::string folder_name(const std::string* path)
{
    size_t end = path->size();
    while (end > 0 && (*path)[end - 1] == '/')
    {
        end--;
    }
    size_t begin = end;
    while (begin > 0 && (*path)[begin - 1] != '/')
    {
        begin--;
    }
    std::string name = path->substr(begin, end - begin);
    if (name.empty() || name.find(':') != std::string::npos)
    {
        name = *path;   // "/" or "C:": not a name a path could hold; replaced below
        for (size_t i = 0; i < name.size(); i++)
        {
            if (name[i] == '/' || name[i] == ':')
            {
                name[i] = '_';
            }
        }
        if (name.empty())
        {
            name = "_";
        }
    }
    return name;
}

static bool root_named(const NetContentFolders* folders, const std::string* name)
{
    for (size_t i = 0; i < folders->roots.size(); i++)
    {
        if (folders->roots[i].name == *name)
        {
            return true;
        }
    }
    return false;
}

void net_content_set_folders(NetContentFolders* folders, const std::vector<std::string>* paths)
{
    folders->roots.clear();
    folders->listed_path.clear();
    folders->listed.clear();
    for (size_t i = 0; i < paths->size(); i++)
    {
        char canonical[NET_CONTENT_PATH_MAX] = {};
        if (!host_fs_canonical((*paths)[i].c_str(), canonical, sizeof(canonical)))
        {
            host_log(LogLevel::info, "Network: game folder %s isn't there now, not offered", (*paths)[i].c_str());
            continue;
        }
        NetContentRoot root;
        root.path = canonical;
        std::string base = folder_name(&root.path);
        root.name = base;
        for (uint32_t n = 2; root_named(folders, &root.name); n++)
        {
            root.name = base + " " + std::to_string(n);
        }
        folders->roots.push_back(root);
    }
}

// One part of a virtual path, before it goes near the disk.
static bool part_is_safe(const std::string* part)
{
    if (part->empty() || *part == "." || *part == "..")
    {
        return false;
    }
    for (size_t i = 0; i < part->size(); i++)
    {
        uint8_t c = static_cast<uint8_t>((*part)[i]);
        if (c < 0x20 || c == 0x7F || c == '\\' || c == ':')
        {
            return false;
        }
    }
    return true;
}

static void split_parts(const char* path, std::vector<std::string>* parts)
{
    parts->clear();
    std::string part;
    for (const char* cursor = path; *cursor != '\0'; cursor++)
    {
        if (*cursor == '/')
        {
            parts->push_back(part);
            part.clear();
        }
        else
        {
            part.push_back(*cursor);
        }
    }
    parts->push_back(part);
}

// An archive's entry name without the '/' some zips end folder names with.
static std::string entry_name(const ArchiveEntry* entry)
{
    std::string name = entry->name;
    while (!name.empty() && name.back() == '/')
    {
        name.pop_back();
    }
    return name;
}

// What `inner` is inside the archive: a file, a folder (stored, or implied by
// a file under it), or nothing.
static bool archive_find(const std::vector<ArchiveEntry>* entries, const std::string* inner, NetContentKind* kind)
{
    std::string prefix = *inner + "/";
    for (size_t i = 0; i < entries->size(); i++)
    {
        std::string name = entry_name(&(*entries)[i]);
        if (name == *inner)
        {
            *kind = (*entries)[i].is_directory ? NetContentKind::folder : NetContentKind::file;
            return true;
        }
        if (name.compare(0, prefix.size(), prefix) == 0)
        {
            *kind = NetContentKind::folder;
            return true;
        }
    }
    return false;
}

NetContentStatus net_content_resolve(const NetContentFolders* folders, const char* path, std::string* real_path,
                                     NetContentKind* kind)
{
    if (path[0] == '\0')
    {
        real_path->clear();
        *kind = NetContentKind::folder;
        return NetContentStatus::ok;
    }
    if (std::strlen(path) > NET_CONTENT_PATH_MAX)
    {
        return NetContentStatus::refused;
    }
    std::vector<std::string> parts;
    split_parts(path, &parts);
    for (size_t i = 0; i < parts.size(); i++)
    {
        if (!part_is_safe(&parts[i]))
        {
            host_log(LogLevel::warning, "Network: refused path \"%s\"", path);
            return NetContentStatus::refused;
        }
    }
    const NetContentRoot* root = nullptr;
    for (size_t i = 0; i < folders->roots.size() && root == nullptr; i++)
    {
        if (folders->roots[i].name == parts[0])
        {
            root = &folders->roots[i];
        }
    }
    if (root == nullptr)
    {
        return NetContentStatus::not_found;
    }

    // Down the folders until the end, or an archive file with parts left
    // (the rest is inside it). A folder named "x.zip" is just a folder.
    std::string disk = root->path;
    size_t part = 1;
    bool in_archive = false;
    while (part < parts.size() && !in_archive)
    {
        if (disk.back() != '/')
        {
            disk += "/";
        }
        disk += parts[part];
        part++;
        in_archive = part < parts.size() && archive_format(parts[part - 1].c_str()) != ArchiveFormat::none &&
                     host_fs_exists(disk.c_str()) && !host_fs_is_directory(disk.c_str());
    }
    if (!host_fs_exists(disk.c_str()))
    {
        return NetContentStatus::not_found;
    }
    // After the parts were checked, so this is about symlinks.
    if (!host_fs_is_inside(root->path.c_str(), disk.c_str()))
    {
        host_log(LogLevel::warning, "Network: refused path \"%s\" (leads out of the game folders)", path);
        return NetContentStatus::refused;
    }
    char canonical[NET_CONTENT_PATH_MAX] = {};
    if (!host_fs_canonical(disk.c_str(), canonical, sizeof(canonical)))
    {
        return NetContentStatus::not_found;
    }

    if (!in_archive)
    {
        *real_path = canonical;
        if (host_fs_is_directory(canonical))
        {
            *kind = NetContentKind::folder;
        }
        else
        {
            *kind = archive_format(canonical) != ArchiveFormat::none ? NetContentKind::archive : NetContentKind::file;
        }
        return NetContentStatus::ok;
    }

    std::string inner;
    for (; part < parts.size(); part++)
    {
        inner += inner.empty() ? "" : "/";
        inner += parts[part];
    }
    std::vector<ArchiveEntry> entries;
    if (!archive_list(canonical, &entries))
    {
        return NetContentStatus::failed;
    }
    NetContentKind found = NetContentKind::file;
    if (!archive_find(&entries, &inner, &found))
    {
        return NetContentStatus::not_found;
    }
    *real_path = std::string(canonical) + "#" + inner;
    *kind = found;
    return NetContentStatus::ok;
}

// ---------------------------------------------------------------------------
// Listing
// ---------------------------------------------------------------------------

static char lower_ascii(char c)
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Folders first, then by name ignoring ASCII case (then exactly, so the
// order is the same every time).
static bool entry_before(const NetContentEntry& a, const NetContentEntry& b)
{
    bool a_folder = a.kind == NetContentKind::folder;
    bool b_folder = b.kind == NetContentKind::folder;
    if (a_folder != b_folder)
    {
        return a_folder;
    }
    size_t length = a.name.size() < b.name.size() ? a.name.size() : b.name.size();
    for (size_t i = 0; i < length; i++)
    {
        char x = lower_ascii(a.name[i]);
        char y = lower_ascii(b.name[i]);
        if (x != y)
        {
            return static_cast<uint8_t>(x) < static_cast<uint8_t>(y);
        }
    }
    if (a.name.size() != b.name.size())
    {
        return a.name.size() < b.name.size();
    }
    return a.name < b.name;
}

static void add_folder_once(std::vector<NetContentEntry>* entries, const std::string* name)
{
    for (size_t i = 0; i < entries->size(); i++)
    {
        if ((*entries)[i].kind == NetContentKind::folder && (*entries)[i].name == *name)
        {
            return;
        }
    }
    NetContentEntry entry;
    entry.kind = NetContentKind::folder;
    entry.name = *name;
    entries->push_back(entry);
}

// "a.7z#dir": the files and folders right under "dir" ("" = the top).
static bool list_archive(const std::string* real_path, std::vector<NetContentEntry>* entries)
{
    size_t hash = real_path->find('#');
    std::string archive = hash == std::string::npos ? *real_path : real_path->substr(0, hash);
    std::string inner = hash == std::string::npos ? std::string() : real_path->substr(hash + 1);
    std::vector<ArchiveEntry> stored;
    if (!archive_list(archive.c_str(), &stored))
    {
        return false;
    }
    std::string prefix = inner.empty() ? std::string() : inner + "/";
    for (size_t i = 0; i < stored.size(); i++)
    {
        std::string name = entry_name(&stored[i]);
        if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0)
        {
            continue;
        }
        std::string rest = name.substr(prefix.size());
        size_t slash = rest.find('/');
        if (slash != std::string::npos)
        {
            std::string folder = rest.substr(0, slash);
            add_folder_once(entries, &folder);
        }
        else if (stored[i].is_directory)
        {
            add_folder_once(entries, &rest);
        }
        else
        {
            // An archive inside an archive is just a file: not browsed.
            NetContentEntry entry;
            entry.kind = NetContentKind::file;
            entry.size = stored[i].size;
            entry.name = rest;
            entries->push_back(entry);
        }
    }
    return true;
}

NetContentStatus net_content_list(const NetContentFolders* folders, const char* path,
                                  std::vector<NetContentEntry>* entries)
{
    entries->clear();
    std::string real_path;
    NetContentKind kind = NetContentKind::file;
    NetContentStatus status = net_content_resolve(folders, path, &real_path, &kind);
    if (status != NetContentStatus::ok)
    {
        return status;
    }
    if (path[0] == '\0')
    {
        for (size_t i = 0; i < folders->roots.size(); i++)
        {
            NetContentEntry entry;
            entry.kind = NetContentKind::folder;
            entry.name = folders->roots[i].name;
            entries->push_back(entry);
        }
        return NetContentStatus::ok;   // in the settings' order, not sorted
    }
    if (kind == NetContentKind::file)
    {
        return NetContentStatus::refused;   // not a folder
    }
    if (kind == NetContentKind::archive || real_path.find('#') != std::string::npos)
    {
        if (!list_archive(&real_path, entries))
        {
            return NetContentStatus::failed;
        }
    }
    else
    {
        std::vector<HostFsEntry> listed;
        if (!host_fs_list_directory(real_path.c_str(), &listed))
        {
            return NetContentStatus::failed;
        }
        for (size_t i = 0; i < listed.size(); i++)
        {
            const HostFsEntry* item = &listed[i];
            if (item->name.empty() || item->name[0] == '.')
            {
                continue;   // hidden, as in LOAD GAME
            }
            std::string item_path = real_path + (real_path.back() == '/' ? "" : "/") + item->name;
            // The folder itself is inside (resolved above); only a link can
            // lead out. Left out rather than shown and refused later.
            if (item->is_symlink && !host_fs_is_inside(real_path.c_str(), item_path.c_str()))
            {
                bool inside_a_root = false;
                for (size_t r = 0; r < folders->roots.size() && !inside_a_root; r++)
                {
                    inside_a_root = host_fs_is_inside(folders->roots[r].path.c_str(), item_path.c_str());
                }
                if (!inside_a_root)
                {
                    continue;
                }
            }
            NetContentEntry entry;
            entry.name = item->name;
            entry.size = item->size;
            if (item->is_directory)
            {
                entry.kind = NetContentKind::folder;
                entry.size = 0;
            }
            else
            {
                entry.kind = archive_format(item->name.c_str()) != ArchiveFormat::none ? NetContentKind::archive
                                                                                       : NetContentKind::file;
            }
            entries->push_back(entry);
        }
    }
    std::sort(entries->begin(), entries->end(), entry_before);
    return NetContentStatus::ok;
}

size_t net_content_answer_list(NetContentFolders* folders, const uint8_t* request, size_t size, uint8_t* out,
                               size_t capacity)
{
    uint32_t id = 0;
    uint32_t start = 0;
    std::string path;
    if (!net_content_read_list_request(request, size, &id, &start, &path))
    {
        return 0;
    }
    NetContentStatus status = NetContentStatus::ok;
    if (start == 0 || path != folders->listed_path)
    {
        folders->listed_path.clear();
        status = net_content_list(folders, path.c_str(), &folders->listed);
        if (status == NetContentStatus::ok)
        {
            folders->listed_path = path;
        }
    }
    return net_content_write_list_result(out, capacity, id, status, &folders->listed, start);
}
