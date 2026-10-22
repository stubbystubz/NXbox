// SPDX-FileCopyrightText: Copyright 2026 NSPX
// SPDX-License-Identifier: GPL-3.0-or-later
#include "eden_uwp/ui/mod_archive.h"

#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

namespace EdenXbox::Ui {
namespace {

namespace fs = std::filesystem;

// libarchive reads through these callbacks rather than opening the file itself, so none of its
// own file code runs in the AppContainer; std::ifstream is what the rest of the UI uses.
struct Source {
    std::ifstream in;
    std::int64_t size = 0;
    std::vector<char> buffer = std::vector<char>(256 * 1024);
};

la_ssize_t ReadCallback(struct archive*, void* data, const void** out) {
    auto* source = static_cast<Source*>(data);
    source->in.read(source->buffer.data(), static_cast<std::streamsize>(source->buffer.size()));
    const std::streamsize got = source->in.gcount();
    if (got <= 0) {
        if (source->in.bad()) {
            return -1;
        }
        *out = source->buffer.data();
        return 0; // end of file
    }
    // A short read sets eof/fail; clear it so a later seek still works (7z and zip seek back).
    source->in.clear();
    *out = source->buffer.data();
    return static_cast<la_ssize_t>(got);
}

la_int64_t SeekCallback(struct archive*, void* data, la_int64_t offset, int whence) {
    auto* source = static_cast<Source*>(data);
    std::int64_t target = 0;
    switch (whence) {
    case SEEK_SET:
        target = offset;
        break;
    case SEEK_CUR: {
        source->in.clear();
        const std::int64_t here = static_cast<std::int64_t>(source->in.tellg());
        if (here < 0) {
            return ARCHIVE_FATAL;
        }
        target = here + offset;
        break;
    }
    case SEEK_END:
        target = source->size + offset;
        break;
    default:
        return ARCHIVE_FATAL;
    }
    if (target < 0) {
        return ARCHIVE_FATAL;
    }
    // libarchive may ask for the end of the file to learn its size; clamp, as lseek would allow.
    if (target > source->size) {
        target = source->size;
    }
    source->in.clear();
    source->in.seekg(static_cast<std::streamoff>(target));
    if (!source->in) {
        return ARCHIVE_FATAL;
    }
    return target;
}

la_int64_t SkipCallback(struct archive* a, void* data, la_int64_t request) {
    auto* source = static_cast<Source*>(data);
    source->in.clear();
    const std::int64_t here = static_cast<std::int64_t>(source->in.tellg());
    if (here < 0 || request <= 0) {
        return 0;
    }
    const std::int64_t room = source->size - here;
    const std::int64_t step = request < room ? request : room;
    if (SeekCallback(a, data, here + step, SEEK_SET) < 0) {
        return 0;
    }
    return step;
}

int CloseCallback(struct archive*, void*) {
    return ARCHIVE_OK;
}

std::string ToUtf8(const std::u8string& text) {
    return std::string(text.begin(), text.end());
}

// The entry's name as UTF-8. Windows gets the wide form, which libarchive decodes from each
// format's own charset (UTF-8 flag, OEM code page, RAR's Unicode names) more reliably.
bool EntryName(struct archive_entry* entry, std::string& name) {
#ifdef _WIN32
    if (const wchar_t* wide = archive_entry_pathname_w(entry)) {
        name = ToUtf8(fs::path(wide).u8string());
        return true;
    }
#endif
    if (const char* utf8 = archive_entry_pathname_utf8(entry)) {
        name = utf8;
        return true;
    }
    if (const char* raw = archive_entry_pathname(entry)) {
        name = raw;
        return true;
    }
    return false;
}

// The relative path an entry unpacks to. Returns false when the name must be refused because it
// would leave the destination folder; `out` is empty when there is nothing to unpack.
bool SafeRelativePath(const std::string& name, fs::path& out) {
    std::string normal = name;
    for (char& letter : normal) {
        if (letter == '\\') {
            letter = '/';
        }
    }
    out.clear();
    std::size_t start = 0;
    while (start <= normal.size()) {
        std::size_t end = normal.find('/', start);
        if (end == std::string::npos) {
            end = normal.size();
        }
        std::string part = normal.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") {
            continue; // also drops a leading '/', so absolute names land inside the folder
        }
        if (part == ".." || part.find(':') != std::string::npos) {
            out.clear();
            return false;
        }
        for (char& letter : part) {
            if (letter == '<' || letter == '>' || letter == '"' || letter == '|' ||
                letter == '?' || letter == '*' || static_cast<unsigned char>(letter) < 32) {
                letter = '_';
            }
        }
        // Windows drops trailing dots and spaces, which would merge distinct names.
        while (!part.empty() && (part.back() == '.' || part.back() == ' ')) {
            part.back() = '_';
        }
        out /= fs::path(std::u8string(part.begin(), part.end()));
    }
    return true;
}

std::string LibarchiveError(struct archive* a, const char* fallback) {
    const char* text = archive_error_string(a);
    return (text != nullptr && *text != '\0') ? std::string(text) : std::string(fallback);
}

// Copies the current entry's data into `out_path`.
std::string WriteEntry(struct archive* a, const fs::path& out_path, std::int64_t expected_size,
                       std::uint64_t& written_total, std::uint64_t max_bytes,
                       const std::atomic<bool>& cancel) {
    std::ofstream out(out_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return "cannot create " + ToUtf8(out_path.filename().u8string());
    }
    std::int64_t position = 0;
    for (;;) {
        if (cancel.load()) {
            return "cancelled";
        }
        const void* block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0;
        const int status = archive_read_data_block(a, &block, &size, &offset);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status < ARCHIVE_WARN) {
            return LibarchiveError(a, "the archive is damaged");
        }
        if (offset > position) {
            // A sparse hole: fill it so the file keeps its length.
            const std::int64_t gap = offset - position;
            if (written_total + static_cast<std::uint64_t>(gap) > max_bytes) {
                return "the archive unpacks to too much data";
            }
            out.seekp(static_cast<std::streamoff>(offset));
            position = offset;
            written_total += static_cast<std::uint64_t>(gap);
        }
        if (size > 0) {
            if (written_total + size > max_bytes) {
                return "the archive unpacks to too much data";
            }
            out.write(static_cast<const char*>(block), static_cast<std::streamsize>(size));
            position += static_cast<std::int64_t>(size);
            written_total += size;
        }
    }
    // A file that ends early is the first volume of a split RAR/7z, or a cut-off download;
    // libarchive can report that as a clean end of data, so the length is checked here.
    if (expected_size >= 0 && position < expected_size) {
        return "the archive is incomplete (split in several parts, or cut off)";
    }
    out.close();
    if (!out) {
        return "the disk is full or not writable";
    }
    return {};
}

struct ReaderDeleter {
    void operator()(struct archive* a) const {
        archive_read_free(a);
    }
};

} // namespace

bool IsModArchiveExtension(std::string_view extension) {
    return extension == ".zip" || extension == ".7z" || extension == ".rar";
}

std::string ExtractModArchive(const fs::path& archive_path, const fs::path& destination,
                              const std::atomic<bool>& cancel, std::uint64_t max_bytes) {
    std::error_code ec;
    const std::uintmax_t file_size = fs::file_size(archive_path, ec);
    if (ec) {
        return "cannot read the archive";
    }
    Source source;
    source.size = static_cast<std::int64_t>(file_size);
    source.in.open(archive_path, std::ios::binary);
    if (!source.in) {
        return "cannot open the archive";
    }

    std::unique_ptr<struct archive, ReaderDeleter> reader(archive_read_new());
    if (!reader) {
        return "out of memory";
    }
    struct archive* a = reader.get();
    archive_read_support_format_zip(a); // the seekable reader handles zip64 and odd local headers
    archive_read_support_format_7zip(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);
    archive_read_set_callback_data(a, &source);
    archive_read_set_read_callback(a, ReadCallback);
    archive_read_set_seek_callback(a, SeekCallback);
    archive_read_set_skip_callback(a, SkipCallback);
    archive_read_set_close_callback(a, CloseCallback);
    if (archive_read_open1(a) != ARCHIVE_OK) {
        return "not a zip, 7z or rar file (" + LibarchiveError(a, "unknown format") + ")";
    }

    fs::create_directories(destination, ec);
    std::uint64_t written = 0;
    int unpacked = 0;
    for (;;) {
        if (cancel.load()) {
            return "cancelled";
        }
        struct archive_entry* entry = nullptr;
        const int status = archive_read_next_header(a, &entry);
        if (status == ARCHIVE_EOF) {
            break;
        }
        if (status < ARCHIVE_WARN) {
            const std::string why = LibarchiveError(a, "the archive is damaged");
            // A volume of a multi-part RAR, or a 7z split in pieces, cannot be read alone.
            if (why.find("multivolume") != std::string::npos ||
                why.find("multi-volume") != std::string::npos) {
                return "the archive is split in several parts";
            }
            return why;
        }
        if (archive_entry_is_encrypted(entry)) {
            return "the archive is password protected";
        }
        std::string name;
        if (!EntryName(entry, name)) {
            // Skipping it would leave the mod silently missing a file.
            return "a file name in the archive cannot be read";
        }
        fs::path relative;
        if (!SafeRelativePath(name, relative)) {
            return "the archive has an unsafe path";
        }
        if (relative.empty()) {
            continue;
        }
        const fs::path target = destination / relative;
        const auto type = archive_entry_filetype(entry);
        const bool is_directory =
            type == AE_IFDIR || (!name.empty() && (name.back() == '/' || name.back() == '\\'));
        if (is_directory) {
            fs::create_directories(target, ec);
            continue;
        }
        // Only plain files: links and devices mean nothing to Eden's load folder. A zero type is a
        // zip written by a tool that left the mode out, which is still a file.
        if (type != AE_IFREG && type != 0) {
            continue;
        }
        if (archive_entry_hardlink(entry) != nullptr && archive_entry_size(entry) == 0) {
            // A RAR/zip hard link: copy the file it points at, which was unpacked before it.
            fs::path link_relative;
            std::string link_name = archive_entry_hardlink(entry);
            if (SafeRelativePath(link_name, link_relative) && !link_relative.empty()) {
                fs::create_directories(target.parent_path(), ec);
                fs::copy_file(destination / link_relative, target,
                              fs::copy_options::overwrite_existing, ec);
            }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        const std::int64_t expected =
            archive_entry_size_is_set(entry) ? static_cast<std::int64_t>(archive_entry_size(entry))
                                             : -1;
        const std::string failure = WriteEntry(a, target, expected, written, max_bytes, cancel);
        if (!failure.empty()) {
            return failure;
        }
        ++unpacked;
    }
    if (unpacked == 0) {
        return "the archive is empty";
    }
    return {};
}

} // namespace EdenXbox::Ui
