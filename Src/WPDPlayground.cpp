#define WIN32_MEAN_AND_LEAN
#include <Windows.h>
#include <combaseapi.h>
#include <PortableDevice.h>
#include <PortableDeviceTypes.h>
#include <PortableDeviceApi.h>
#include <comdef.h>
#include <wrl/client.h> // ComPtr
#include <stdio.h>

extern "C"
{
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
}

#define DQN_PLATFORM_HEADER
#include "External/Dqn.h"

using Microsoft::WRL::ComPtr;

struct Context
{
    DqnLogger          logger;
    DqnMemStack        allocator;
    DqnBuffer<wchar_t> exe_name;
    DqnBuffer<wchar_t> exe_directory;
};

struct SelectedDevice
{
    int                num_devices;
    DqnBuffer<wchar_t> device_id;
    DqnBuffer<wchar_t> device_friendly_name;
};

struct State
{
    SelectedDevice          chosen_device;
    ComPtr<IPortableDevice> opened_device;
};

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

void DqnWin32_GetExeNameAndDirectory(DqnMemStack *allocator, DqnBuffer<wchar_t> *exe_name, DqnBuffer<wchar_t> *exe_directory)
{
    if (!exe_name && !exe_directory) return;

    i32 offset_to_last_backslash = -1;
    int exe_buf_len              = 512;
    wchar_t *exe_buf                = nullptr;
    while(offset_to_last_backslash == -1)
    {
        if (exe_buf)
        {
            exe_buf_len += 128;
        }

        exe_buf = (decltype(exe_buf))allocator->Push(exe_buf_len * sizeof(*exe_buf), DqnMemStack::PushType::Tail);
        DQN_DEFER { allocator->Pop(exe_buf); };
        offset_to_last_backslash = DqnWin32_GetExeDirectory(exe_buf, exe_buf_len);
    }

    if (exe_name)
        *exe_name = CopyWStringToBuffer(allocator, exe_buf, offset_to_last_backslash + 1);

    if (exe_directory)
        *exe_directory = CopyWStringToBuffer(allocator, exe_buf, offset_to_last_backslash);
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

struct FileNode
{
    DqnBuffer<wchar_t>  name;
    struct FileNode    *parent;
    struct FileNode    *child;
    int                 num_children;
    struct FileNode    *next;
};

#define HANDLE_COM_ERROR(hresult, failing_function_name, logger) HandleComError(hresult, failing_function_name, logger, DQN_LOGGER_CONTEXT)
FILE_SCOPE void HandleComError(HRESULT hresult, char const *failing_function_name, DqnLogger *logger, DqnLogger::Context log_context)
{
    _com_error com_err(hresult);
    logger->Log(DqnLogger::Type::Error,
                log_context,
                "%s: error %08x: %s",
                failing_function_name,
                hresult,
                com_err.ErrorMessage());
}

// return: num_devices is set to 0 if no devices are available, otherwise the device_id and friendly name is set.
SelectedDevice PromptAndSelectPortableDeviceID(Context *context)
{
    auto mem_region = global_func_local_allocator_.MemRegionBegin();

    SelectedDevice result                         = {};
    HRESULT hresult                               = 0;
    ComPtr<IPortableDeviceManager> device_manager = nullptr;

    if (FAILED(hresult = CoCreateInstance(__uuidof(PortableDeviceManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device_manager))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &context->logger);
        return result;
    }

    DWORD num_devices = 0;
    if (FAILED(hresult = device_manager->GetDevices(nullptr, &num_devices)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDevices", &context->logger);
        return result;
    }

    fprintf(stdout, "Enumerated %u Windows Portable Device(s)\n", num_devices);
    result.num_devices = (int)num_devices;
    if (num_devices == 0)
        return result;

    WCHAR **device_names          = DQN_MEMSTACK_PUSH_ARRAY(&global_func_local_allocator_, WCHAR *, num_devices);
    WCHAR **friendly_device_names = DQN_MEMSTACK_PUSH_ARRAY(&global_func_local_allocator_, WCHAR *, num_devices);
    if (FAILED(hresult = device_manager->GetDevices(device_names, &num_devices)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDevices", &context->logger);
        return result;
    }

    for (isize device_index = 0; device_index < (isize)num_devices; ++device_index)
    {
        WCHAR *name = device_names[device_index];
        DWORD friendly_name_len = 0;
        if (FAILED(device_manager->GetDeviceFriendlyName(name, nullptr, &friendly_name_len)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDeviceFriendlyName", &context->logger);
            return result;
        }

        WCHAR *friendly_name = DQN_MEMSTACK_PUSH_ARRAY(&global_func_local_allocator_, WCHAR, friendly_name_len);
        if (FAILED(device_manager->GetDeviceFriendlyName(name, friendly_name, &friendly_name_len)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDeviceFriendlyName", &context->logger);
            return result;
        }

        char *friendly_name_utf8 = WCharToUTF8(&global_func_local_allocator_, friendly_name, nullptr);

        if (num_devices > 10) fprintf(stdout, "    [%02zu] %s\n", device_index, friendly_name_utf8);
        else                  fprintf(stdout, "    [%zu] %s\n", device_index, friendly_name_utf8);
        friendly_device_names[device_index] = friendly_name;
    }

    int chosen_device_index = 0;
    fprintf(stdout, "\n");
    for (char input_buf[8];;)
    {
        fprintf(stdout, "Please choose a numbered device: ");

        input_buf[0] = 0;
        gets_s(input_buf, DQN_ARRAY_COUNT(input_buf) - 1);
        char *input = DqnChar_SkipWhitespace(input_buf);

        if (!DqnChar_IsDigit(input[0]))
            continue;

        int choice = (int)Dqn_StrToI64(input, DqnStr_Len(input));
        if (choice >= 0 && choice < (int)num_devices)
        {
            chosen_device_index = choice;
            break;
        }
    }

    WCHAR const *chosen_device               = device_names[chosen_device_index];
    WCHAR const *chosen_device_friendly_name = friendly_device_names[chosen_device_index];
    result.device_id                         = CopyWStringToBuffer(&context->allocator, chosen_device);
    result.device_friendly_name              = CopyWStringToBuffer(&context->allocator, chosen_device_friendly_name);

    // Free the memory Windows allocated for us
    for (isize device_index = 0; device_index < num_devices; ++device_index)
        CoTaskMemFree(device_names[device_index]);

    return result;
}

ComPtr<IPortableDevice> OpenPortableDevice(Context *context, wchar_t const *device_id)
{
    ComPtr<IPortableDeviceValues> device_values = nullptr;
    HRESULT hresult                             = 0;

    if (FAILED(hresult = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device_values))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &context->logger);
        return nullptr;
    }

    ComPtr<IPortableDevice> portable_device = nullptr;
    if (FAILED(hresult = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&portable_device))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &context->logger);
        return nullptr;
    }

    if (FAILED(hresult = portable_device->Open(device_id, device_values.Get())))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::Open", &context->logger);
        return nullptr;
    }

    ComPtr<IPortableDevice> result = nullptr;
    if (FAILED(hresult = portable_device->QueryInterface(IID_IPortableDevice, (void **)&result)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::QueryInterface", &context->logger);
        return nullptr;
    }

    return result;
}

struct WPDReadSettings
{
    ComPtr<IPortableDeviceKeyCollection> properties_to_read;
    ComPtr<IPortableDeviceProperties>    device_properties;
    ComPtr<IPortableDeviceContent>       content;
};

FILE_SCOPE bool WPDMakeReadSettings(Context *context, IPortableDevice *portable_device, WPDReadSettings *settings)
{
    HRESULT hresult;
    if (FAILED(hresult = portable_device->Content(&settings->content)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::Content", &context->logger);
        return false;
    }

    if (FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&settings->properties_to_read))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &context->logger);
        return false;
    }

    if (FAILED(settings->properties_to_read->Add(WPD_OBJECT_NAME)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceKeyCollection::Add", &context->logger);
        return false;
    }

    if (FAILED(settings->content->Properties(&settings->device_properties)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceContext::Properties", &context->logger);
        return false;
    }

    return true;
}

void WPDMakeFileTreeRecursively(Context *context, WPDReadSettings *read_settings, WCHAR const *parent_object_id, FileNode *file_node, isize *enum_count, int depth = 0)
{
    (*enum_count)++;
    // Get the file name of the object
    {
        IPortableDeviceKeyCollection *properties_to_read = read_settings->properties_to_read.Get();
        IPortableDeviceProperties *device_properties     = read_settings->device_properties.Get();

        HRESULT hresult = 0;
        ComPtr<IPortableDeviceValues> object_values = nullptr;
        if (SUCCEEDED(hresult = device_properties->GetValues(parent_object_id, properties_to_read, &object_values)))
        {
            WCHAR *object_name = nullptr;
            if (SUCCEEDED(hresult = object_values->GetStringValue(WPD_OBJECT_NAME, &object_name)))
            {
                DQN_DEFER { CoTaskMemFree(object_name); };
                file_node->name = CopyWStringToBuffer(&context->allocator, object_name);
            }
            else
            {
                HANDLE_COM_ERROR(hresult, "IPortableDeviceValues::GetStringValue", &context->logger);
            }
        }
        else
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceKeyCollection::GetValues", &context->logger);
        }
    }

    // Recursively iterate contents
    {
        HRESULT hresult = 0;
        IPortableDeviceContent *content                 = read_settings->content.Get();
        ComPtr<IEnumPortableDeviceObjectIDs> object_ids = nullptr;
        if (FAILED(hresult = content->EnumObjects(0 /*dwFlags ignored*/, parent_object_id, nullptr /*pFilter ignored*/, &object_ids)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceContent::EnumObjects", &context->logger);
            return;
        }

        FileNode *child = nullptr;
        for (; hresult == S_OK;)
        {
            WCHAR *object_id_array[128] = {0};
            DWORD num_fetched           = 0;
            hresult = object_ids->Next(DQN_ARRAY_COUNT(object_id_array), object_id_array, &num_fetched);

            if (SUCCEEDED(hresult))
            {
                for (isize fetch_index = 0; fetch_index < num_fetched; ++fetch_index)
                {
                    if (!child)
                    {
                        file_node->child = DQN_MEMSTACK_PUSH_STRUCT(&context->allocator, FileNode);
                        child            = file_node->child;
                        child->parent    = file_node;
                    }
                    else
                    {
                        child->next   = DQN_MEMSTACK_PUSH_STRUCT(&context->allocator, FileNode);
                        child         = child->next;
                        child->parent = file_node;
                    }

                    WPDMakeFileTreeRecursively(context, read_settings, object_id_array[fetch_index], child, enum_count, depth + 1);
                    CoTaskMemFree(object_id_array[fetch_index]);
                }

                if (num_fetched > 0)
                    child->parent->num_children += num_fetched;

            }
        }
    }
}

FILE_SCOPE bool PromptYesOrNoStdin(char const *yes_no_msg)
{
    for (char input_buf[8];;)
    {
        input_buf[0] = 0;
        fprintf(stdout, "%s\n[Yes/Y/No/N]: ", yes_no_msg);
        gets_s(input_buf, DQN_ARRAY_COUNT(input_buf) - 1);
        char *input   = DqnChar_SkipWhitespace(input_buf);
        int input_len = DqnStr_Len(input);

        if (input_len == 3)
        {
            char const yes[] = "Yes";
            if (DqnStr_Cmp(input, yes, DQN_CHAR_COUNT(yes), Dqn::IgnoreCase::Yes) == 0)
                return true;
            else
                continue;
        }

        if (input_len == 2)
        {
            char const no[] = "No";
            if (DqnStr_Cmp(input, no, DQN_CHAR_COUNT(no), Dqn::IgnoreCase::Yes) == 0)
                return false;
            else
                continue;
        }

        if (input_len == 1)
        {
            char input_lower = DqnChar_ToLower(input[0]);
            if (input_lower == 'y')
            {
                return true;
            }
            else if (input_lower == 'n')
            {
                return false;
            }
        }
    }
}

FileNode const *PromptSelectFile(FileNode const *root, DqnFixedString2048 *curr_path)
{
    auto mem_region = global_func_local_allocator_.MemRegionScoped();

    FileNode const *result = root;
    {
        int name_utf8_len = 0;
        char *name_utf8   = WCharToUTF8(&global_func_local_allocator_, result->name.str, &name_utf8_len);
        curr_path->SprintfAppend("/%.*s", name_utf8_len, name_utf8);
        global_func_local_allocator_.Pop(name_utf8);
        result = root->child;
    }

    bool exit_prompt               = false;
    FileNode const **index_to_node = nullptr;
    for (char input_buf[8]; !exit_prompt; global_func_local_allocator_.Pop(index_to_node), system("cls"))
    {
        input_buf[0]          = 0;
        isize const num_nodes = result->parent->num_children;
        index_to_node         = DQN_MEMSTACK_PUSH_ARRAY(&global_func_local_allocator_, FileNode const *, num_nodes);

        // Print interactive display and generate index_to_node array
        {
            fprintf(stdout, "\n");
            fprintf(stdout, "Path: %.*s\n", curr_path->len, curr_path->str);

            FileNode const *node = result;
            for (isize node_index = 0; node_index < num_nodes; node_index++, node = node->next)
            {
                index_to_node[node_index] = node;
                fwprintf(stdout, L"  |--[%zu] %ws\n", node_index, node->name.str);
            }
            DQN_ASSERTM(node == nullptr, "Tree node did not calculate the number of children correctly, reported: %d", num_nodes);

            fprintf(stdout, "\na: Accept Current Directory | p: Previous Directory | [0-n]: Enter Directory\n");
            fprintf(stdout, "\nSelect the next directory to enter: ");
        }

        gets_s(input_buf, DQN_ARRAY_COUNT(input_buf) - 1);
        char *input     = DqnChar_SkipWhitespace(input_buf);
        isize input_len = DqnStr_Len(input);

        if (input_len <= 0)
            continue;

        DqnFixedString2048 exit_msg = {};
        char const *exit_msg_fmt    = "Would you like to accept the selected path: %.*s";

        // Parse User Input
        if (input_len == 1)
        {
            char input_lower = DqnChar_ToLower(input[0]);
            if (input_lower == 'p' && result->parent != root)
            {
                char *fwd_slash = nullptr;
                for (isize i = curr_path->len - 1; i >= 0; i--)
                {
                    if (curr_path->str[i] == '/')
                    {
                        fwd_slash = curr_path->str + i;
                        break;
                    }
                }

                DQN_ASSERT(fwd_slash);
                curr_path->len = static_cast<int>(fwd_slash - curr_path->str);
                curr_path->NullTerminate();

                result = result->parent;
                continue;
            }
            else if (input_lower == 'a')
            {
                exit_msg.SprintfAppend(exit_msg_fmt, curr_path->len, curr_path->str);
                exit_prompt = PromptYesOrNoStdin(exit_msg.str);
            }
        }

        if (num_nodes > 0 && DqnChar_IsDigit(input[0]))
        {
            int choice = (int)Dqn_StrToI64(input, input_len);
            if (choice >= 0 && choice < (int)num_nodes)
            {
                result         = index_to_node[choice];
                int node_name_len = 0;
                char *node_name   = WCharToUTF8(&global_func_local_allocator_, result->name.str, &node_name_len);
                curr_path->SprintfAppend("/%.*s", node_name_len, node_name);
                global_func_local_allocator_.Pop(node_name);

                if (result->child)
                {
                    result = result->child;
                }
                else
                {
                    exit_msg.SprintfAppend(exit_msg_fmt, curr_path->len, curr_path->str);
                    exit_prompt = PromptYesOrNoStdin(exit_msg.str);
                }
            }
        }
    }
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

struct SoundFileToDisk
{
    DqnBuffer<wchar_t> dest_file_path;
};

FILE_SCOPE DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> ReadPlaylistFile(Context *context, wchar_t const *file)
{
    DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> result = {};

    auto mem_region = global_func_local_allocator_.MemRegionScoped();
    usize buf_size = 0;
    u8 *buf        = DqnFile_ReadAll(file, &buf_size, &global_func_local_allocator_);
    if (!buf)
    {
        DQN_LOGGER_W(&context->logger, "DqnFile_ReadAll: Failed, could not read file: %s\n", WCharToUTF8(&global_func_local_allocator_, file));
        return result;
    }

    auto *buf_ptr = reinterpret_cast<char *>(buf);

    // Skip UTF8-BOM bytes if present
    if (buf_size > 3 && (u8)buf_ptr[0] == 0xEF && (u8)buf_ptr[1] == 0xBB && (u8)buf_ptr[2] == 0xBF)
        buf_ptr += 3;

    DqnVArray<char *> missing_files = {};
    missing_files.LazyInit(DQN_MEGABYTE(1)/sizeof(missing_files.data[0]));
    DQN_DEFER { missing_files.Free(); };

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
            missing_files.Push(line);
        }
    }

    for (DqnVHashTable<DqnBuffer<wchar_t>, SoundFile>::Entry const &entry : result)
        fwprintf(stdout, L"parsed line: %s\n", entry.key.str);

    for (char const *missing_file : missing_files)
        fprintf(stdout, "could not access file in fs: %s\n", missing_file);


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
    void *buf = context->allocator.Push(playlist->num_used_buckets * sizeof(SoundFile));
    auto result = DqnArray<SoundFile>(static_cast<SoundFile *>(buf), playlist->num_used_buckets);

    for (DqnVHashTable<DqnBuffer<wchar_t>, SoundFile>::Entry const &entry : *playlist)
    {
        CheckAllocatorHasZeroAllocations(&global_func_local_allocator_);
        AVFormatContext *fmt_context = nullptr;
        auto mem_region               = global_func_local_allocator_.MemRegionScoped();

        DqnBuffer<wchar_t> const sound_path = entry.key;
        DqnBuffer<char> sound_path_utf8     = {};
        sound_path_utf8.str                 = WCharToUTF8(&global_func_local_allocator_, sound_path.str);
        if (avformat_open_input(&fmt_context, sound_path_utf8.str, nullptr, nullptr))
        {
            DQN_LOGGER_E(&context->logger, "avformat_open_input: failed to open file: %s", sound_path_utf8.str);
            continue;
        }
        DQN_DEFER { avformat_close_input(&fmt_context); };

        // TODO(doyle): Verify what this does
        if (avformat_find_stream_info(fmt_context, nullptr) < 0)
        {
            DQN_LOGGER_E(&context->logger, "avformat_find_stream_info: failed to find stream info");
            continue;
        }

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
            DQN_LOGGER_E(&context->logger, "Could not figure out the file extension for file path: %s", sound_path_utf8.str);
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
            DQN_LOGGER_E(&context->logger, "Could not figure out the file name for file path: %s", sound_path_utf8.str);
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
            DQN_LOGGER_W(&context->logger, "No metadata could be parsed for file: %s", entry.key.str);
            continue;
        }

        result.Push(sound_file);
#else
        av_dump_format(fmt_context, 0, sound_file_path.str, 0);
#endif
    }

    return result;
}

int main(int, char)
{
    Context context              = {};
    context.allocator            = DqnMemStack(DQN_MEGABYTE(1), Dqn::ZeroMem::Yes, DqnMemStack::Flag::DefaultFlags);
    global_func_local_allocator_ = DqnMemStack(DQN_MEGABYTE(1), Dqn::ZeroMem::Yes, DqnMemStack::Flag::DefaultFlags);
    DqnWin32_GetExeNameAndDirectory(&context.allocator, &context.exe_name, &context.exe_directory);

    HRESULT hresult = 0;
    if (FAILED(hresult = CoInitializeEx(0, COINIT_SPEED_OVER_MEMORY)))
    {
        HANDLE_COM_ERROR(hresult, "CoInitializeEx", &context.logger);
        return -1;
    }

    State state         = {};
    state.chosen_device = PromptAndSelectPortableDeviceID(&context);
    if (state.chosen_device.num_devices == 0)
    {
        fprintf(stdout, "There are no MTP devices to choose from.");
        return 0;
    }

    isize file_tree_allocator_mem_size = DQN_MEGABYTE(16);
    void *file_tree_allocator_mem      = DqnOS_VAlloc(file_tree_allocator_mem_size);
    DQN_DEFER { DqnOS_VFree(file_tree_allocator_mem, file_tree_allocator_mem_size); };

    Context file_tree_context   = {};
    file_tree_context.allocator = DqnMemStack(file_tree_allocator_mem, file_tree_allocator_mem_size, Dqn::ZeroMem::No, DqnMemStack::Flag::DefaultFlags);
    file_tree_context.logger    = context.logger;
    FileNode root               = {};
    {
        state.opened_device              = OpenPortableDevice(&context, state.chosen_device.device_id.str);
        IPortableDevice *portable_device = state.opened_device.Get();
        WPDReadSettings read_settings    = {};
        if (!WPDMakeReadSettings(&context, portable_device, &read_settings))
        {
            DQN_LOGGER_E(&context.logger, "Could not make WPDReadSettings");
            return -1;
        }

        isize enum_count = 0;
        f64 start = DqnTimer_NowInMs();
        // TODO(doyle): #performance make an iterative solution. but not important now
        WPDMakeFileTreeRecursively(&file_tree_context, &read_settings, WPD_DEVICE_OBJECT_ID, &root, &enum_count);
        f64 end = DqnTimer_NowInMs();

        fprintf(stdout, "Device recursively visited: %zu items and took: %5.2fs\n", enum_count, (f32)(end - start)/1000.0f);
    }

    DqnFixedString2048 abs_file_path = {};
    FileNode const *chosen_path      = PromptSelectFile(&root, &abs_file_path);

    DqnVHashTable<DqnBuffer<wchar_t>, SoundFile> playlist = ReadPlaylistFile(&context, L"Data/test.m3u8");
    DqnArray<SoundFile> sounds = MakeSoundFiles(&context, &playlist);

    for (SoundFile const &sound_file : sounds)
    {
        CheckAllocatorHasZeroAllocations(&global_func_local_allocator_);
        wchar_t const *artist = (sound_file.metadata.artist) ? sound_file.metadata.artist.str : L"_";
        wchar_t const *album  = (sound_file.metadata.album)  ? sound_file.metadata.album.str : L"_";
        wchar_t const *title  = (sound_file.metadata.title)  ? sound_file.metadata.title.str : sound_file.name.str;

        DqnBuffer<wchar_t> dest_folder_path = AllocateSwprintf(&context.allocator, L"%s\\Files\\%s\\%s\\%s.%s", context.exe_directory.str, artist, album, title, sound_file.extension.str);

        auto mem_region = context.allocator.MemRegionScoped();
        DQN_LOGGER_D(&context.logger, "%s", WCharToUTF8(&context.allocator, dest_folder_path.str));
    }

    DqnFile m3u_file      = {};
    if (!m3u_file.Open("test.m3u8", DqnFile::Flag::FileReadWrite, DqnFile::Action::ForceCreate))
    {
        DQN_LOGGER_E(&context.logger, "DqnFile::open failed: Could not create file: test.m3u8");
        return -1;
    }

    DQN_DEFER { m3u_file.Close(); };

    return 0;
}
