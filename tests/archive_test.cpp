// Checks archive.h: zip (made here with miniz's writer) and 7z (LZMA2 solid
// and PPMd, tests/data, see its README.md) list the same three files, extract
// them byte for byte, and refuse missing names, folders and damaged files.
//
// Usage: frontend_archive_test <tests/data folder> <scratch folder>
// Exit code 0 = all checks passed. Each failure is printed.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "archive.h"
#include "host_fs.h"
#include "miniz.h"

static int g_failures = 0;

static void check(bool condition, const char* what)
{
    if (!condition)
    {
        std::printf("FAIL %s\n", what);
        g_failures++;
    }
}

static const char* GAME_NAME  = "Game One.sfc";
static const char* SECOND_NAME = "sub/G\xC3\xA2me Two.md";   // "Gâme", UTF-8
static const char* EMPTY_NAME = "empty.txt";

static std::vector<uint8_t> game_data()
{
    std::vector<uint8_t> data(100000);
    for (size_t i = 0; i < data.size(); i++)
    {
        data[i] = static_cast<uint8_t>((i * 7) % 251);
    }
    return data;
}

static std::vector<uint8_t> second_data()
{
    std::string text;
    for (int32_t i = 0; i < 50; i++)
    {
        text += "second file\n";
    }
    return std::vector<uint8_t>(text.begin(), text.end());
}

static uint32_t crc_of(const std::vector<uint8_t>* data)
{
    return static_cast<uint32_t>(mz_crc32(MZ_CRC32_INIT, data->data(), data->size()));
}

// The zip: the three files, an explicit "sub/" folder entry and a name
// stored with a Windows separator.
static bool make_zip(const char* path)
{
    std::vector<uint8_t> game = game_data();
    std::vector<uint8_t> second = second_data();
    const char* windows_text = "backslash";

    mz_zip_archive zip = {};
    mz_zip_zero_struct(&zip);
    bool ok = mz_zip_writer_init_heap(&zip, 0, 0) != 0;
    ok = ok && mz_zip_writer_add_mem(&zip, GAME_NAME, game.data(), game.size(), MZ_DEFAULT_COMPRESSION) != 0;
    ok = ok && mz_zip_writer_add_mem(&zip, "sub/", nullptr, 0, 0) != 0;
    ok = ok && mz_zip_writer_add_mem(&zip, SECOND_NAME, second.data(), second.size(), MZ_DEFAULT_COMPRESSION) != 0;
    ok = ok && mz_zip_writer_add_mem(&zip, EMPTY_NAME, nullptr, 0, 0) != 0;
    ok = ok && mz_zip_writer_add_mem(&zip, "win\\file.txt", windows_text, std::strlen(windows_text), 0) != 0;
    void* buffer = nullptr;
    size_t size = 0;
    ok = ok && mz_zip_writer_finalize_heap_archive(&zip, &buffer, &size) != 0;
    mz_zip_writer_end(&zip);
    ok = ok && host_fs_write_file(path, buffer, size);
    mz_free(buffer);
    return ok;
}

static const ArchiveEntry* find_entry(const std::vector<ArchiveEntry>* entries, const char* name)
{
    for (size_t i = 0; i < entries->size(); i++)
    {
        if ((*entries)[i].name == name)
        {
            return &(*entries)[i];
        }
    }
    return nullptr;
}

static bool file_equals(const char* path, const std::vector<uint8_t>* expected)
{
    uint8_t* data = nullptr;
    size_t size = 0;
    if (!host_fs_read_file(path, &data, &size))
    {
        return false;
    }
    bool same = size == expected->size() && (size == 0 || std::memcmp(data, expected->data(), size) == 0);
    delete[] data;
    return same;
}

static void check_archive(const char* archive, const char* scratch, const char* label)
{
    char what[256] = {};
    std::vector<uint8_t> game = game_data();
    std::vector<uint8_t> second = second_data();
    std::vector<uint8_t> empty;

    std::vector<ArchiveEntry> entries;
    std::snprintf(what, sizeof(what), "%s: lists", label);
    check(archive_list(archive, &entries), what);

    const ArchiveEntry* entry = find_entry(&entries, GAME_NAME);
    std::snprintf(what, sizeof(what), "%s: game file listed with size and CRC", label);
    check(entry != nullptr && !entry->is_directory && entry->size == game.size() && entry->has_crc32 &&
          entry->crc32 == crc_of(&game), what);
    entry = find_entry(&entries, SECOND_NAME);
    std::snprintf(what, sizeof(what), "%s: UTF-8 name in a folder listed", label);
    check(entry != nullptr && entry->size == second.size(), what);
    entry = find_entry(&entries, EMPTY_NAME);
    std::snprintf(what, sizeof(what), "%s: empty file listed", label);
    check(entry != nullptr && entry->size == 0 && !entry->is_directory, what);
    entry = find_entry(&entries, "sub");
    std::snprintf(what, sizeof(what), "%s: stored folder listed without its '/'", label);
    check(entry != nullptr && entry->is_directory, what);

    std::string out = std::string(scratch) + "/" + label + "_game";
    std::snprintf(what, sizeof(what), "%s: game file extracts byte for byte", label);
    check(archive_extract(archive, GAME_NAME, out.c_str()) && file_equals(out.c_str(), &game), what);
    std::snprintf(what, sizeof(what), "%s: no .tmp left behind", label);
    check(!host_fs_exists((out + ".tmp").c_str()), what);

    out = std::string(scratch) + "/" + label + "_second";
    std::snprintf(what, sizeof(what), "%s: UTF-8 name extracts", label);
    check(archive_extract(archive, SECOND_NAME, out.c_str()) && file_equals(out.c_str(), &second), what);

    out = std::string(scratch) + "/" + label + "_empty";
    std::snprintf(what, sizeof(what), "%s: empty file extracts", label);
    check(archive_extract(archive, EMPTY_NAME, out.c_str()) && file_equals(out.c_str(), &empty), what);

    out = std::string(scratch) + "/" + label + "_missing";
    std::snprintf(what, sizeof(what), "%s: a missing name fails, nothing written", label);
    check(!archive_extract(archive, "nope.bin", out.c_str()) && !host_fs_exists(out.c_str()), what);
    std::snprintf(what, sizeof(what), "%s: a folder doesn't extract", label);
    check(!archive_extract(archive, "sub", out.c_str()) && !host_fs_exists(out.c_str()), what);
}

static void test_split()
{
    std::string archive;
    std::string name;
    check(archive_split_path("/r/snes.7z#dir/Game.sfc", &archive, &name) && archive == "/r/snes.7z" &&
          name == "dir/Game.sfc", "split: 7z with a folder");
    check(archive_split_path("C:/R/A.ZIP#x.bin", &archive, &name) && archive == "C:/R/A.ZIP" && name == "x.bin",
          "split: upper-case .ZIP");
    check(archive_split_path("/r/a.zip#", &archive, &name) && archive == "/r/a.zip" && name.empty(),
          "split: the archive's top folder");
    check(archive_split_path("/r/Game #1.zip#a.nes", &archive, &name) && archive == "/r/Game #1.zip" &&
          name == "a.nes", "split: '#' in the archive's own name");
    check(archive_split_path("/r/a.zip#b.7z#c.sfc", &archive, &name) && archive == "/r/a.zip" &&
          name == "b.7z#c.sfc", "split: at the first archive");
    archive = "kept";
    check(!archive_split_path("/r/Game #1.sfc", &archive, &name) && archive == "kept", "split: not an archive path");
    check(!archive_split_path("/r/a.zip", &archive, &name), "split: an archive itself isn't a path into it");
}

static void test_format()
{
    check(archive_format("/r/a.zip") == ArchiveFormat::zip, "format: .zip");
    check(archive_format("/r/A.7Z") == ArchiveFormat::seven_zip, "format: .7Z");
    check(archive_format("/r/a.sfc") == ArchiveFormat::none, "format: .sfc");
    check(archive_format("/r/zip") == ArchiveFormat::none, "format: no extension");
}

static void test_damaged(const char* data_folder, const char* scratch)
{
    std::vector<ArchiveEntry> entries;
    const char* garbage = "this is not an archive at all, just some text";
    std::string bad_zip = std::string(scratch) + "/bad.zip";
    std::string bad_7z = std::string(scratch) + "/bad.7z";
    check(host_fs_write_file(bad_zip.c_str(), garbage, std::strlen(garbage)), "damaged: write bad.zip");
    check(host_fs_write_file(bad_7z.c_str(), garbage, std::strlen(garbage)), "damaged: write bad.7z");
    check(!archive_list(bad_zip.c_str(), &entries), "damaged: text as .zip fails");
    check(!archive_list(bad_7z.c_str(), &entries), "damaged: text as .7z fails");

    // The first half of a real 7z: the header (at the end) is gone.
    std::string source = std::string(data_folder) + "/lzma2_solid.7z";
    uint8_t* data = nullptr;
    size_t size = 0;
    check(host_fs_read_file(source.c_str(), &data, &size), "damaged: read lzma2_solid.7z");
    std::string half = std::string(scratch) + "/half.7z";
    check(host_fs_write_file(half.c_str(), data, size / 2), "damaged: write half.7z");
    delete[] data;
    check(!archive_list(half.c_str(), &entries), "damaged: a cut 7z fails");

    std::string missing = std::string(scratch) + "/missing.zip";
    check(!archive_list(missing.c_str(), &entries), "damaged: a missing file fails");
}

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::printf("usage: %s <tests/data folder> <scratch folder>\n", argv[0]);
        return 2;
    }
    const char* data_folder = argv[1];
    const char* scratch = argv[2];
    check(host_fs_make_directories(scratch), "scratch folder");

    test_format();
    test_split();

    std::string zip = std::string(scratch) + "/made.zip";
    check(make_zip(zip.c_str()), "zip: made with miniz");
    check_archive(zip.c_str(), scratch, "zip");
    std::vector<ArchiveEntry> entries;
    archive_list(zip.c_str(), &entries);
    check(find_entry(&entries, "win/file.txt") != nullptr, "zip: '\\' in a stored name becomes '/'");

    std::string lzma2 = std::string(data_folder) + "/lzma2_solid.7z";
    check_archive(lzma2.c_str(), scratch, "7z_lzma2");
    std::string ppmd = std::string(data_folder) + "/ppmd.7z";
    check_archive(ppmd.c_str(), scratch, "7z_ppmd");

    test_damaged(data_folder, scratch);

    if (g_failures == 0)
    {
        std::printf("archive: all checks passed\n");
        return 0;
    }
    std::printf("archive: %d check(s) failed\n", g_failures);
    return 1;
}
