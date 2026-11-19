#include "archive.h"

#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "7z.h"
#include "7zCrc.h"
#include "miniz.h"

// --- Shared helpers -----------------------------------------------------------

// Names inside archives always use '/'. Archives made by some Windows tools
// store '\\' (invalid in Windows file names anyway, so never part of a name).
static void normalize_separators(std::string* name)
{
    for (size_t i = 0; i < name->size(); i++)
    {
        if ((*name)[i] == '\\')
        {
            (*name)[i] = '/';
        }
    }
}

static bool ends_with_no_case(const char* text, const char* suffix)
{
    size_t text_length = std::strlen(text);
    size_t suffix_length = std::strlen(suffix);
    return text_length >= suffix_length && SDL_strcasecmp(text + text_length - suffix_length, suffix) == 0;
}

ArchiveFormat archive_format(const char* path)
{
    if (path == nullptr)
    {
        return ArchiveFormat::none;
    }
    if (ends_with_no_case(path, ".zip"))
    {
        return ArchiveFormat::zip;
    }
    if (ends_with_no_case(path, ".7z"))
    {
        return ArchiveFormat::seven_zip;
    }
    return ArchiveFormat::none;
}

// Where `marker` (lower case) first appears in `text`, ignoring case; -1 if nowhere.
static int64_t find_no_case(const char* text, const char* marker)
{
    size_t marker_length = std::strlen(marker);
    for (size_t i = 0; text[i] != '\0'; i++)
    {
        size_t j = 0;
        while (j < marker_length && text[i + j] != '\0' && SDL_tolower(text[i + j]) == marker[j])
        {
            j++;
        }
        if (j == marker_length)
        {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

bool archive_split_path(const char* path, std::string* archive_path, std::string* name)
{
    if (path == nullptr)
    {
        return false;
    }
    int64_t zip = find_no_case(path, ".zip#");
    int64_t seven_zip = find_no_case(path, ".7z#");
    size_t extension_length = 0;
    int64_t at = -1;
    if (zip >= 0 && (seven_zip < 0 || zip < seven_zip))
    {
        at = zip;
        extension_length = 4;
    }
    else if (seven_zip >= 0)
    {
        at = seven_zip;
        extension_length = 3;
    }
    if (at < 0)
    {
        return false;
    }
    size_t hash = static_cast<size_t>(at) + extension_length;   // the '#'
    archive_path->assign(path, hash);
    name->assign(path + hash + 1);
    return true;
}

// The output file: written to "<out>.tmp", renamed when complete.
struct OutputFile
{
    SDL_IOStream* io = nullptr;
    std::string   temporary_path;
    bool          failed = false;
};

static bool output_open(OutputFile* output, const char* out_path)
{
    output->temporary_path = out_path;
    output->temporary_path += ".tmp";
    output->io = SDL_IOFromFile(output->temporary_path.c_str(), "wb");
    if (output->io == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't create %s: %s", output->temporary_path.c_str(),
                     SDL_GetError());
        return false;
    }
    return true;
}

static bool output_write(OutputFile* output, const void* data, size_t size)
{
    if (size > 0 && SDL_WriteIO(output->io, data, size) != size)
    {
        if (!output->failed)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't write %s: %s", output->temporary_path.c_str(),
                         SDL_GetError());
        }
        output->failed = true;
    }
    return !output->failed;
}

// Closes the file; renames it into place if `complete`, else removes it.
static bool output_finish(OutputFile* output, const char* out_path, bool complete)
{
    bool closed = SDL_CloseIO(output->io);
    output->io = nullptr;
    if (complete && !closed)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't finish %s: %s", output->temporary_path.c_str(),
                     SDL_GetError());
    }
    if (complete && closed && !output->failed)
    {
        if (SDL_RenamePath(output->temporary_path.c_str(), out_path))
        {
            return true;
        }
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't rename %s to %s: %s",
                     output->temporary_path.c_str(), out_path, SDL_GetError());
    }
    SDL_RemovePath(output->temporary_path.c_str());
    return false;
}

// --- zip (miniz) --------------------------------------------------------------

// miniz reads through this callback, so the archive is opened with SDL
// (UTF-8 paths on every platform) instead of miniz's own fopen.
static size_t zip_read(void* opaque, mz_uint64 offset, void* buffer, size_t size)
{
    SDL_IOStream* io = static_cast<SDL_IOStream*>(opaque);
    if (offset > static_cast<mz_uint64>(INT64_MAX) ||
        SDL_SeekIO(io, static_cast<Sint64>(offset), SDL_IO_SEEK_SET) < 0)
    {
        return 0;
    }
    return SDL_ReadIO(io, buffer, size);
}

static size_t zip_write(void* opaque, mz_uint64 offset, const void* data, size_t size)
{
    (void)offset;   // miniz extracts in order: offsets only grow by `size`
    OutputFile* output = static_cast<OutputFile*>(opaque);
    return output_write(output, data, size) ? size : 0;
}

struct ZipArchive
{
    SDL_IOStream*  io = nullptr;
    mz_zip_archive zip = {};
    bool           open = false;
};

static bool zip_open(const char* path, ZipArchive* archive)
{
    archive->io = SDL_IOFromFile(path, "rb");
    if (archive->io == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't open %s: %s", path, SDL_GetError());
        return false;
    }
    Sint64 size = SDL_GetIOSize(archive->io);
    if (size < 0)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't get the size of %s: %s", path, SDL_GetError());
        return false;
    }
    mz_zip_zero_struct(&archive->zip);
    archive->zip.m_pRead = zip_read;
    archive->zip.m_pIO_opaque = archive->io;
    if (!mz_zip_reader_init(&archive->zip, static_cast<mz_uint64>(size), 0))
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: %s is not a zip file we can read (%s)", path,
                     mz_zip_get_error_string(mz_zip_get_last_error(&archive->zip)));
        return false;
    }
    archive->open = true;
    return true;
}

static void zip_close(ZipArchive* archive)
{
    if (archive->open)
    {
        mz_zip_reader_end(&archive->zip);
        archive->open = false;
    }
    if (archive->io != nullptr)
    {
        SDL_CloseIO(archive->io);
        archive->io = nullptr;
    }
}

static bool zip_entry(ZipArchive* archive, mz_uint index, ArchiveEntry* entry)
{
    mz_zip_archive_file_stat stat = {};
    if (!mz_zip_reader_file_stat(&archive->zip, index, &stat))
    {
        return false;
    }
    entry->name = stat.m_filename;
    normalize_separators(&entry->name);
    entry->is_directory = stat.m_is_directory != 0;
    while (!entry->name.empty() && entry->name[entry->name.size() - 1] == '/')
    {
        entry->name.pop_back();   // folders are stored as "name/"
    }
    entry->size = stat.m_uncomp_size;
    entry->crc32 = stat.m_crc32;
    entry->has_crc32 = !entry->is_directory;
    return true;
}

static bool zip_list(const char* path, std::vector<ArchiveEntry>* entries)
{
    ZipArchive archive = {};
    bool ok = zip_open(path, &archive);
    if (ok)
    {
        mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
        for (mz_uint i = 0; i < count; i++)
        {
            ArchiveEntry entry = {};
            if (zip_entry(&archive, i, &entry))
            {
                entries->push_back(entry);
            }
        }
    }
    zip_close(&archive);
    return ok;
}

static bool zip_extract(const char* path, const char* name, const char* out_path)
{
    ZipArchive archive = {};
    if (!zip_open(path, &archive))
    {
        zip_close(&archive);
        return false;
    }

    // Found by our own (normalized) names, the ones archive_list gave out.
    mz_uint count = mz_zip_reader_get_num_files(&archive.zip);
    mz_uint index = count;
    for (mz_uint i = 0; i < count && index == count; i++)
    {
        ArchiveEntry entry = {};
        if (zip_entry(&archive, i, &entry) && !entry.is_directory && entry.name == name)
        {
            index = i;
        }
    }
    if (index == count)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: %s has no file \"%s\"", path, name);
        zip_close(&archive);
        return false;
    }

    OutputFile output = {};
    bool ok = output_open(&output, out_path);
    if (ok)
    {
        // miniz checks the CRC itself (MZ_ZIP_CRC_CHECK_FAILED).
        bool extracted = mz_zip_reader_extract_to_callback(&archive.zip, index, zip_write, &output, 0) != 0;
        if (!extracted && !output.failed)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't extract \"%s\" from %s (%s)", name, path,
                         mz_zip_get_error_string(mz_zip_get_last_error(&archive.zip)));
        }
        ok = output_finish(&output, out_path, extracted);
    }
    zip_close(&archive);
    return ok;
}

// --- 7z (LZMA SDK) --------------------------------------------------------------

// The SDK reads through an ISeekInStream "interface": a struct of function
// pointers it passes back to them. Ours is the first member, so the pointer
// the SDK hands back is also a pointer to the whole SevenZipFile.
struct SevenZipFile
{
    ISeekInStream vt = {};
    SDL_IOStream* io = nullptr;
};

static const SevenZipFile* seven_zip_file(ISeekInStreamPtr stream)
{
    return static_cast<const SevenZipFile*>(static_cast<const void*>(stream));
}

static SRes seven_zip_read(ISeekInStreamPtr stream, void* buffer, size_t* size)
{
    SDL_IOStream* io = seven_zip_file(stream)->io;
    size_t wanted = *size;
    *size = SDL_ReadIO(io, buffer, wanted);
    if (*size == 0 && wanted > 0 && SDL_GetIOStatus(io) == SDL_IO_STATUS_ERROR)
    {
        return SZ_ERROR_READ;
    }
    return SZ_OK;   // 0 bytes read = end of the file, as the SDK expects
}

static SRes seven_zip_seek(ISeekInStreamPtr stream, Int64* position, ESzSeek origin)
{
    SDL_IOWhence whence = SDL_IO_SEEK_SET;
    if (origin == SZ_SEEK_CUR)
    {
        whence = SDL_IO_SEEK_CUR;
    }
    else if (origin == SZ_SEEK_END)
    {
        whence = SDL_IO_SEEK_END;
    }
    Sint64 result = SDL_SeekIO(seven_zip_file(stream)->io, *position, whence);
    if (result < 0)
    {
        return SZ_ERROR_READ;
    }
    *position = result;
    return SZ_OK;
}

static void* seven_zip_alloc(ISzAllocPtr allocator, size_t size)
{
    (void)allocator;
    return size == 0 ? nullptr : std::malloc(size);
}

static void seven_zip_free(ISzAllocPtr allocator, void* address)
{
    (void)allocator;
    std::free(address);
}

static const ISzAlloc SEVEN_ZIP_ALLOCATOR = {seven_zip_alloc, seven_zip_free};

// The SDK's read-ahead buffer (the same size its own 7z tool uses).
constexpr size_t SEVEN_ZIP_LOOK_BUFFER = 256 * 1024;

struct SevenZipArchive
{
    SevenZipFile file = {};
    CLookToRead2 look = {};
    CSzArEx      db = {};
    bool         db_open = false;
};

static const char* seven_zip_error(SRes result)
{
    switch (result)
    {
    case SZ_ERROR_DATA:        return "damaged data";
    case SZ_ERROR_MEM:         return "out of memory";
    case SZ_ERROR_CRC:         return "CRC mismatch";
    case SZ_ERROR_UNSUPPORTED: return "unsupported compression method";
    case SZ_ERROR_NO_ARCHIVE:  return "not a 7z file";
    case SZ_ERROR_ARCHIVE:     return "damaged archive";
    case SZ_ERROR_INPUT_EOF:   return "file ends too soon";
    case SZ_ERROR_READ:        return "read error";
    default:                   return "error";
    }
}

static bool seven_zip_open(const char* path, SevenZipArchive* archive)
{
    archive->file.io = SDL_IOFromFile(path, "rb");
    if (archive->file.io == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't open %s: %s", path, SDL_GetError());
        return false;
    }
    archive->file.vt.Read = seven_zip_read;
    archive->file.vt.Seek = seven_zip_seek;

    LookToRead2_CreateVTable(&archive->look, 0);
    archive->look.realStream = &archive->file.vt;
    archive->look.buf = static_cast<Byte*>(std::malloc(SEVEN_ZIP_LOOK_BUFFER));
    if (archive->look.buf == nullptr)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: out of memory opening %s", path);
        return false;
    }
    archive->look.bufSize = SEVEN_ZIP_LOOK_BUFFER;
    LookToRead2_INIT(&archive->look)

    CrcGenerateTable();   // the SDK's CRC table; filling it again is harmless
    SzArEx_Init(&archive->db);
    archive->db_open = true;   // SzArEx_Free is safe after a failed open
    SRes result = SzArEx_Open(&archive->db, &archive->look.vt, &SEVEN_ZIP_ALLOCATOR, &SEVEN_ZIP_ALLOCATOR);
    if (result != SZ_OK)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't read %s as 7z (%s)", path, seven_zip_error(result));
        return false;
    }
    return true;
}

static void seven_zip_close(SevenZipArchive* archive)
{
    if (archive->db_open)
    {
        SzArEx_Free(&archive->db, &SEVEN_ZIP_ALLOCATOR);
        archive->db_open = false;
    }
    std::free(archive->look.buf);
    archive->look.buf = nullptr;
    if (archive->file.io != nullptr)
    {
        SDL_CloseIO(archive->file.io);
        archive->file.io = nullptr;
    }
}

static void append_utf8(uint32_t code, std::string* out)
{
    if (code < 0x80)
    {
        out->push_back(static_cast<char>(code));
    }
    else if (code < 0x800)
    {
        out->push_back(static_cast<char>(0xC0 | (code >> 6)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else if (code < 0x10000)
    {
        out->push_back(static_cast<char>(0xE0 | (code >> 12)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
    else
    {
        out->push_back(static_cast<char>(0xF0 | (code >> 18)));
        out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
    }
}

// 7z stores names as UTF-16 (little endian, zero-terminated).
static void utf16_to_utf8(const UInt16* text, std::string* out)
{
    out->clear();
    for (size_t i = 0; text[i] != 0; i++)
    {
        uint32_t code = text[i];
        bool high = code >= 0xD800 && code <= 0xDBFF;
        if (high && text[i + 1] >= 0xDC00 && text[i + 1] <= 0xDFFF)
        {
            code = 0x10000 + ((code - 0xD800) << 10) + (static_cast<uint32_t>(text[i + 1]) - 0xDC00);
            i++;
        }
        else if (code >= 0xD800 && code <= 0xDFFF)
        {
            code = 0xFFFD;   // a lone surrogate: the replacement character
        }
        append_utf8(code, out);
    }
}

static void seven_zip_entry(const CSzArEx* db, UInt32 index, std::vector<UInt16>* name_buffer, ArchiveEntry* entry)
{
    size_t length = SzArEx_GetFileNameUtf16(db, index, nullptr);   // with the terminating zero
    name_buffer->assign(length + 1, 0);
    SzArEx_GetFileNameUtf16(db, index, name_buffer->data());
    utf16_to_utf8(name_buffer->data(), &entry->name);
    normalize_separators(&entry->name);
    entry->is_directory = SzArEx_IsDir(db, index) != 0;
    entry->size = SzArEx_GetFileSize(db, index);
    entry->has_crc32 = SzBitWithVals_Check(&db->CRCs, index);
    entry->crc32 = entry->has_crc32 ? db->CRCs.Vals[index] : 0;
}

static bool seven_zip_list(const char* path, std::vector<ArchiveEntry>* entries)
{
    SevenZipArchive archive = {};
    bool ok = seven_zip_open(path, &archive);
    if (ok)
    {
        std::vector<UInt16> name_buffer;
        for (UInt32 i = 0; i < archive.db.NumFiles; i++)
        {
            ArchiveEntry entry = {};
            seven_zip_entry(&archive.db, i, &name_buffer, &entry);
            entries->push_back(entry);
        }
    }
    seven_zip_close(&archive);
    return ok;
}

static bool seven_zip_extract(const char* path, const char* name, const char* out_path)
{
    SevenZipArchive archive = {};
    if (!seven_zip_open(path, &archive))
    {
        seven_zip_close(&archive);
        return false;
    }

    std::vector<UInt16> name_buffer;
    UInt32 index = archive.db.NumFiles;
    for (UInt32 i = 0; i < archive.db.NumFiles && index == archive.db.NumFiles; i++)
    {
        ArchiveEntry entry = {};
        seven_zip_entry(&archive.db, i, &name_buffer, &entry);
        if (!entry.is_directory && entry.name == name)
        {
            index = i;
        }
    }
    if (index == archive.db.NumFiles)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: %s has no file \"%s\"", path, name);
        seven_zip_close(&archive);
        return false;
    }

    // The whole solid block the file is in gets decoded into memory.
    UInt32 folder = archive.db.FileToFolder[index];
    if (folder != static_cast<UInt32>(-1))   // -1: an empty file, no block
    {
        UInt64 block = SzAr_GetFolderUnpackSize(&archive.db.db, folder);
        if (block > ARCHIVE_7Z_MAX_BLOCK || block > SIZE_MAX)
        {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
                         "Archive: \"%s\" in %s is in a %llu MiB solid block, more than we decode in memory "
                         "(%llu MiB); recompress the archive non-solid (7z a -ms=off)",
                         name, path, static_cast<unsigned long long>(block / (1024 * 1024)),
                         static_cast<unsigned long long>(ARCHIVE_7Z_MAX_BLOCK / (1024 * 1024)));
            seven_zip_close(&archive);
            return false;
        }
    }

    UInt32 block_index = 0xFFFFFFFF;   // no block cached yet
    Byte*  block_data = nullptr;
    size_t block_size = 0;
    size_t offset = 0;
    size_t size = 0;
    // Checks the file's CRC (SZ_ERROR_CRC).
    SRes result = SzArEx_Extract(&archive.db, &archive.look.vt, index, &block_index, &block_data, &block_size,
                                 &offset, &size, &SEVEN_ZIP_ALLOCATOR, &SEVEN_ZIP_ALLOCATOR);
    bool ok = result == SZ_OK;
    if (!ok)
    {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: can't extract \"%s\" from %s (%s)", name, path,
                     seven_zip_error(result));
    }
    else
    {
        OutputFile output = {};
        ok = output_open(&output, out_path);
        if (ok)
        {
            bool written = output_write(&output, block_data + offset, size);
            ok = output_finish(&output, out_path, written);
        }
    }
    std::free(block_data);
    seven_zip_close(&archive);
    return ok;
}

// --- Public functions -------------------------------------------------------------

bool archive_list(const char* archive_path, std::vector<ArchiveEntry>* entries)
{
    entries->clear();
    switch (archive_format(archive_path))
    {
    case ArchiveFormat::zip:       return zip_list(archive_path, entries);
    case ArchiveFormat::seven_zip: return seven_zip_list(archive_path, entries);
    case ArchiveFormat::none:      break;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: %s is not a .zip or .7z file", archive_path);
    return false;
}

bool archive_extract(const char* archive_path, const char* name, const char* out_path)
{
    switch (archive_format(archive_path))
    {
    case ArchiveFormat::zip:       return zip_extract(archive_path, name, out_path);
    case ArchiveFormat::seven_zip: return seven_zip_extract(archive_path, name, out_path);
    case ArchiveFormat::none:      break;
    }
    SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Archive: %s is not a .zip or .7z file", archive_path);
    return false;
}
