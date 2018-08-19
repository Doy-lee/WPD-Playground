#define WIN32_MEAN_AND_LEAN
#include <Windows.h>
#include <combaseapi.h>
#include <PortableDevice.h>
#include <PortableDeviceTypes.h>
#include <PortableDeviceApi.h>
#include <comdef.h>
#include <wrl/client.h> // ComPtr
#include <stdio.h>

#define DQN_PLATFORM_HEADER
#include "External/Dqn.h"

using Microsoft::WRL::ComPtr;

struct Context
{
    DqnLogger   logger;
    DqnMemStack allocator;
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

FILE_SCOPE DqnMemStack global_tmp_allocator;

char *WCharToUTF8(DqnMemStack *allocator, WCHAR const *wstr, int *result_len)
{
    int required_len   = DqnWin32_WCharToUTF8(wstr, nullptr, 0);
    char *result       = DQN_MEMSTACK_PUSH_ARRAY(allocator, char, required_len);
    int convert_result = DqnWin32_WCharToUTF8(wstr, result, required_len);
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

DqnBuffer<wchar_t> CopyWStringToSlice(DqnMemStack *allocator, wchar_t const *str_to_copy, int len = -1)
{
    if (len == -1) len = DqnWStr_Len(str_to_copy);

    DqnBuffer<wchar_t> result = {};
    result.len                = len;
    result.str                = DQN_MEMSTACK_PUSH_ARRAY(allocator, wchar_t, len + 1);
    DqnMem_Copy(result.str, str_to_copy, sizeof(*result.str) * len);
    result.data[len] = 0;
    return result;
}

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
SelectedDevice PromptAndSelectPortableDeviceID(Context *ctx)
{
    auto memGuard = global_tmp_allocator.TempRegionGuard();

    SelectedDevice result                         = {};
    HRESULT hresult                               = 0;
    ComPtr<IPortableDeviceManager> device_manager = nullptr;

    if (FAILED(hresult = CoCreateInstance(__uuidof(PortableDeviceManager), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device_manager))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &ctx->logger);
        return result;
    }

    DWORD num_devices = 0;
    if (FAILED(hresult = device_manager->GetDevices(nullptr, &num_devices)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDevices", &ctx->logger);
        return result;
    }

    fprintf(stdout, "Enumerated %u Windows Portable Device(s)\n", num_devices);
    result.num_devices = (int)num_devices;
    if (num_devices == 0)
        return result;

    WCHAR **device_names          = DQN_MEMSTACK_PUSH_ARRAY(&global_tmp_allocator, WCHAR *, num_devices);
    WCHAR **friendly_device_names = DQN_MEMSTACK_PUSH_ARRAY(&global_tmp_allocator, WCHAR *, num_devices);
    if (FAILED(hresult = device_manager->GetDevices(device_names, &num_devices)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDevices", &ctx->logger);
        return result;
    }

    for (isize device_index = 0; device_index < (isize)num_devices; ++device_index)
    {
        WCHAR *name = device_names[device_index];
        DWORD friendly_name_len = 0;
        if (FAILED(device_manager->GetDeviceFriendlyName(name, nullptr, &friendly_name_len)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDeviceFriendlyName", &ctx->logger);
            return result;
        }

        WCHAR *friendly_name = DQN_MEMSTACK_PUSH_ARRAY(&global_tmp_allocator, WCHAR, friendly_name_len);
        if (FAILED(device_manager->GetDeviceFriendlyName(name, friendly_name, &friendly_name_len)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceManager::GetDeviceFriendlyName", &ctx->logger);
            return result;
        }

        char *friendly_name_utf8 = WCharToUTF8(&global_tmp_allocator, friendly_name, nullptr);

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
    result.device_id                         = CopyWStringToSlice(&ctx->allocator, chosen_device);
    result.device_friendly_name              = CopyWStringToSlice(&ctx->allocator, chosen_device_friendly_name);

    // Free the memory Windows allocated for us
    for (isize device_index = 0; device_index < num_devices; ++device_index)
        CoTaskMemFree(device_names[device_index]);

    return result;
}

ComPtr<IPortableDevice> OpenPortableDevice(Context *ctx, wchar_t const *device_id)
{
    ComPtr<IPortableDeviceValues> device_values = nullptr;
    HRESULT hresult                             = 0;

    if (FAILED(hresult = CoCreateInstance(CLSID_PortableDeviceValues, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&device_values))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &ctx->logger);
        return nullptr;
    }

    ComPtr<IPortableDevice> portable_device = nullptr;
    if (FAILED(hresult = CoCreateInstance(CLSID_PortableDeviceFTM, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&portable_device))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &ctx->logger);
        return nullptr;
    }

    if (FAILED(hresult = portable_device->Open(device_id, device_values.Get())))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::Open", &ctx->logger);
        return nullptr;
    }

    ComPtr<IPortableDevice> result = nullptr;
    if (FAILED(hresult = portable_device->QueryInterface(IID_IPortableDevice, (void **)&result)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::QueryInterface", &ctx->logger);
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

FILE_SCOPE bool WPDMakeReadSettings(Context *ctx, IPortableDevice *portable_device, WPDReadSettings *settings)
{
    HRESULT hresult;
    if (FAILED(hresult = portable_device->Content(&settings->content)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDevice::Content", &ctx->logger);
        return false;
    }

    if (FAILED(CoCreateInstance(CLSID_PortableDeviceKeyCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&settings->properties_to_read))))
    {
        HANDLE_COM_ERROR(hresult, "CoCreateInstance", &ctx->logger);
        return false;
    }

    if (FAILED(settings->properties_to_read->Add(WPD_OBJECT_NAME)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceKeyCollection::Add", &ctx->logger);
        return false;
    }

    if (FAILED(settings->content->Properties(&settings->device_properties)))
    {
        HANDLE_COM_ERROR(hresult, "IPortableDeviceContext::Properties", &ctx->logger);
        return false;
    }

    return true;
}

void WPDMakeFileTreeRecursively(Context *ctx, WPDReadSettings *read_settings, WCHAR const *parent_object_id, FileNode *file_node, isize *enum_count, int depth = 0)
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
                DQN_DEFER(CoTaskMemFree(object_name));
#if 0
                for (isize depth_index = 1; depth_index < (depth - 1); depth_index++)
                    wprintf(L"    ");

                if (depth >= 1)
                    wprintf(L"|--");

                wprintf(L"%s\n", object_name);
#endif

                file_node->name = CopyWStringToSlice(&ctx->allocator, object_name);
            }
            else
            {
                HANDLE_COM_ERROR(hresult, "IPortableDeviceValues::GetStringValue", &ctx->logger);
            }
        }
        else
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceKeyCollection::GetValues", &ctx->logger);
        }
    }

    // Recursively iterate contents
    {
        HRESULT hresult = 0;
        IPortableDeviceContent *content                 = read_settings->content.Get();
        ComPtr<IEnumPortableDeviceObjectIDs> object_ids = nullptr;
        if (FAILED(hresult = content->EnumObjects(0 /*dwFlags ignored*/, parent_object_id, nullptr /*pFilter ignored*/, &object_ids)))
        {
            HANDLE_COM_ERROR(hresult, "IPortableDeviceContent::EnumObjects", &ctx->logger);
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
                        file_node->child = DQN_MEMSTACK_PUSH_STRUCT(&ctx->allocator, FileNode);
                        child            = file_node->child;
                        child->parent    = file_node;
                    }
                    else
                    {
                        child->next   = DQN_MEMSTACK_PUSH_STRUCT(&ctx->allocator, FileNode);
                        child         = child->next;
                        child->parent = file_node;
                    }

                    WPDMakeFileTreeRecursively(ctx, read_settings, object_id_array[fetch_index], child, enum_count, depth + 1);
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
    FileNode const *result = root;
    {
        int name_utf8_len = 0;
        char *name_utf8   = WCharToUTF8(&global_tmp_allocator, result->name.str, &name_utf8_len);
        curr_path->SprintfAppend("/%.*s", name_utf8_len, name_utf8);
        global_tmp_allocator.Pop(name_utf8);
        result = root->child;
    }

    bool exit_prompt               = false;
    FileNode const **index_to_node = nullptr;
    for (char input_buf[8]; !exit_prompt; global_tmp_allocator.Pop(index_to_node), system("cls"))
    {
        input_buf[0]          = 0;
        isize const num_nodes = result->parent->num_children;
        index_to_node         = DQN_MEMSTACK_PUSH_ARRAY(&global_tmp_allocator, FileNode const *, num_nodes);

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
                char *node_name   = WCharToUTF8(&global_tmp_allocator, result->name.str, &node_name_len);
                curr_path->SprintfAppend("/%.*s", node_name_len, node_name);
                global_tmp_allocator.Pop(node_name);

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

int main(int, char)
{
    global_tmp_allocator = DqnMemStack(DQN_MEGABYTE(1), Dqn::ZeroClear::Yes, DqnMemStack::Flag::All);

    Context ctx   = {};
    ctx.allocator = DqnMemStack(DQN_MEGABYTE(1), Dqn::ZeroClear::Yes, DqnMemStack::Flag::All);

    HRESULT hresult = 0;
    if (FAILED(hresult = CoInitializeEx(0, COINIT_SPEED_OVER_MEMORY)))
    {
        HANDLE_COM_ERROR(hresult, "CoInitializeEx", &ctx.logger);
        return -1;
    }

    State state         = {};
    state.chosen_device = PromptAndSelectPortableDeviceID(&ctx);
    if (state.chosen_device.num_devices == 0)
    {
        fprintf(stdout, "There are no MTP devices to choose from.");
        return 0;
    }

    isize file_tree_allocator_mem_size = DQN_MEGABYTE(16);
    void *file_tree_allocator_mem      = DqnOS_VAlloc(file_tree_allocator_mem_size);

    Context file_tree_ctx   = {};
    file_tree_ctx.allocator = DqnMemStack(file_tree_allocator_mem, file_tree_allocator_mem_size, Dqn::ZeroClear::No, DqnMemStack::Flag::All);
    file_tree_ctx.logger    = ctx.logger;
    FileNode root           = {};
    {
        state.opened_device              = OpenPortableDevice(&ctx, state.chosen_device.device_id.str);
        IPortableDevice *portable_device = state.opened_device.Get();
        WPDReadSettings read_settings    = {};
        if (!WPDMakeReadSettings(&ctx, portable_device, &read_settings))
        {
            DQN_LOGGER_E(&ctx.logger, "Could not make WPDReadSettings");
            return -1;
        }

        isize enum_count = 0;
        f64 start = DqnTimer_NowInMs();
        WPDMakeFileTreeRecursively(&file_tree_ctx, &read_settings, WPD_DEVICE_OBJECT_ID, &root, &enum_count);
        f64 end = DqnTimer_NowInMs();

        fprintf(stdout, "Device recursively visited: %zu items and took: %5.2fs\n", enum_count, (f32)(end - start)/1000.0f);
    }

    DqnFixedString2048 abs_file_path = {};
    FileNode const *chosen_file      = PromptSelectFile(&root, &abs_file_path);
    (void)chosen_file;

    return 0;
}
