## `mgsvtpp.exe`

- path: `<steam-library>/common/MGS_TPP/mgsvtpp.exe`
- version resource: FileVersion 1.0.15.4 / ProductVersion 1.0.15.4 / InternalName Tpp_main.exe / OriginalFilename Tpp_main.exe / Company Konami Digital Entertainment
- size: 166517760 bytes; sha256 `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`
- TimeDateStamp: 0x6a4cb898 (2026-07-07 08:28:08 UTC)
- ImageBase 0x140000000, SizeOfImage 0xa01d000, SizeOfHeaders 0x400
- EntryPoint RVA 0x69b5f80 in `.xdata`
- Subsystem 2, DllCharacteristics 0x8120 (HIGH_ENTROPY_VA, NX_COMPAT, TERMINAL_SERVER_AWARE), Linker 11.0, OS 5.2, CheckSum 0x0
- CodeView PDB: `mgsvtpp.pdb` (debug ts 2026-07-07 08:28:08 UTC)
- LoadConfig: Size=0x70, SecurityCookie=0x142b49330

| section | RVA | VSize | raw off | raw size | flags | entropy |
|---|---|---|---|---|---|---|
| `.xtext` | 0x1000 | 0x208c000 | 0x400 | 0x208c000 | R-X CODE (0x60000020) | 6.492 |
| `.edata` | 0x208d000 | 0x956000 | 0x208c400 | 0x956000 | R-- IDATA (0x40000040) | 5.165 |
| `.data2` | 0x29e3000 | 0x2c1000 | 0x29e2400 | 0x176000 | RW- IDATA (0xc0000040) | 5.929 |
| `.didata` | 0x2ca4000 | 0x19e000 | 0x2b58400 | 0x19e000 | R-- IDATA (0x40000040) | 0.001 |
| `.bss` | 0x2e42000 | 0x1000 | 0x2cf6400 | 0x200 | RW- IDATA (0xc0000040) | -0.0 |
| `.text` | 0x2e43000 | 0x9000 | 0x2cf6600 | 0x9000 | R-- IDATA (0x40000040) | 5.732 |
| `.impdata` | 0x2e4c000 | 0x28000 | 0x2cff600 | 0x28000 | R-- IDATA (0x40000040) | 7.846 |
| `.trace` | 0x2e74000 | 0x8f000 | 0x2d27600 | 0x8f000 | R-- IDATA|DISCARD (0x42000040) | 4.857 |
| `.xdata` | 0x2f03000 | 0x6f74df4 | 0x2db6600 | 0x6f74e00 | RWX CODE (0xe0000020) | 6.66 |
| `.00cfg` | 0x9e78000 | 0x40a8 | 0x9d2b400 | 0x4200 | R-- CODE (0x40000020) | 5.375 |
| `.xtls` | 0x9e7d000 | 0x308 | 0x9d2f600 | 0x400 | RW- CODE (0xc0000020) | 2.938 |
| `.debug$P` | 0x9e7e000 | 0x100 | 0x9d2fa00 | 0x200 | R-- IDATA (0x40000040) | 3.019 |
| `.ecode` | 0x9e7f000 | 0x19df04 | 0x9d2fc00 | 0x19e000 | R-- IDATA (0x40000040) | 7.028 |

Data directories: EXPORT@0x9e7e040+0x54(.debug$P), IMPORT@0x9e79340+0x2bc(.00cfg), RESOURCE@0x2e4c000+0x27a88(.impdata), EXCEPTION@0x9e7f000+0x19df04(.ecode), DEBUG@0x209ff40+0x38(.edata), TLS@0x255d060+0x28(.edata), LOAD_CONFIG@0x255cff0+0x70(.edata), IAT@0x208d000+0x1140(.edata)

TLS: {'StartAddressOfRawData': '0x149e78020', 'EndAddressOfRawData': '0x149e79340', 'AddressOfIndex_rva': '0x2c6ba70', 'AddressOfCallBacks_rva': '0x9e78000', 'callbacks': [{'rva': '0x6cfc4ce', 'section': '.xdata'}, {'rva': '0x5aab920', 'section': '.xdata'}, {'rva': '0xe200', 'section': '.xtext'}]}

Protector heuristics:
- Entry point is in section '.xdata' (not .text)
- Non-standard section name '.xtext' (R-X CODE, vsize 0x208c000, entropy 6.492)
- Non-standard section name '.data2' (RW- IDATA, vsize 0x2c1000, entropy 5.929)
- Non-standard section name '.didata' (R-- IDATA, vsize 0x19e000, entropy 0.001)
- Non-standard section name '.impdata' (R-- IDATA, vsize 0x28000, entropy 7.846)
- Non-standard section name '.trace' (R-- IDATA|DISCARD, vsize 0x8f000, entropy 4.857)
- Non-standard section name '.xtls' (RW- CODE, vsize 0x308, entropy 2.938)
- Non-standard section name '.debug$P' (R-- IDATA, vsize 0x100, entropy 3.019)
- Non-standard section name '.ecode' (R-- IDATA, vsize 0x19df04, entropy 7.028)
- 3 TLS callback(s): 0x6cfc4ce(.xdata), 0x5aab920(.xdata), 0xe200(.xtext)
- protector-name string 'DENUVO' x1: rva 0x6f3fbe1 (.xdata)

Entry point disassembly:
```
0x69B5F80: lea rsp, [rsp - 0x98]
0x69B5F88: mov qword ptr [rsp + 0x20], rax
0x69B5F90: mov qword ptr [rsp + 0x28], rcx
0x69B5F98: mov qword ptr [rsp + 0x30], rdx
0x69B5FA0: mov qword ptr [rsp + 0x38], rbx
0x69B5FA8: mov qword ptr [rsp + 0x40], rbp
0x69B5FB0: mov qword ptr [rsp + 0x48], rsi
0x69B5FB8: mov qword ptr [rsp + 0x50], rdi
0x69B5FC0: mov qword ptr [rsp + 0x58], r8
0x69B5FC8: mov qword ptr [rsp + 0x60], r9
0x69B5FD0: mov qword ptr [rsp + 0x68], r10
0x69B5FD8: mov qword ptr [rsp + 0x70], r11
```
TLS callback 0x6cfc4ce:
```
0x6CFC4CE: jmp 0x84566c0
0x6CFC4D3: nop 
0x6CFC4D4: lea rsi, [rsp - 0x3c5df0b1]
0x6CFC4DC: push r15
0x6CFC4DE: not dword ptr [rsp]
0x6CFC4E1: lea r11, [rsp]
0x6CFC4E5: and dword ptr [rsp], r15d
0x6CFC4E9: shl dword ptr [rsp], 1
0x6CFC4EC: xor r15d, r15d
0x6CFC4EF: add r11, 0x769f383e
0x6CFC4F6: mov rdi, 0x5f518a9f
0x6CFC4FD: sub r15d, dword ptr [r11 - 0x769f383e]
```
TLS callback 0x5aab920:
```
0x5AAB920: lea rsp, [rsp - 0x28]
0x5AAB928: push r12
0x5AAB92A: sub rsp, 8
0x5AAB92E: mov qword ptr [rsp], r11
0x5AAB932: add rsp, -8
0x5AAB936: mov qword ptr [rsp], r11
0x5AAB93A: not r11
0x5AAB93D: and qword ptr [rsp], r11
0x5AAB941: pop r11
0x5AAB943: push rax
0x5AAB944: lea rax, [rip - 0x2b9d938]
0x5AAB94B: add rax, -0x5df0b730
```
TLS callback 0xe200:
```
0xE200: sub rsp, 0x28
0xE204: cmp edx, 3
0xE207: jne 0xe20e
0xE209: call 0xe220
0xE20E: mov eax, 1
0xE213: add rsp, 0x28
0xE217: ret 
0xE218: int3 
0xE219: int3 
0xE21A: int3 
0xE21B: int3 
0xE21C: int3 
```

### Watch-list imports
- **steam_api64.dll**: SteamAPI_UnregisterCallResult, SteamAPI_RegisterCallResult, SteamAPI_RunCallbacks, SteamAPI_UnregisterCallback, SteamAPI_RegisterCallback, SteamUserStats, SteamUtils, SteamFriends, SteamRemoteStorage, SteamApps, SteamUser, SteamAPI_Init, SteamNetworking
- **kernel32.dll**: IsDebuggerPresent, LoadLibraryW, GetProcAddress, SleepEx, GetTickCount, MapViewOfFile, CreateFileMappingW, CreateEventW, GetModuleHandleW, SetEvent, LoadLibraryA, QueryPerformanceFrequency, QueryPerformanceCounter, WaitForSingleObject, CreateThread, Sleep, VirtualAlloc, GetModuleHandleA, LoadLibraryExW
- **user32.dll**: GetForegroundWindow, SetFocus, GetClientRect, SetWindowPos, ShowWindow, CreateWindowExW, RegisterClassExW, DefWindowProcW, DispatchMessageW, TranslateMessage, GetMessageW, RegisterRawInputDevices, MapVirtualKeyW, ToUnicode, GetKeyboardState, CallWindowProcW, SetWindowLongPtrW, GetRawInputData, ScreenToClient, GetCursorPos, SetCursorPos, ShowCursor, SetForegroundWindow, ClientToScreen, CreateWindowExA, ReleaseCapture, SetCapture, SetCursor
- **dinput8.dll**: DirectInput8Create
- **xinput1_3.dll**: #ord2
- **winmm.dll**: timeEndPeriod, timeGetTime, timeBeginPeriod
- **dxgi.dll**: CreateDXGIFactory1
- **d3d11.dll**: D3D11CreateDevice

### Proxy DLL viability evidence
| DLL | evidence | KnownDLL |
|---|---|---|
| dinput8.dll | static import; name string in image (ascii) | no |
| dxgi.dll | static import; name string in image (ascii) | no |
| d3d11.dll | static import; name string in image (ascii) | no |
| winmm.dll | static import; name string in image (ascii) | no |
| winhttp.dll | static import; name string in image (ascii) | no |
| xinput1_3.dll | static import; name string in image (ascii,utf16) | no |
| xinput1_4.dll | name string in image (utf16) | no |
| wininet.dll | static import; name string in image (ascii) | no |
| hid.dll | static import; name string in image (ascii) | no |
| steam_api64.dll | static import; name string in image (ascii,utf16) | no |
| iphlpapi.dll | static import; name string in image (ascii) | no |
| ws2_32.dll | static import; name string in image (ascii) | yes |
| setupapi.dll | static import; name string in image (ascii) | yes |
| xaudio2_7.dll | name string in image (ascii) | no |

### Full import table
- **IMM32.dll** (1): ImmDisableIME
- **steam_api64.dll** (13): SteamAPI_UnregisterCallResult, SteamAPI_RegisterCallResult, SteamAPI_RunCallbacks, SteamAPI_UnregisterCallback, SteamAPI_RegisterCallback, SteamUserStats, SteamUtils, SteamFriends, SteamRemoteStorage, SteamApps, SteamUser, SteamAPI_Init, SteamNetworking
- **KERNEL32.dll** (92): GetLastError, DuplicateHandle, SetFilePointerEx, FindNextFileW, FindFirstFileW, GetFileAttributesW, SetThreadExecutionState, GetFileTime, FindClose, GetFileSizeEx, FileTimeToLocalFileTime, GetLocalTime, GetSystemTime, RaiseException, ReleaseMutex, CloseHandle, CreateMutexW, LocalFree, GetCommandLineW, GetSystemTimeAsFileTime, IsProcessorFeaturePresent, IsDebuggerPresent, DecodePointer, EncodePointer, GetVersion, GetUserDefaultUILanguage, LoadLibraryW, GetProcAddress, FreeLibrary, GetModuleHandleExA, WaitForMultipleObjectsEx, SleepEx, GetTickCount, UnmapViewOfFile, MapViewOfFile, CreateFileMappingW, SetFilePointer, GetFileSize, CreateEventW, DeleteFileA, GetTempPathW, GetModuleFileNameW, GetExitCodeThread, TerminateThread, GetCurrentThread, WriteFile, CreateFileW, CreateSemaphoreW, SignalObjectAndWait, ReleaseSemaphore, DeleteCriticalSection, GetModuleHandleW, GetCurrentDirectoryW, CreateEventA, WaitForMultipleObjects, ResetEvent, SetEvent, GetSystemDirectoryA, CreateProcessA, GetModuleFileNameA, LoadLibraryA, PeekNamedPipe, CreatePipe, ReadFile, GetExitCodeProcess, GetCurrentProcessId, ResumeThread, SetThreadPriority, SetThreadAffinityMask, QueryPerformanceFrequency, QueryPerformanceCounter, WideCharToMultiByte, MultiByteToWideChar, WaitForSingleObject, CreateThread, TerminateProcess, InitializeCriticalSectionAndSpinCount, FormatMessageA, TlsSetValue, TlsGetValue, TlsAlloc, SetLastError, Sleep, GetCurrentThreadId, SwitchToThread, GetSystemInfo, LeaveCriticalSection, EnterCriticalSection, InitializeCriticalSection, GetCurrentProcess, VirtualFree, VirtualAlloc
- **USER32.dll** (56): GetForegroundWindow, SetActiveWindow, SetFocus, IsWindowVisible, IsWindow, AttachThreadInput, SendMessageW, GetMonitorInfoW, MonitorFromPoint, LoadImageW, SetParent, GetParent, SetWindowLongW, GetWindowLongW, GetWindowRect, GetClientRect, GetWindowDC, UpdateWindow, SetMenu, GetMenu, SetWindowPlacement, GetWindowPlacement, SetWindowPos, ShowWindow, CreateWindowExW, RegisterClassExW, PostQuitMessage, DefWindowProcW, PostThreadMessageW, SendMessageCallbackW, SendNotifyMessageW, DispatchMessageW, TranslateMessage, GetMessageW, MessageBoxW, IsIconic, RegisterRawInputDevices, MapVirtualKeyW, ToUnicode, ToAscii, GetKeyboardState, MessageBoxA, GetWindowLongPtrW, CallWindowProcW, SetWindowLongPtrW, UnregisterDeviceNotification, GetRawInputData, ScreenToClient, GetCursorPos, SetCursorPos, ShowCursor, SetForegroundWindow, GetWindowThreadProcessId, MonitorFromWindow, GetAncestor, RegisterDeviceNotificationW
- **ADVAPI32.dll** (8): CryptDestroyHash, CryptHashData, CryptCreateHash, CryptGetHashParam, CryptReleaseContext, CryptAcquireContextW, SetSecurityDescriptorDacl, InitializeSecurityDescriptor
- **SHELL32.dll** (3): CommandLineToArgvW, ShellExecuteW, SHGetSpecialFolderPathW
- **HID.DLL** (12): HidP_GetButtonCaps, HidD_GetManufacturerString, HidD_GetSerialNumberString, HidP_GetCaps, HidD_GetProductString, HidP_GetValueCaps, HidD_GetAttributes, HidD_GetHidGuid, HidD_GetPreparsedData, HidD_FreePreparsedData, HidD_GetFeature, HidD_SetFeature
- **DINPUT8.dll** (1): DirectInput8Create
- **XINPUT1_3.dll** (1): #ord2
- **MSACM32.dll** (4): acmStreamSize, acmStreamConvert, acmStreamPrepareHeader, acmStreamOpen
- **MSVCP110.dll** (75): ?sync@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ, ?uflow@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHXZ, ??0?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@@Z, ??1?$basic_iostream@DU?$char_traits@D@std@@@std@@UEAA@XZ, ??_D?$basic_iostream@DU?$char_traits@D@std@@@std@@QEAAXXZ, ?pbackfail@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAHH@Z, ?seekoff@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA?AV?$fpos@H@2@_JHH@Z, ?seekpos@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA?AV?$fpos@H@2@V32@H@Z, ?always_noconv@codecvt_base@std@@QEBA_NXZ, ?in@?$codecvt@DDH@std@@QEBAHAEAHPEBD1AEAPEBDPEAD3AEAPEAD@Z, ?out@?$codecvt@DDH@std@@QEBAHAEAHPEBD1AEAPEBDPEAD3AEAPEAD@Z, ?unshift@?$codecvt@DDH@std@@QEBAHAEAHPEAD1AEAPEAD@Z, ?_Getcat@?$codecvt@DDH@std@@SA_KPEAPEBVfacet@locale@2@PEBV42@@Z, ??1ios_base@std@@UEAA@XZ, ??0?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAA@XZ, ??1?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAA@XZ, ?getloc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEBA?AVlocale@2@XZ, ?sputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAA_JPEBD_J@Z, ?setg@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXPEAD00@Z, ?_Gninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ, ?_Pninc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAPEADXZ, ?_Init@?$basic_streambuf@DU?$char_traits@D@std@@@std@@IEAAXXZ, ??1?$basic_ios@DU?$char_traits@D@std@@@std@@UEAA@XZ, ?clear@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z, ??0?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z, ??1?$basic_ostream@DU?$char_traits@D@std@@@std@@UEAA@XZ, ?_Add_vtordisp2@?$basic_ostream@DU?$char_traits@D@std@@@std@@UEAAXXZ, ??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAV01@AEAV01@@Z@Z, ??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@I@Z, ?endl@std@@YAAEAV?$basic_ostream@DU?$char_traits@D@std@@@1@AEAV21@@Z, ?_Fiopen@std@@YAPEAU_iobuf@@PEBDHH@Z, ?setbuf@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAPEAV12@PEAD_J@Z, ?showmanyc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JXZ, ?xsgetn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEAD_J@Z, ?xsputn@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAA_JPEBD_J@Z, ?_BADOFF@std@@3_JB, ?id@?$codecvt@DDH@std@@2V0locale@2@A, ??_7ios_base@std@@6B@, ??_7?$basic_ios@DU?$char_traits@D@std@@@std@@6B@, ?sbumpc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ, ?sgetc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ, ?snextc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHXZ, ?widen@?$basic_ios@DU?$char_traits@D@std@@@std@@QEBADD@Z, ??0?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA@PEAV?$basic_streambuf@DU?$char_traits@D@std@@@1@_N@Z, ??1?$basic_istream@DU?$char_traits@D@std@@@std@@UEAA@XZ, ?_Add_vtordisp1@?$basic_istream@DU?$char_traits@D@std@@@std@@UEAAXXZ, ?_Ipfx@?$basic_istream@DU?$char_traits@D@std@@@std@@QEAA_N_N@Z, ?_Add_vtordisp2@?$basic_ios@DU?$char_traits@D@std@@@std@@UEAAXXZ, ?_Xbad_function_call@std@@YAXXZ, ?imbue@?$basic_streambuf@DU?$char_traits@D@std@@@std@@MEAAXAEBVlocale@2@@Z, ?_Unlock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ, ?_Getgloballocale@locale@std@@CAPEAV_Locimp@12@XZ, ??Bid@locale@std@@QEAA_KXZ, ??1_Lockit@std@@QEAA@XZ, ??0_Lockit@std@@QEAA@H@Z, ??1_Container_base12@std@@QEAA@XZ, ?_Ios_base_dtor@ios_base@std@@CAXPEAV12@@Z, ?flush@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV12@XZ, ??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@H@Z, ?_Osfx@?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAXXZ, ?setstate@?$basic_ios@DU?$char_traits@D@std@@@std@@QEAAXH_N@Z, ?sputc@?$basic_streambuf@DU?$char_traits@D@std@@@std@@QEAAHD@Z, ?uncaught_exception@std@@YA_NXZ, ?set_new_handler@std@@YAP6AXXZP6AXXZ@Z, ?_Winerror_map@std@@YAPEBDH@Z, ?_Syserror_map@std@@YAPEBDH@Z, ?_Xout_of_range@std@@YAXPEBD@Z, ?_Xlength_error@std@@YAXPEBD@Z, ?_Xbad_alloc@std@@YAXXZ, ?_Orphan_all@_Container_base0@std@@QEAAXXZ, ?_Lock@?$basic_streambuf@DU?$char_traits@D@std@@@std@@UEAAXXZ, ?setw@std@@YA?AU?$_Smanip@_J@1@_J@Z, ??6?$basic_ostream@DU?$char_traits@D@std@@@std@@QEAAAEAV01@P6AAEAVios_base@1@AEAV21@@Z@Z, ?_Add_vtordisp1@?$basic_ios@DU?$char_traits@D@std@@@std@@UEAAXXZ, ??0?$basic_ios@DU?$char_traits@D@std@@@std@@IEAA@XZ
- **MSVCR110.dll** (191): _exit, _cexit, _ismbblead, _configthreadlocale, __setusermatherr, _initterm_e, _initterm, _acmdln, _fmode, _commode, __crt_debugger_hook, __crtUnhandledException, __crtTerminateProcess, __crtCaptureCurrentContext, __crtCapturePreviousContext, __crtSetUnhandledExceptionFilter, ?_type_info_dtor_internal_method@type_info@@QEAAXXZ, __getmainargs, _amsg_exit, __crtGetShowWindowMode, _XcptFilter, _onexit, __dllonexit, _calloc_crt, _unlock, _lock, ?terminate@@YAXXZ, _aligned_malloc, _aligned_free, _vsnprintf_s, vprintf, vfprintf, atan2, _atoi64, btowc, vsprintf_s, _strtoi64, atof, _HUGE, tanh, sinh, exp, cosh, atan, asin, modf, ldexp, frexp, ispunct, isxdigit, islower, isupper, tmpnam, rename, remove, _snprintf, acos, fmod, tan, srand, printf, log10f, pow, strnlen, powf, atan2f, _mktime64, wcsncpy_s, wcsncmp, _strnicmp, _stricmp, strcat_s, strcpy_s, memchr, _get_errno, tolower, toupper, memcmp, strchr, strncmp, strtol, memset, ?what@exception@std@@UEBAPEBDXZ, ??1exception@std@@UEAA@XZ, ??0exception@std@@QEAA@AEBV01@@Z, ??0exception@std@@QEAA@AEBQEBDH@Z, memcpy, __CxxFrameHandler3, _CxxThrowException, sprintf_s, memmove, exit, _purecall, log10, log, rand, strncpy, _strtoui64, fopen_s, mbstowcs_s, strtok_s, sin, cos, qsort, _itoa_s, _time64, isdigit, isalpha, tanf, logf, expf, atanf, asinf, acosf, ??1bad_cast@std@@UEAA@XZ, ??0bad_cast@std@@QEAA@AEBV01@@Z, ??0bad_cast@std@@QEAA@PEBD@Z, _unlock_file, _lock_file, ungetc, setvbuf, fwrite, _fseeki64, fsetpos, fputc, fgetpos, fgetc, fflush, ??0exception@std@@QEAA@XZ, fmodf, sscanf_s, ftell, fseek, fread, fopen, fclose, _localtime64, _gmtime64, _difftime64, strftime, clock, getenv, setlocale, tmpfile, _popen, _pclose, fscanf, fgets, localeconv, iscntrl, isalnum, strcoll, strncat, strcspn, strtoul, strtod, isspace, strpbrk, strerror, realloc, getc, fprintf, feof, __iob_func, _errno, _setjmp, longjmp, sprintf, sscanf, _write, atoi, strncat_s, strcmp, __C_specific_handler, strstr, malloc, free, _endthread, _beginthread, memcpy_s, swscanf, wcsstr, strrchr, strncpy_s, system, _beginthreadex, sinf, cosf, _vsnwprintf_s, signal, __set_app_type
- **PSAPI.DLL** (4): GetModuleBaseNameA, EnumProcessModules, GetProcessMemoryInfo, GetModuleInformation
- **WINMM.dll** (3): timeEndPeriod, timeGetTime, timeBeginPeriod
- **SETUPAPI.dll** (4): SetupDiDestroyDeviceInfoList, SetupDiGetClassDevsW, SetupDiEnumDeviceInterfaces, SetupDiGetDeviceInterfaceDetailW
- **dxgi.dll** (1): CreateDXGIFactory1
- **d3d11.dll** (1): D3D11CreateDevice
- **MFPlat.DLL** (5): MFStartup, MFCreateWaveFormatExFromMFMediaType, MFCreateMediaType, MFCreateAttributes, MFShutdown
- **MFReadWrite.dll** (1): MFCreateSourceReaderFromURL
- **GDI32.dll** (1): GetDeviceCaps
- **ole32.dll** (6): CoInitializeEx, CoSetProxyBlanket, CoCreateInstance, CoTaskMemFree, OleInitialize, CoUninitialize
- **OLEAUT32.dll** (2): SysFreeString, SysAllocString
- **WS2_32.dll** (30): socket, WSAGetLastError, WSAEventSelect, WSAIoctl, WSAStartup, WSACleanup, getsockopt, setsockopt, select, sendto, WSACloseEvent, WSACreateEvent, WSAEnumNetworkEvents, WSAResetEvent, WSASetEvent, WSAWaitForMultipleEvents, send, recv, listen, getsockname, getpeername, ioctlsocket, connect, closesocket, bind, accept, inet_addr, htons, htonl, recvfrom
- **IPHLPAPI.DLL** (1): GetAdaptersAddresses
- **WINHTTP.dll** (12): WinHttpOpen, WinHttpQueryHeaders, WinHttpReceiveResponse, WinHttpSendRequest, WinHttpOpenRequest, WinHttpSetStatusCallback, WinHttpSetTimeouts, WinHttpSetOption, WinHttpReadData, WinHttpConnect, WinHttpCloseHandle, WinHttpCrackUrl
- **advapi32.dll** (7): CryptAcquireContextA, CryptGetProvParam, GetUserNameW, RegCloseKey, RegOpenKeyExA, RegQueryValueExA, RegQueryValueExW
- **gdi32.dll** (6): CreateFontIndirectA, DeleteObject, GetObjectA, GetStockObject, GetTextExtentPoint32W, SetTextColor
- **kernel32.dll** (26): CreateDirectoryW, DeleteFileW, ExitProcess, FlushViewOfFile, FreeEnvironmentStringsA, FreeEnvironmentStringsW, GetComputerNameW, GetEnvironmentStrings, GetEnvironmentStringsW, GetFileAttributesA, GetModuleHandleA, GetProcessHeap, GetStdHandle, GetSystemDirectoryW, GetUserPreferredUILanguages, GetVolumeInformationW, GetWindowsDirectoryW, GlobalAlloc, GlobalFree, GlobalLock, GlobalUnlock, HeapAlloc, HeapFree, LoadLibraryExW, LocalAlloc, WriteConsoleA
- **ntdll.dll** (5): NtClose, NtCreateSection, NtMapViewOfSection, NtQuerySystemInformation, NtUnmapViewOfSection
- **shell32.dll** (2): SHGetFolderPathW, SHGetKnownFolderPath
- **shlwapi.dll** (2): PathFileExistsW, UrlEscapeW
- **user32.dll** (41): AppendMenuA, BeginPaint, CallWindowProcA, ClientToScreen, CloseClipboard, CreatePopupMenu, CreateWindowExA, DestroyWindow, DispatchMessageA, DrawTextW, EmptyClipboard, EnableWindow, EndPaint, GetCapture, GetClipboardData, GetDC, GetDesktopWindow, GetDlgCtrlID, GetDlgItem, GetMessageA, GetWindowLongPtrA, GetWindowTextA, GetWindowTextLengthA, GetWindowTextLengthW, GetWindowTextW, InvalidateRect, LoadCursorA, LoadIconA, OpenClipboard, PtInRect, ReleaseCapture, ReleaseDC, SendDlgItemMessageA, SendMessageA, SetCapture, SetClipboardData, SetCursor, SetWindowLongPtrA, SetWindowTextA, SetWindowTextW, TrackPopupMenu
- **wininet.dll** (7): HttpOpenRequestA, HttpQueryInfoA, HttpSendRequestA, InternetCloseHandle, InternetConnectA, InternetOpenA, InternetReadFile
- **ws2_32.dll** (1): htonl

Exports: 1: ExportProductMetadata

### DLL names referenced as strings
advapi32.dll[ascii], d3d11.dll[ascii], dinput8.dll[ascii], dtdata.dll[utf16], dxgi.dll[ascii], gdi32.dll[ascii], hid.dll[ascii], imm32.dll[ascii], iphlpapi.dll[ascii], kernel32.dll[ascii], loadall.dll[ascii], mfplat.dll[ascii], mfreadwrite.dll[ascii], msacm32.dll[ascii], msvcp110.dll[ascii], msvcr110.dll[ascii], ntdll.dll[ascii], ole32.dll[ascii], oleaut32.dll[ascii], psapi.dll[ascii], setupapi.dll[ascii], shell32.dll[ascii], shlwapi.dll[ascii], steam_api64.dll[ascii/utf16], steamclient64.dll[utf16], user32.dll[ascii], win32u.dll[utf16], winhttp.dll[ascii], wininet.dll[ascii], winmm.dll[ascii], ws2_32.dll[ascii], xaudio2_7.dll[ascii], xinput1_3.dll[ascii/utf16], xinput1_4.dll[utf16]
