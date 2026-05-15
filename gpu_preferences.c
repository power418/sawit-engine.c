#include "gpu_preferences.h"

#include "diagnostics.h"

#if defined(_WIN32)

#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi1_6.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t k_gpu_preferences_registry_path[] = L"Software\\Microsoft\\DirectX\\UserGpuPreferences";

static void gpu_preferences_copy_utf8(char* destination, size_t destination_size, const char* source);
static void gpu_preferences_copy_wide_to_utf8(char* destination, size_t destination_size, const wchar_t* source);
static GpuPreferenceMode gpu_preferences_read_selected_mode(void);
static int gpu_preferences_write_selected_mode(GpuPreferenceMode mode);
static int gpu_preferences_get_executable_path_wide(wchar_t* buffer, size_t buffer_count);
static int gpu_preferences_find_adapter_index_by_luid(const LUID* luids, int adapter_count, LUID target_luid);
static void gpu_preferences_assign_task_manager_indices(GpuPreferenceInfo* info);

static void gpu_preferences_copy_utf8(char* destination, size_t destination_size, const char* source)
{
  size_t length = 0;

  if (destination == NULL || destination_size == 0U)
  {
    return;
  }

  destination[0] = '\0';
  if (source == NULL)
  {
    return;
  }

  length = strlen(source);
  if (length >= destination_size)
  {
    length = destination_size - 1U;
  }

  memcpy(destination, source, length);
  destination[length] = '\0';
}

static void gpu_preferences_copy_wide_to_utf8(char* destination, size_t destination_size, const wchar_t* source)
{
  int converted_length = 0;

  if (destination == NULL || destination_size == 0U)
  {
    return;
  }

  destination[0] = '\0';
  if (source == NULL || source[0] == L'\0')
  {
    return;
  }

  converted_length = WideCharToMultiByte(CP_UTF8, 0, source, -1, destination, (int)destination_size, NULL, NULL);
  if (converted_length <= 0)
  {
    (void)snprintf(destination, destination_size, "GPU");
  }
}

static int gpu_preferences_get_executable_path_wide(wchar_t* buffer, size_t buffer_count)
{
  DWORD length = 0;

  if (buffer == NULL || buffer_count == 0U)
  {
    return 0;
  }

  length = GetModuleFileNameW(NULL, buffer, (DWORD)buffer_count);
  if (length == 0U || length >= (DWORD)buffer_count)
  {
    buffer[0] = L'\0';
    return 0;
  }

  return 1;
}

static GpuPreferenceMode gpu_preferences_read_selected_mode(void)
{
  HKEY key = NULL;
  wchar_t executable_path[MAX_PATH] = { 0 };
  wchar_t value_buffer[64] = { 0 };
  DWORD value_size = sizeof(value_buffer);
  DWORD value_type = 0;
  long status = ERROR_FILE_NOT_FOUND;
  unsigned int value = 0U;

  if (!gpu_preferences_get_executable_path_wide(executable_path, sizeof(executable_path) / sizeof(executable_path[0])))
  {
    return GPU_PREFERENCE_MODE_AUTO;
  }

  status = RegOpenKeyExW(HKEY_CURRENT_USER, k_gpu_preferences_registry_path, 0U, KEY_READ, &key);
  if (status != ERROR_SUCCESS)
  {
    return GPU_PREFERENCE_MODE_AUTO;
  }

  status = RegQueryValueExW(key, executable_path, NULL, &value_type, (LPBYTE)value_buffer, &value_size);
  (void)RegCloseKey(key);
  if (status != ERROR_SUCCESS || value_type != REG_SZ)
  {
    return GPU_PREFERENCE_MODE_AUTO;
  }

  if (swscanf_s(value_buffer, L"GpuPreference=%u;", &value) != 1)
  {
    return GPU_PREFERENCE_MODE_AUTO;
  }

  if (value == 1U)
  {
    return GPU_PREFERENCE_MODE_MINIMUM_POWER;
  }
  if (value == 2U)
  {
    return GPU_PREFERENCE_MODE_HIGH_PERFORMANCE;
  }

  return GPU_PREFERENCE_MODE_AUTO;
}

static int gpu_preferences_write_selected_mode(GpuPreferenceMode mode)
{
  HKEY key = NULL;
  wchar_t executable_path[MAX_PATH] = { 0 };
  long status = ERROR_SUCCESS;

  if (!gpu_preferences_get_executable_path_wide(executable_path, sizeof(executable_path) / sizeof(executable_path[0])))
  {
    return 0;
  }

  status = RegCreateKeyExW(
    HKEY_CURRENT_USER,
    k_gpu_preferences_registry_path,
    0U,
    NULL,
    REG_OPTION_NON_VOLATILE,
    KEY_SET_VALUE,
    NULL,
    &key,
    NULL);
  if (status != ERROR_SUCCESS)
  {
    return 0;
  }

  if (mode == GPU_PREFERENCE_MODE_AUTO)
  {
    status = RegDeleteValueW(key, executable_path);
    (void)RegCloseKey(key);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
  }
  else
  {
    const unsigned int registry_value = (mode == GPU_PREFERENCE_MODE_HIGH_PERFORMANCE) ? 2U : 1U;
    wchar_t value_buffer[32] = { 0 };

    (void)swprintf(value_buffer, sizeof(value_buffer) / sizeof(value_buffer[0]), L"GpuPreference=%u;", registry_value);
    status = RegSetValueExW(
      key,
      executable_path,
      0U,
      REG_SZ,
      (const BYTE*)value_buffer,
      (DWORD)((wcslen(value_buffer) + 1U) * sizeof(wchar_t)));
    (void)RegCloseKey(key);
    return status == ERROR_SUCCESS;
  }
}

static int gpu_preferences_find_adapter_index_by_luid(const LUID* luids, int adapter_count, LUID target_luid)
{
  int index = 0;

  if (luids == NULL || adapter_count <= 0)
  {
    return -1;
  }

  for (index = 0; index < adapter_count; ++index)
  {
    if (luids[index].LowPart == target_luid.LowPart && luids[index].HighPart == target_luid.HighPart)
    {
      return index;
    }
  }

  return -1;
}

static void gpu_preferences_assign_task_manager_indices(GpuPreferenceInfo* info)
{
  int assigned_index = 0;
  int adapter_index = 0;
  const int use_preference_order =
    info != NULL &&
    info->minimum_power_index >= 0 &&
    info->minimum_power_index < info->adapter_count &&
    info->high_performance_index >= 0 &&
    info->high_performance_index < info->adapter_count &&
    info->minimum_power_index != info->high_performance_index;

  if (info == NULL)
  {
    return;
  }

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    info->adapters[adapter_index].task_manager_index = GPU_PREFERENCES_INVALID_TASK_MANAGER_INDEX;
  }

  if (use_preference_order != 0)
  {
    info->adapters[info->minimum_power_index].task_manager_index = assigned_index;
    assigned_index += 1;
    info->adapters[info->high_performance_index].task_manager_index = assigned_index;
    assigned_index += 1;
  }

  while (assigned_index < info->adapter_count)
  {
    int best_candidate = -1;
    unsigned int best_memory = 0U;

    for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
    {
      const GpuAdapterInfo* adapter = &info->adapters[adapter_index];

      if (adapter->task_manager_index != GPU_PREFERENCES_INVALID_TASK_MANAGER_INDEX)
      {
        continue;
      }

      if (best_candidate < 0 ||
        adapter->dedicated_video_memory_mb < best_memory ||
        (adapter->dedicated_video_memory_mb == best_memory && adapter_index < best_candidate))
      {
        best_candidate = adapter_index;
        best_memory = adapter->dedicated_video_memory_mb;
      }
    }

    if (best_candidate < 0)
    {
      break;
    }

    info->adapters[best_candidate].task_manager_index = assigned_index;
    assigned_index += 1;
  }
}

int gpu_preferences_query(GpuPreferenceInfo* out_info)
{
  IDXGIFactory6* factory = NULL;
  HRESULT result = S_OK;
  LUID stored_luids[GPU_PREFERENCES_MAX_ADAPTERS] = { 0 };
  int stored_adapter_count = 0;
  int total_adapter_count = 0;
  int adapter_index = 0;

  if (out_info == NULL)
  {
    return 0;
  }

  memset(out_info, 0, sizeof(*out_info));
  out_info->selected_mode = gpu_preferences_read_selected_mode();
  out_info->minimum_power_index = -1;
  out_info->high_performance_index = -1;
  gpu_preferences_copy_utf8(out_info->status_message, sizeof(out_info->status_message), "OS GPU routing will apply after relaunch");

  result = CreateDXGIFactory1(&IID_IDXGIFactory6, (void**)(&factory));
  if (FAILED(result) || factory == NULL)
  {
    gpu_preferences_copy_utf8(out_info->status_message, sizeof(out_info->status_message), "DXGI adapter enumeration unavailable");
    diagnostics_logf("gpu_preferences_query: CreateDXGIFactory1 failed hr=0x%08lx", (unsigned long)result);
    return 0;
  }

  for (adapter_index = 0;; ++adapter_index)
  {
    IDXGIAdapter1* adapter = NULL;
    DXGI_ADAPTER_DESC1 description;

    result = IDXGIFactory6_EnumAdapters1(factory, (UINT)adapter_index, &adapter);
    if (result == DXGI_ERROR_NOT_FOUND)
    {
      break;
    }
    if (FAILED(result) || adapter == NULL)
    {
      diagnostics_logf("gpu_preferences_query: EnumAdapters1 failed hr=0x%08lx index=%d", (unsigned long)result, adapter_index);
      break;
    }

    memset(&description, 0, sizeof(description));
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(adapter, &description)) &&
      (description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0)
    {
      if (stored_adapter_count < GPU_PREFERENCES_MAX_ADAPTERS)
      {
        GpuAdapterInfo* target_info = &out_info->adapters[stored_adapter_count];
        gpu_preferences_copy_wide_to_utf8(target_info->name, sizeof(target_info->name), description.Description);
        target_info->dedicated_video_memory_mb = (unsigned int)((description.DedicatedVideoMemory + (1024U * 1024U - 1U)) / (1024U * 1024U));
        target_info->luid_low_part = description.AdapterLuid.LowPart;
        target_info->luid_high_part = description.AdapterLuid.HighPart;
        target_info->task_manager_index = GPU_PREFERENCES_INVALID_TASK_MANAGER_INDEX;
        stored_luids[stored_adapter_count] = description.AdapterLuid;
        stored_adapter_count += 1;
      }
      total_adapter_count += 1;
    }

    IDXGIAdapter1_Release(adapter);
  }

  out_info->available = (stored_adapter_count > 0);
  out_info->adapter_count = stored_adapter_count;
  out_info->total_adapter_count = total_adapter_count;

  for (adapter_index = 0; adapter_index < 2; ++adapter_index)
  {
    const DXGI_GPU_PREFERENCE preference =
      (adapter_index == 0) ? DXGI_GPU_PREFERENCE_MINIMUM_POWER : DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE;
    IDXGIAdapter1* preferred_adapter = NULL;
    DXGI_ADAPTER_DESC1 preferred_description;
    int matched_index = -1;

    result = IDXGIFactory6_EnumAdapterByGpuPreference(
      factory,
      0U,
      preference,
      &IID_IDXGIAdapter1,
      (void**)(&preferred_adapter));
    if (FAILED(result) || preferred_adapter == NULL)
    {
      continue;
    }

    memset(&preferred_description, 0, sizeof(preferred_description));
    if (SUCCEEDED(IDXGIAdapter1_GetDesc1(preferred_adapter, &preferred_description)))
    {
      matched_index = gpu_preferences_find_adapter_index_by_luid(
        stored_luids,
        stored_adapter_count,
        preferred_description.AdapterLuid);
      if (matched_index >= 0)
      {
        if (preference == DXGI_GPU_PREFERENCE_MINIMUM_POWER)
        {
          out_info->minimum_power_index = matched_index;
          out_info->adapters[matched_index].is_minimum_power_candidate = 1;
        }
        else
        {
          out_info->high_performance_index = matched_index;
          out_info->adapters[matched_index].is_high_performance_candidate = 1;
        }
      }
    }

    IDXGIAdapter1_Release(preferred_adapter);
  }

  gpu_preferences_assign_task_manager_indices(out_info);
  IDXGIFactory6_Release(factory);
  diagnostics_logf(
    "gpu_preferences_query: adapters=%d total=%d selected=%d power=%d high=%d",
    out_info->adapter_count,
    out_info->total_adapter_count,
    (int)out_info->selected_mode,
    out_info->minimum_power_index,
    out_info->high_performance_index);
  return out_info->available;
}

void gpu_preferences_set_current_renderer(GpuPreferenceInfo* info, const char* renderer_name, const char* vendor_name)
{
  if (info == NULL)
  {
    return;
  }

  gpu_preferences_copy_utf8(info->current_renderer, sizeof(info->current_renderer), (renderer_name != NULL) ? renderer_name : "");
  gpu_preferences_copy_utf8(info->current_vendor, sizeof(info->current_vendor), (vendor_name != NULL) ? vendor_name : "");
}

int gpu_preferences_apply_and_relaunch(GpuPreferenceMode mode)
{
  STARTUPINFOW startup_info = { 0 };
  PROCESS_INFORMATION process_info = { 0 };
  const wchar_t* command_line = GetCommandLineW();
  wchar_t* mutable_command_line = NULL;
  size_t command_length = 0U;
  int launch_result = 0;

  if (mode < GPU_PREFERENCE_MODE_AUTO || mode >= GPU_PREFERENCE_MODE_COUNT)
  {
    return 0;
  }

  if (!gpu_preferences_write_selected_mode(mode))
  {
    diagnostics_log("gpu_preferences_apply_and_relaunch: failed to write GPU preference registry");
    return 0;
  }

  if (command_line == NULL)
  {
    diagnostics_log("gpu_preferences_apply_and_relaunch: GetCommandLineW returned NULL");
    return 0;
  }

  command_length = wcslen(command_line);
  mutable_command_line = (wchar_t*)malloc((command_length + 1U) * sizeof(wchar_t));
  if (mutable_command_line == NULL)
  {
    diagnostics_log("gpu_preferences_apply_and_relaunch: command line allocation failed");
    return 0;
  }

  memcpy(mutable_command_line, command_line, (command_length + 1U) * sizeof(wchar_t));
  startup_info.cb = sizeof(startup_info);
  launch_result = CreateProcessW(
    NULL,
    mutable_command_line,
    NULL,
    NULL,
    FALSE,
    0U,
    NULL,
    NULL,
    &startup_info,
    &process_info);
  free(mutable_command_line);

  if (!launch_result)
  {
    diagnostics_logf("gpu_preferences_apply_and_relaunch: CreateProcessW failed error=%lu", (unsigned long)GetLastError());
    return 0;
  }

  (void)CloseHandle(process_info.hThread);
  (void)CloseHandle(process_info.hProcess);
  diagnostics_logf("gpu_preferences_apply_and_relaunch: relaunch requested mode=%d", (int)mode);
  return 1;
}

const char* gpu_preferences_get_mode_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Power saving";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High performance";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "Let Windows decide";
  }
}

const char* gpu_preferences_get_mode_short_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Eco";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "Auto";
  }
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_task_manager_index(const GpuPreferenceInfo* info, int task_manager_index)
{
  int adapter_index = 0;

  if (info == NULL || task_manager_index < 0)
  {
    return NULL;
  }

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    if (info->adapters[adapter_index].task_manager_index == task_manager_index)
    {
      return &info->adapters[adapter_index];
    }
  }

  return NULL;
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_luid(
  const GpuPreferenceInfo* info,
  unsigned int luid_low_part,
  int luid_high_part)
{
  int adapter_index = 0;

  if (info == NULL)
  {
    return NULL;
  }

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    const GpuAdapterInfo* adapter = &info->adapters[adapter_index];
    if (adapter->luid_low_part == luid_low_part && adapter->luid_high_part == luid_high_part)
    {
      return adapter;
    }
  }

  return NULL;
}

#elif defined(__linux__)

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static void gpu_preferences_copy_utf8(char* destination, size_t destination_size, const char* source)
{
  size_t length = 0U;

  if (destination == NULL || destination_size == 0U)
  {
    return;
  }

  destination[0] = '\0';
  if (source == NULL)
  {
    return;
  }

  length = strlen(source);
  if (length >= destination_size)
  {
    length = destination_size - 1U;
  }

  memcpy(destination, source, length);
  destination[length] = '\0';
}

static int gpu_preferences_contains_case_insensitive(const char* text, const char* needle)
{
  size_t needle_length = 0U;
  size_t text_length = 0U;
  size_t index = 0U;

  if (text == NULL || needle == NULL)
  {
    return 0;
  }

  needle_length = strlen(needle);
  text_length = strlen(text);
  if (needle_length == 0U)
  {
    return 1;
  }
  if (needle_length > text_length)
  {
    return 0;
  }

  for (index = 0U; index <= text_length - needle_length; ++index)
  {
    size_t match_index = 0U;
    for (match_index = 0U; match_index < needle_length; ++match_index)
    {
      const unsigned char a = (unsigned char)text[index + match_index];
      const unsigned char b = (unsigned char)needle[match_index];
      if (tolower(a) != tolower(b))
      {
        break;
      }
    }
    if (match_index == needle_length)
    {
      return 1;
    }
  }

  return 0;
}

static int gpu_preferences_read_text_file(const char* path, char* buffer, size_t buffer_size)
{
  FILE* file = NULL;
  size_t bytes_read = 0U;

  if (path == NULL || buffer == NULL || buffer_size == 0U)
  {
    return 0;
  }

  buffer[0] = '\0';
  file = fopen(path, "rb");
  if (file == NULL)
  {
    return 0;
  }

  bytes_read = fread(buffer, 1U, buffer_size - 1U, file);
  fclose(file);
  buffer[bytes_read] = '\0';
  while (bytes_read > 0U && (buffer[bytes_read - 1U] == '\n' || buffer[bytes_read - 1U] == '\r' || buffer[bytes_read - 1U] == ' '))
  {
    buffer[bytes_read - 1U] = '\0';
    bytes_read -= 1U;
  }

  return bytes_read > 0U;
}

static int gpu_preferences_read_sysfs_hex(const char* path, unsigned int* out_value)
{
  char buffer[64] = { 0 };
  char* end = NULL;
  unsigned long value = 0UL;

  if (out_value == NULL || !gpu_preferences_read_text_file(path, buffer, sizeof(buffer)))
  {
    return 0;
  }

  value = strtoul(buffer, &end, 0);
  if (end == buffer)
  {
    return 0;
  }

  *out_value = (unsigned int)value;
  return 1;
}

static unsigned int gpu_preferences_read_vram_mb(const char* device_directory)
{
  static const char* k_vram_files[] = {
    "mem_info_vram_total",
    "mem_info_vis_vram_total"
  };
  size_t index = 0U;

  if (device_directory == NULL)
  {
    return 0U;
  }

  for (index = 0U; index < sizeof(k_vram_files) / sizeof(k_vram_files[0]); ++index)
  {
    char path[PATH_MAX] = { 0 };
    char buffer[64] = { 0 };
    char* end = NULL;
    unsigned long long bytes = 0ULL;

    if (snprintf(path, sizeof(path), "%s/%s", device_directory, k_vram_files[index]) <= 0 ||
      !gpu_preferences_read_text_file(path, buffer, sizeof(buffer)))
    {
      continue;
    }

    bytes = strtoull(buffer, &end, 10);
    if (end != buffer && bytes > 0ULL)
    {
      return (unsigned int)((bytes + (1024ULL * 1024ULL - 1ULL)) / (1024ULL * 1024ULL));
    }
  }

  return 0U;
}

static const char* gpu_preferences_get_linux_vendor_name(unsigned int vendor_id)
{
  switch (vendor_id)
  {
    case 0x8086U:
      return "Intel";
    case 0x10deU:
      return "NVIDIA";
    case 0x1002U:
    case 0x1022U:
      return "AMD Radeon";
    default:
      return "GPU";
  }
}

static int gpu_preferences_is_linux_integrated_vendor(unsigned int vendor_id)
{
  return vendor_id == 0x8086U;
}

static int gpu_preferences_is_linux_discrete_vendor(unsigned int vendor_id)
{
  return vendor_id == 0x10deU || vendor_id == 0x1002U || vendor_id == 0x1022U;
}

static GpuPreferenceMode gpu_preferences_read_selected_mode(void)
{
  const char* dri_prime = getenv("DRI_PRIME");
  const char* nvidia_offload = getenv("__NV_PRIME_RENDER_OFFLOAD");
  const char* optimus = getenv("__VK_LAYER_NV_optimus");

  if ((dri_prime != NULL && strcmp(dri_prime, "0") == 0) ||
    (nvidia_offload != NULL && strcmp(nvidia_offload, "0") == 0) ||
    (optimus != NULL && gpu_preferences_contains_case_insensitive(optimus, "non_NVIDIA")))
  {
    return GPU_PREFERENCE_MODE_MINIMUM_POWER;
  }

  if ((nvidia_offload != NULL && strcmp(nvidia_offload, "1") == 0) ||
    (dri_prime != NULL && dri_prime[0] != '\0' && strcmp(dri_prime, "0") != 0) ||
    (optimus != NULL && gpu_preferences_contains_case_insensitive(optimus, "NVIDIA_only")))
  {
    return GPU_PREFERENCE_MODE_HIGH_PERFORMANCE;
  }

  return GPU_PREFERENCE_MODE_AUTO;
}

static void gpu_preferences_assign_task_manager_indices(GpuPreferenceInfo* info)
{
  int index = 0;

  if (info == NULL)
  {
    return;
  }

  for (index = 0; index < info->adapter_count; ++index)
  {
    info->adapters[index].task_manager_index = index;
  }
}

static void gpu_preferences_select_linux_candidates(GpuPreferenceInfo* info, const unsigned int* vendor_ids)
{
  int adapter_index = 0;
  int best_high_index = -1;
  unsigned int best_high_memory = 0U;
  int best_any_index = -1;
  unsigned int best_any_memory = 0U;

  if (info == NULL || vendor_ids == NULL)
  {
    return;
  }

  info->minimum_power_index = -1;
  info->high_performance_index = -1;

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    const unsigned int vendor_id = vendor_ids[adapter_index];
    const unsigned int memory_mb = info->adapters[adapter_index].dedicated_video_memory_mb;

    if (info->minimum_power_index < 0 && gpu_preferences_is_linux_integrated_vendor(vendor_id))
    {
      info->minimum_power_index = adapter_index;
    }

    if (gpu_preferences_is_linux_discrete_vendor(vendor_id) &&
      (best_high_index < 0 || memory_mb >= best_high_memory))
    {
      best_high_index = adapter_index;
      best_high_memory = memory_mb;
    }

    if (best_any_index < 0 || memory_mb >= best_any_memory)
    {
      best_any_index = adapter_index;
      best_any_memory = memory_mb;
    }
  }

  if (best_high_index >= 0)
  {
    info->high_performance_index = best_high_index;
  }
  else if (best_any_index >= 0)
  {
    info->high_performance_index = best_any_index;
  }

  if (info->minimum_power_index < 0 && info->adapter_count > 0)
  {
    info->minimum_power_index = (info->high_performance_index == 0 && info->adapter_count > 1) ? 1 : 0;
  }

  if (info->minimum_power_index >= 0 && info->minimum_power_index < info->adapter_count)
  {
    info->adapters[info->minimum_power_index].is_minimum_power_candidate = 1;
  }
  if (info->high_performance_index >= 0 && info->high_performance_index < info->adapter_count)
  {
    info->adapters[info->high_performance_index].is_high_performance_candidate = 1;
  }
}

int gpu_preferences_query(GpuPreferenceInfo* out_info)
{
  DIR* directory = NULL;
  struct dirent* entry = NULL;
  unsigned int vendor_ids[GPU_PREFERENCES_MAX_ADAPTERS] = { 0U };

  if (out_info == NULL)
  {
    return 0;
  }

  memset(out_info, 0, sizeof(*out_info));
  out_info->minimum_power_index = -1;
  out_info->high_performance_index = -1;
  out_info->selected_mode = gpu_preferences_read_selected_mode();

  directory = opendir("/sys/class/drm");
  if (directory != NULL)
  {
    while ((entry = readdir(directory)) != NULL)
    {
      const char* name = entry->d_name;
      char* end = NULL;
      long card_number = 0L;
      char device_directory[PATH_MAX] = { 0 };
      char vendor_path[PATH_MAX] = { 0 };
      char device_path[PATH_MAX] = { 0 };
      unsigned int vendor_id = 0U;
      unsigned int device_id = 0U;

      if (strncmp(name, "card", 4) != 0 || !isdigit((unsigned char)name[4]))
      {
        continue;
      }

      card_number = strtol(name + 4, &end, 10);
      (void)card_number;
      if (end == name + 4 || *end != '\0')
      {
        continue;
      }

      if (snprintf(device_directory, sizeof(device_directory), "/sys/class/drm/%s/device", name) <= 0 ||
        snprintf(vendor_path, sizeof(vendor_path), "%s/vendor", device_directory) <= 0 ||
        !gpu_preferences_read_sysfs_hex(vendor_path, &vendor_id))
      {
        continue;
      }

      {
        const size_t device_directory_length = strlen(device_directory);
        if (device_directory_length + sizeof("/device") <= sizeof(device_path))
        {
          memcpy(device_path, device_directory, device_directory_length);
          memcpy(device_path + device_directory_length, "/device", sizeof("/device"));
          (void)gpu_preferences_read_sysfs_hex(device_path, &device_id);
        }
      }
      out_info->total_adapter_count += 1;

      if (out_info->adapter_count < GPU_PREFERENCES_MAX_ADAPTERS)
      {
        GpuAdapterInfo* adapter = &out_info->adapters[out_info->adapter_count];
        const char* vendor_name = gpu_preferences_get_linux_vendor_name(vendor_id);

        if (device_id != 0U)
        {
          (void)snprintf(adapter->name, sizeof(adapter->name), "%s GPU 0x%04x", vendor_name, device_id);
        }
        else
        {
          gpu_preferences_copy_utf8(adapter->name, sizeof(adapter->name), vendor_name);
        }
        adapter->dedicated_video_memory_mb = gpu_preferences_read_vram_mb(device_directory);
        adapter->luid_low_part = (unsigned int)out_info->adapter_count;
        adapter->luid_high_part = (int)vendor_id;
        adapter->task_manager_index = GPU_PREFERENCES_INVALID_TASK_MANAGER_INDEX;
        vendor_ids[out_info->adapter_count] = vendor_id;
        out_info->adapter_count += 1;
      }
    }
    closedir(directory);
  }

  out_info->available = (out_info->adapter_count > 0);
  gpu_preferences_select_linux_candidates(out_info, vendor_ids);
  gpu_preferences_assign_task_manager_indices(out_info);

  if (out_info->available != 0)
  {
    if (out_info->adapter_count > 1)
    {
      gpu_preferences_copy_utf8(
        out_info->status_message,
        sizeof(out_info->status_message),
        "Linux PRIME GPU routing will apply after relaunch");
    }
    else
    {
      gpu_preferences_copy_utf8(
        out_info->status_message,
        sizeof(out_info->status_message),
        "Single GPU detected; PRIME routing env can still be relaunched");
    }
  }
  else
  {
    gpu_preferences_copy_utf8(
      out_info->status_message,
      sizeof(out_info->status_message),
      "Linux GPU routing uses PRIME env; no DRM adapters were enumerated");
  }

  diagnostics_logf(
    "gpu_preferences_query: linux adapters=%d total=%d selected=%d power=%d high=%d",
    out_info->adapter_count,
    out_info->total_adapter_count,
    (int)out_info->selected_mode,
    out_info->minimum_power_index,
    out_info->high_performance_index);
  return out_info->available;
}

void gpu_preferences_set_current_renderer(GpuPreferenceInfo* info, const char* renderer_name, const char* vendor_name)
{
  if (info == NULL)
  {
    return;
  }

  gpu_preferences_copy_utf8(info->current_renderer, sizeof(info->current_renderer), (renderer_name != NULL) ? renderer_name : "");
  gpu_preferences_copy_utf8(info->current_vendor, sizeof(info->current_vendor), (vendor_name != NULL) ? vendor_name : "");
}

int gpu_preferences_apply_and_relaunch(GpuPreferenceMode mode)
{
  char executable_path[PATH_MAX] = { 0 };
  char command_line_buffer[8192] = { 0 };
  char* argv_storage = NULL;
  char** argv = NULL;
  ssize_t executable_length = 0;
  size_t bytes_read = 0U;
  int argc = 0;
  FILE* cmdline = NULL;
  pid_t child = -1;
  GpuPreferenceInfo info;
  int high_is_nvidia = 0;

  if (mode < GPU_PREFERENCE_MODE_AUTO || mode >= GPU_PREFERENCE_MODE_COUNT)
  {
    return 0;
  }

  executable_length = readlink("/proc/self/exe", executable_path, sizeof(executable_path) - 1U);
  if (executable_length <= 0 || executable_length >= (ssize_t)sizeof(executable_path))
  {
    diagnostics_log("gpu_preferences_apply_and_relaunch: failed to resolve /proc/self/exe");
    return 0;
  }
  executable_path[executable_length] = '\0';

  memset(&info, 0, sizeof(info));
  (void)gpu_preferences_query(&info);
  if (info.high_performance_index >= 0 && info.high_performance_index < info.adapter_count)
  {
    high_is_nvidia = gpu_preferences_contains_case_insensitive(info.adapters[info.high_performance_index].name, "nvidia");
  }

  if (mode == GPU_PREFERENCE_MODE_HIGH_PERFORMANCE)
  {
    (void)setenv("DRI_PRIME", "1", 1);
    if (high_is_nvidia != 0)
    {
      (void)setenv("__NV_PRIME_RENDER_OFFLOAD", "1", 1);
      (void)setenv("__GLX_VENDOR_LIBRARY_NAME", "nvidia", 1);
      (void)setenv("__VK_LAYER_NV_optimus", "NVIDIA_only", 1);
    }
    else
    {
      (void)unsetenv("__NV_PRIME_RENDER_OFFLOAD");
      (void)unsetenv("__GLX_VENDOR_LIBRARY_NAME");
      (void)unsetenv("__VK_LAYER_NV_optimus");
    }
  }
  else if (mode == GPU_PREFERENCE_MODE_MINIMUM_POWER)
  {
    (void)setenv("DRI_PRIME", "0", 1);
    (void)setenv("__NV_PRIME_RENDER_OFFLOAD", "0", 1);
    (void)unsetenv("__GLX_VENDOR_LIBRARY_NAME");
    (void)setenv("__VK_LAYER_NV_optimus", "non_NVIDIA_only", 1);
  }
  else
  {
    (void)unsetenv("DRI_PRIME");
    (void)unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    (void)unsetenv("__GLX_VENDOR_LIBRARY_NAME");
    (void)unsetenv("__VK_LAYER_NV_optimus");
  }

  cmdline = fopen("/proc/self/cmdline", "rb");
  if (cmdline != NULL)
  {
    bytes_read = fread(command_line_buffer, 1U, sizeof(command_line_buffer) - 1U, cmdline);
    fclose(cmdline);
  }

  if (bytes_read > 0U)
  {
    size_t index = 0U;
    size_t arg_count = 0U;

    for (index = 0U; index < bytes_read; ++index)
    {
      if (command_line_buffer[index] == '\0')
      {
        arg_count += 1U;
      }
    }

    argv_storage = (char*)malloc(bytes_read + 1U);
    argv = (char**)calloc(arg_count + 2U, sizeof(char*));
    if (argv_storage != NULL && argv != NULL)
    {
      memcpy(argv_storage, command_line_buffer, bytes_read);
      argv_storage[bytes_read] = '\0';
      for (index = 0U; index < bytes_read && argc < (int)arg_count; )
      {
        argv[argc++] = argv_storage + index;
        while (index < bytes_read && argv_storage[index] != '\0')
        {
          index += 1U;
        }
        index += 1U;
      }
      argv[0] = executable_path;
      if (argc <= 0)
      {
        argc = 1;
      }
      argv[argc] = NULL;
    }
    else
    {
      free(argv);
      free(argv_storage);
      argv = NULL;
      argv_storage = NULL;
    }
  }

  if (argv == NULL)
  {
    argv = (char**)calloc(2U, sizeof(char*));
    if (argv == NULL)
    {
      free(argv_storage);
      return 0;
    }
    argv[0] = executable_path;
    argv[1] = NULL;
  }

  child = fork();
  if (child == 0)
  {
    execv(executable_path, argv);
    _exit(127);
  }

  free(argv);
  free(argv_storage);
  if (child < 0)
  {
    diagnostics_log("gpu_preferences_apply_and_relaunch: fork failed");
    return 0;
  }

  diagnostics_logf("gpu_preferences_apply_and_relaunch: linux relaunch requested mode=%d child=%ld", (int)mode, (long)child);
  return 1;
}

const char* gpu_preferences_get_mode_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Power saving";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High performance";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "System default";
  }
}

const char* gpu_preferences_get_mode_short_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Eco";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "Auto";
  }
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_task_manager_index(const GpuPreferenceInfo* info, int task_manager_index)
{
  int adapter_index = 0;

  if (info == NULL || task_manager_index < 0)
  {
    return NULL;
  }

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    if (info->adapters[adapter_index].task_manager_index == task_manager_index)
    {
      return &info->adapters[adapter_index];
    }
  }

  return NULL;
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_luid(
  const GpuPreferenceInfo* info,
  unsigned int luid_low_part,
  int luid_high_part)
{
  int adapter_index = 0;

  if (info == NULL)
  {
    return NULL;
  }

  for (adapter_index = 0; adapter_index < info->adapter_count; ++adapter_index)
  {
    const GpuAdapterInfo* adapter = &info->adapters[adapter_index];
    if (adapter->luid_low_part == luid_low_part && adapter->luid_high_part == luid_high_part)
    {
      return adapter;
    }
  }

  return NULL;
}

#else

#include <string.h>

static void gpu_preferences_copy_utf8(char* destination, size_t destination_size, const char* source)
{
  size_t length = 0U;

  if (destination == NULL || destination_size == 0U)
  {
    return;
  }

  destination[0] = '\0';
  if (source == NULL)
  {
    return;
  }

  length = strlen(source);
  if (length >= destination_size)
  {
    length = destination_size - 1U;
  }

  memcpy(destination, source, length);
  destination[length] = '\0';
}

int gpu_preferences_query(GpuPreferenceInfo* out_info)
{
  if (out_info == NULL)
  {
    return 0;
  }

  memset(out_info, 0, sizeof(*out_info));
  out_info->minimum_power_index = -1;
  out_info->high_performance_index = -1;
  out_info->selected_mode = GPU_PREFERENCE_MODE_AUTO;
  gpu_preferences_copy_utf8(
    out_info->status_message,
    sizeof(out_info->status_message),
    "GPU routing selection is not available on this platform");
  diagnostics_log("gpu_preferences_query: using non-Windows stub implementation");
  return 0;
}

void gpu_preferences_set_current_renderer(GpuPreferenceInfo* info, const char* renderer_name, const char* vendor_name)
{
  if (info == NULL)
  {
    return;
  }

  gpu_preferences_copy_utf8(info->current_renderer, sizeof(info->current_renderer), (renderer_name != NULL) ? renderer_name : "");
  gpu_preferences_copy_utf8(info->current_vendor, sizeof(info->current_vendor), (vendor_name != NULL) ? vendor_name : "");
}

int gpu_preferences_apply_and_relaunch(GpuPreferenceMode mode)
{
  (void)mode;
  diagnostics_log("gpu_preferences_apply_and_relaunch: non-Windows stub does not support relaunch routing");
  return 0;
}

const char* gpu_preferences_get_mode_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Power saving";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High performance";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "System default";
  }
}

const char* gpu_preferences_get_mode_short_label(GpuPreferenceMode mode)
{
  switch (mode)
  {
    case GPU_PREFERENCE_MODE_MINIMUM_POWER:
      return "Eco";
    case GPU_PREFERENCE_MODE_HIGH_PERFORMANCE:
      return "High";
    case GPU_PREFERENCE_MODE_AUTO:
    default:
      return "Auto";
  }
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_task_manager_index(const GpuPreferenceInfo* info, int task_manager_index)
{
  (void)info;
  (void)task_manager_index;
  return NULL;
}

const GpuAdapterInfo* gpu_preferences_find_adapter_by_luid(
  const GpuPreferenceInfo* info,
  unsigned int luid_low_part,
  int luid_high_part)
{
  (void)info;
  (void)luid_low_part;
  (void)luid_high_part;
  return NULL;
}

#endif
