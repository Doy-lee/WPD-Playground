#define WIN32_MEAN_AND_LEAN
#include <Windows.h>
#include <comdef.h>
#include <stdio.h>

#pragma warning(push)
#pragma warning(disable: 4244) // 'return': conversion from 'int' to 'uint8_t', possible loss of data
extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}
#pragma warning(pop)

#define DQN_PLATFORM_HEADER
#include "External/Dqn.h"

struct Context
{
    DqnLogger          logger;
    DqnMemStack        allocator;
    DqnBuffer<wchar_t> exe_name;
    DqnBuffer<wchar_t> exe_directory;
};

FILE_SCOPE DqnVArray<char> global_logger_buf;
FILE_SCOPE DqnMemStack global_func_local_allocator_;

DqnBuffer<wchar_t> CopyWStringToBuffer(DqnMemStack *allocator, wchar_t const *str_to_copy, int len = -1)
{
    if (len == -1) len = DqnWStr_Len(str_to_copy);

    DqnBuffer<wchar_t> result = {};
    result.len                = len;
    result.str                = DQN_MEMSTACK_PUSH_ARRAY(allocator, wchar_t, len + 1);
    DqnMem_Copy(result.str, str_to_copy, sizeof(*result.str) * len);
    result.data[len] = 0;
    return result;
}

DqnBuffer<char> CopyStringToBuffer(DqnMemStack *allocator, char const *str_to_copy, int len = -1)
{
    if (len == -1) len = DqnStr_Len(str_to_copy);

    DqnBuffer<char> result = {};
    result.len                = len;
    result.str                = DQN_MEMSTACK_PUSH_ARRAY(allocator, char, len + 1);
    DqnMem_Copy(result.str, str_to_copy, sizeof(*result.str) * len);
    result.data[len] = 0;
    return result;
}

char *WCharToUTF8(DqnMemStack *allocator, WCHAR const *wstr, int *result_len = nullptr)
{
    int required_len   = DqnWin32_WCharToUTF8(wstr, nullptr, 0);
    char *result       = DQN_MEMSTACK_PUSH_ARRAY(allocator, char, required_len);
    int convert_result = DqnWin32_WCharToUTF8(wstr, result, required_len);
    DQN_ASSERTM(convert_result != -1, "UTF8 conversion error should never happen.");

    if (result_len) *result_len = required_len;
    return result;
}

wchar_t *UTF8ToWChar(DqnMemStack *allocator, char const *str, int *result_len = nullptr)
{
    int required_len   = DqnWin32_UTF8ToWChar(str, nullptr, 0);
    wchar_t *result    = DQN_MEMSTACK_PUSH_ARRAY(allocator, wchar_t, required_len);
    int convert_result = DqnWin32_UTF8ToWChar(str, result, required_len);
    DQN_ASSERTM(convert_result != -1, "UTF8 conversion error should never happen.");

    if (result_len) *result_len = required_len;
    return result;
}

struct SoundMetadata
{
    DqnBuffer<wchar_t> album;
    DqnBuffer<wchar_t> album_artist;
    DqnBuffer<wchar_t> artist;
    DqnBuffer<wchar_t> date;
    DqnBuffer<wchar_t> disc;
    DqnBuffer<wchar_t> genre;
    DqnBuffer<wchar_t> title;
    DqnBuffer<wchar_t> track;
    DqnBuffer<wchar_t> tracktotal;
};

struct SoundFile
{
    DqnBuffer<wchar_t> path;
    DqnSlice <wchar_t> name;
    DqnSlice <wchar_t> extension; // Slice into file_path
    SoundMetadata      metadata;
};

FILE_SCOPE DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> ReadPlaylistFile(Context *context, wchar_t const *file)
{
    DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> result(DQN_MEGABYTE(8));

    auto DQN_UNIQUE_NAME(mem_region) = global_func_local_allocator_.MemRegionScope();
    usize buf_size = 0;
    u8 *buf        = DqnFile_ReadAll(file, &buf_size, &global_func_local_allocator_);
    if (!buf)
    {
        char const *msg = DQN_LOGGER_W(&context->logger, "DqnFile_ReadAll: Failed, could not read file: %s", WCharToUTF8(&global_func_local_allocator_, file));
        global_logger_buf.Push(msg, DqnStr_Len(msg));
        return result;
    }

    auto *buf_ptr = reinterpret_cast<char *>(buf);

    // Skip UTF8-BOM bytes if present
    if (buf_size > 3 && (u8)buf_ptr[0] == 0xEF && (u8)buf_ptr[1] == 0xBB && (u8)buf_ptr[2] == 0xBF)
        buf_ptr += 3;

    while (char *line = Dqn_EatLine(&buf_ptr, nullptr/*line_len*/))
    {
        if (line[0] == '#')
            continue;

        if (DqnFile_Size(line, nullptr))
        {
            DqnBuffer<wchar_t> file_path = {};
            file_path.str                = UTF8ToWChar(&context->allocator, line, &file_path.len);
            result.GetOrMake(file_path);
        }
        else
        {
            fprintf(stdout, "Could not access file in file system: %s\n", line);
        }
    }

#if 0
    for (DqnVHashTable<DqnBuffer<wchar_t>, SoundFile>::Entry const &entry : result)
        fwprintf(stdout, L"parsed line: %s\n", entry.key.str);
#endif

    return result;
}

FILE_SCOPE bool ExtractSoundMetadata(DqnMemStack *allocator, AVDictionary const *dictionary, SoundMetadata *metadata)
{
    bool atleast_one_entry_filled = false;
    if (!dictionary) return atleast_one_entry_filled;

    AVDictionaryEntry const *entry = av_dict_get(dictionary, "", nullptr, AV_DICT_IGNORE_SUFFIX);
    if (!entry) return atleast_one_entry_filled;

    while (entry)
    {
        DqnBuffer<wchar_t> *dest_buffer = nullptr;
        const auto key                  = DqnSlice<char>(entry->key, DqnStr_Len(entry->key));

        if      (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("album"),        Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->album;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("album_artist"), Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->album_artist;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("artist"),       Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->artist;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("date"),         Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->date;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("disc"),         Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->disc;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("genre"),        Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->genre;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("title"),        Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->title;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("track"),        Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->track;
        else if (DQN_BUFFER_STRCMP(key, DQN_BUFFER_STR_LIT("tracktotal"),   Dqn::IgnoreCase::Yes)) dest_buffer = &metadata->tracktotal;

        if (dest_buffer && dest_buffer->len == 0)
        {
            atleast_one_entry_filled = true;
            const auto value         = DqnSlice<char>(entry->value, DqnStr_Len(entry->value));
            dest_buffer->str = UTF8ToWChar(allocator, value.str, &dest_buffer->len);
        }

        AVDictionaryEntry const *prev_entry = entry;
        entry = av_dict_get(dictionary, "", prev_entry, AV_DICT_IGNORE_SUFFIX);
    }

    return atleast_one_entry_filled;
}

DqnBuffer<wchar_t> AllocateSwprintf(DqnMemStack *allocator, wchar_t const *fmt, ...)
{
    va_list va;
    va_start(va, fmt);
    DqnBuffer<wchar_t> result = {};
    result.len                = vswprintf(nullptr, 0, fmt, va) + 1;
    result.str                = DQN_MEMSTACK_PUSH_ARRAY(allocator, wchar_t, result.len);
    vswprintf(result.str, result.len, fmt, va);
    va_end(va);
    return result;
}

void CheckAllocatorHasZeroAllocations(DqnMemStack const *allocator)
{
    DQN_ASSERTM(!allocator->block->prev_block, "Allocator should not have multiple blocks at zero allocations, just the initial block.");
    DQN_ASSERTM(allocator->block->Usage() == 0, "Allocator has non-zero memory usage: %zu\n", allocator->block->Usage());
}

DqnArray<SoundFile> MakeSoundFiles(Context *context, DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> *playlist)
{
    auto DQN_UNIQUE_NAME(mem_region) = global_func_local_allocator_.MemRegionScope();
    auto *buf   = DQN_MEMSTACK_PUSH_ARRAY(&context->allocator, SoundFile, playlist->num_used_entries);
    auto result = DqnArray<SoundFile>(buf, playlist->num_used_entries);

    for (DqnVHashTable<DqnBuffer<wchar_t>, SoundFile>::Entry const &entry : *playlist)
    {
        auto mem_region = context->allocator.MemRegionScope();

        DqnBuffer<wchar_t> const sound_path = entry.key;
        DqnBuffer<char> sound_path_utf8     = {};
        sound_path_utf8.str                 = WCharToUTF8(&global_func_local_allocator_, sound_path.str);

        AVFormatContext *fmt_context = nullptr;
        if (avformat_open_input(&fmt_context, sound_path_utf8.str, nullptr, nullptr))
        {
            char const *msg = DQN_LOGGER_E(&context->logger, "avformat_open_input: failed to open file: %s", sound_path_utf8.str);
            global_logger_buf.Push(msg, DqnStr_Len(msg));
            continue;
        }
        DQN_DEFER { avformat_close_input(&fmt_context); };

#if 1
        SoundFile sound_file = {};
        sound_file.path = CopyWStringToBuffer(&context->allocator, entry.key.str, entry.key.len);
        for (isize i = sound_file.path.len; i >= 0; --i)
        {
            if (sound_file.path.str[i] == '.')
            {
                sound_file.extension.str = sound_file.path.str + (i + 1);
                sound_file.extension.len = static_cast<int>(sound_file.path.len - i);
                break;
            }
        }

        if (!sound_file.path)
        {
            char const *msg = DQN_LOGGER_E(&context->logger, "Could not figure out the file extension for file path: %s", sound_path_utf8.str);
            global_logger_buf.Push(msg, DqnStr_Len(msg));
            continue;
        }

        for (isize i = sound_file.extension.len; i >= 0; --i)
        {
            if (sound_file.path.str[i] == '\\')
            {
                sound_file.name.str = sound_file.path.str + (i + 1);
                sound_file.name.len = static_cast<int>(sound_file.path.len - i);
                break;
            }
        }

        if (!sound_file.name)
        {
            char const *msg = DQN_LOGGER_E(&context->logger, "Could not figure out the file name for file path: %s", sound_path_utf8.str);
            global_logger_buf.Push(msg, DqnStr_Len(msg));
            continue;
        }

        bool atleast_one_entry_filled = ExtractSoundMetadata(&context->allocator, fmt_context->metadata, &sound_file.metadata);
        DQN_FOR_EACH(i, fmt_context->nb_streams)
        {
            AVStream const *stream = fmt_context->streams[i];
            atleast_one_entry_filled |= ExtractSoundMetadata(&context->allocator, stream->metadata, &sound_file.metadata);
        }

        if (!atleast_one_entry_filled)
        {
            char const *msg = DQN_LOGGER_W(&context->logger, "No metadata could be parsed for file: %s", entry.key.str);
            global_logger_buf.Push(msg, DqnStr_Len(msg));
            continue;
        }

        context->allocator.MemRegionSave(&mem_region);
        result.Push(sound_file);
#else
        av_dump_format(fmt_context, 0, sound_file_path.str, 0);
#endif
    }

    return result;
}

void SanitiseStringForDiskFile(wchar_t *string)
{
    while(*string++)
    {
        switch (string[0])
        {
            case '?':
            case ':':
            case '\\':
            case '/':
            case '<':
            case '>':
            case '*':
            case '|':
            case '"':
                string[0] = ' ';
                break;
        }
    }
}

int main(int, char)
{
    Context context              = {};
    context.logger.no_console    = true;
    context.allocator            = DqnMemStack(DQN_MEGABYTE(16), Dqn::ZeroMem::Yes, 0, DqnMemTracker::All);
    global_func_local_allocator_ = DqnMemStack(DQN_MEGABYTE(1), Dqn::ZeroMem::Yes, 0, DqnMemTracker::All);
    DqnWin32_GetExeNameAndDirectory(&context.allocator, &context.exe_name, &context.exe_directory);
    global_logger_buf.LazyInit(DQN_MEGABYTE(16));

    DqnFile_MakeDir("Input");
    DqnFile_MakeDir("Output");

    i32 num_files    = 0;
    char **dir_files = DqnFile_ListDir(".\\Input\\*", &num_files);
    DQN_DEFER { DqnFile_ListDirFree(dir_files, num_files); };
    DQN_FOR_EACH(dir_index, num_files)
    {
        auto DQN_UNIQUE_NAME(mem_scope) = context.allocator.MemRegionScope();
        auto DQN_UNIQUE_NAME(mem_scope) = global_func_local_allocator_.MemRegionScope();

        char const *playlist_file = dir_files[dir_index];
        DqnBuffer<wchar_t> playlist_file_path = AllocateSwprintf(&context.allocator, L".\\Input\\%s", UTF8ToWChar(&context.allocator, playlist_file));

        DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> playlist = ReadPlaylistFile(&context, playlist_file_path.str);
        DQN_DEFER
        {
            playlist.Free();
        };

        if (playlist.num_used_entries == 0)
        {
            continue;
        }

        DqnArray<SoundFile> sounds = MakeSoundFiles(&context, &playlist);

        isize sounds_to_rel_path_num = sounds.len;
        auto *sounds_to_rel_path_mem = DQN_MEMSTACK_PUSH_ARRAY(&context.allocator, DqnBuffer<wchar_t>, sounds_to_rel_path_num);
        DqnArray<DqnBuffer<wchar_t>> sounds_to_rel_path(sounds_to_rel_path_mem, sounds_to_rel_path_num);

        isize estimated_buf_chars = sounds.len; // for each sound file path, we also need a new line \n
        for (SoundFile &sound_file : sounds)
        {
            CheckAllocatorHasZeroAllocations(&global_func_local_allocator_);
            wchar_t *artist = (sound_file.metadata.artist) ? sound_file.metadata.artist.str : L"_";
            wchar_t *album  = (sound_file.metadata.album)  ? sound_file.metadata.album.str : L"_";
            wchar_t *title  = (sound_file.metadata.title)  ? sound_file.metadata.title.str : sound_file.name.str;

            // NOTE(doyle): Destructive
            SanitiseStringForDiskFile(artist);
            SanitiseStringForDiskFile(album);
            SanitiseStringForDiskFile(title);

            context.allocator.SetAllocMode(DqnMemStack::AllocMode::Head);
            DqnBuffer<wchar_t> rel_path = AllocateSwprintf(&context.allocator, L"Files\\%s\\%s\\%s.%s", artist, album, title, sound_file.extension.str);
            sounds_to_rel_path.Push(rel_path);
            estimated_buf_chars += rel_path.len;

            context.allocator.SetAllocMode(DqnMemStack::AllocMode::Tail);
            DqnBuffer<wchar_t> dest_path = AllocateSwprintf(&context.allocator, L"%s\\Output\\%s", context.exe_directory.str, rel_path.str);
            context.allocator.SetAllocMode(DqnMemStack::AllocMode::Head);
            DQN_DEFER { context.allocator.Pop(dest_path.str); };

            if (DqnFile_GetInfo(dest_path.str, nullptr))
            {
                continue;
            }

            DQN_FOR_EACH(buf_index, dest_path.len)
            {
                if (dest_path.str[buf_index] == '\\')
                {
                    wchar_t tmp = dest_path.str[buf_index+1];
                    dest_path.str[buf_index+1] = 0;
                    CreateDirectoryW(dest_path.str, nullptr);
                    dest_path.str[buf_index+1] = tmp;
                }
            }

            if (!CreateHardLinkW(dest_path.str, sound_file.path.str, nullptr))
            {
                char const *msg = DQN_LOGGER_E(
                    &context.logger,
                    "CreateSymbolicLinkW failed: %s. Could not make symbolic link from: %s -> %s",
                    DqnWin32_GetLastError(),
                    WCharToUTF8(&context.allocator, sound_file.path.str),
                    WCharToUTF8(&context.allocator, dest_path.str));
                global_logger_buf.Push(msg, DqnStr_Len(msg));
            }
        }

        DQN_ASSERT(sounds.len == sounds_to_rel_path.len);
        // Make M3U8 playlist
        {
            DqnArray<char> m3u_buf = {};
            m3u_buf.Reserve(estimated_buf_chars + /*safety_margin*/ 1024);
            for (DqnBuffer<wchar_t> const &output_path : sounds_to_rel_path)
            {
                auto DQN_UNIQUE_NAME(mem_scope) = context.allocator.MemRegionScope();
                DqnBuffer<char> utf8 = {};
                utf8.str = WCharToUTF8(&context.allocator, output_path.str, &utf8.len);

                m3u_buf.Push(utf8.str, utf8.len - 1);
                m3u_buf.Push('\n');
            }
            m3u_buf.Push('\0');

            // NOTE(doyle): len - 1, don't write the null terminating byte
            DqnBuffer<wchar_t> dest_file = AllocateSwprintf(&context.allocator, L"%s\\Output\\%s", context.exe_directory.str, UTF8ToWChar(&context.allocator, playlist_file));
            if (!DqnFile_WriteAll(dest_file.str, reinterpret_cast<u8 *>(m3u_buf.data), (m3u_buf.len - 1) * sizeof(m3u_buf.data[0])))
            {
                char const *msg = DQN_LOGGER_E(&context.logger, "DqnFile_WriteAll failed: Could not write m3u file to destination: %s", WCharToUTF8(&context.allocator, dest_file.str));
                global_logger_buf.Push(msg, DqnStr_Len(msg));
            }
        }
    }

    fprintf(stderr, "%s", global_logger_buf.data);
    return 0;
}
