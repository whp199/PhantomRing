## `eldenring.exe`

- path: `<steam-library>/common/ELDEN RING/Game/eldenring.exe`
- version resource: FileVersion 2.7.1.0 / ProductVersion 2.7.1.0 / InternalName ELDEN RING™ / OriginalFilename None / Company FromSoftware, Inc.
- size: 87042128 bytes; sha256 `1a3547101327f65d0c76da2f9190ac0aa66871ea42bae2aecc61e11a8b597891`
- TimeDateStamp: 0x6a96b418 (2026-09-01 11:16:40 UTC)
- ImageBase 0x140000000, SizeOfImage 0x5e0da00, SizeOfHeaders 0x600
- EntryPoint RVA 0x24fe160 in `.text`
- Subsystem 2, DllCharacteristics 0x8160 (HIGH_ENTROPY_VA, DYNAMIC_BASE(ASLR), NX_COMPAT, TERMINAL_SERVER_AWARE), Linker 14.0, OS 6.0, CheckSum 0x530afbc
- CodeView PDB: `W:\GR\RootBranch\Source\GR\Title\Project\vs2015\dist_win64_d3d12\MasterLTO_SteamWW\GR_win64_MasterLTO_SteamWW_d3d12.pdb` (debug ts 2026-09-01 11:16:40 UTC)

| section | RVA | VSize | raw off | raw size | flags | entropy |
|---|---|---|---|---|---|---|
| `.text` | 0x1000 | 0x29a4800 | 0x600 | 0x29a4800 | R-X CODE (0x60000020) | 6.583 |
| `.interpr` | 0x29a6000 | 0xc000 | 0x29a4e00 | 0xc000 | R-X CODE (0x60000020) | 6.465 |
| `.rdata` | 0x29b2000 | 0x1162800 | 0x29b0e00 | 0x1162800 | R-- IDATA (0x40000040) | 5.116 |
| `.data` | 0x3b15000 | 0xd51bc4 | 0x3b13600 | 0x249e00 | RW- IDATA (0xc0000040) | 4.695 |
| `.pdata` | 0x4867000 | 0x2b3200 | 0x3d5d400 | 0x2b3200 | R-- IDATA (0x40000040) | 7.126 |
| `.tls` | 0x4b1b000 | 0x200 | 0x4010600 | 0x200 | RW- IDATA (0xc0000040) | 4.883 |
| `.gfids` | 0x4b1c000 | 0x1200 | 0x4010800 | 0x1200 | R-- IDATA (0x40000040) | 4.176 |
| `.rsrc` | 0x4b1e000 | 0x35c00 | 0x4011a00 | 0x35c00 | R-- IDATA (0x40000040) | 7.434 |
| `.reloc` | 0x4b54000 | 0xb9e00 | 0x4047600 | 0xb9e00 | R-- IDATA (0x40000040) | 5.48 |
| `.idata` | 0x4c0e000 | 0x4200 | 0x4101400 | 0x4200 | RW- IDATA (0xc0000040) | 4.73 |
| `.text` | 0x4c13000 | 0x11faa00 | 0x4105600 | 0x11faa00 | R-X CODE (0x60000020) | 7.29 |

Data directories: EXPORT@0x3b07cf0+0x9934(.rdata), IMPORT@0x4c0e000+0x1e0(.idata), RESOURCE@0x4b1e000+0x35b90(.rsrc), EXCEPTION@0x4867000+0x2b3020(.pdata), SECURITY@0x5300000+0x2850(file-offset(Authenticode)), BASERELOC@0x5ce1990+0xea754(.text), DEBUG@0x32ad740+0x54(.rdata), TLS@0x32ad798+0x28(.rdata), IAT@0x4c11224+0xf20(.idata), DELAY_IMPORT@0x3b0774c+0x40(.rdata)

TLS: {'StartAddressOfRawData': '0x144b1b000', 'EndAddressOfRawData': '0x144b1b024', 'AddressOfIndex_rva': '0x485b020', 'AddressOfCallBacks_rva': '0x29ca5a0', 'callbacks': []}

Protector heuristics:
- Non-standard section name '.interpr' (R-X CODE, vsize 0xc000, entropy 6.465)
- Executable section .text entropy 7.29 (>7.2: packed/encrypted?)
- protector-name string 'GuardIT' x2: rva 0x2bd0ef7 (.rdata), rva 0x2bd0f18 (.rdata)
- protector-name string 'Arxan' x5: rva 0x308ab44 (.rdata), rva 0x3d3ee88 (.data), rva 0x3d3fc89 (.data), rva 0x3d40998 (.data)

Entry point disassembly:
```
0x24FE160: sub rsp, 0x28
0x24FE164: call 0x1dd1761
0x24FE169: add rsp, 0x28
0x24FE16D: jmp 0x24fdff4
0x24FE172: nop 
0x24FE173: jmp 0x24fddc8
0x24FE179: nop 
0x24FE17A: jb 0x24fe1f7
0x24FE17C: mov qword ptr [rsp + 0x10], rbx
0x24FE181: mov qword ptr [rsp + 0x18], rdi
0x24FE186: push rbp
0x24FE187: mov rbp, rsp
0x24FE18A: sub rsp, 0x20
0x24FE18E: and dword ptr [rbp - 0x18], 0
0x24FE192: xor ecx, ecx
0x24FE194: xor eax, eax
0x24FE196: mov dword ptr [rip + 0x1760c94], 2
0x24FE1A0: cpuid 
0x24FE1A2: mov r8d, ecx
0x24FE1A5: mov dword ptr [rip + 0x1760c81], 1
0x24FE1AF: xor ecx, 0x444d4163
0x24FE1B5: mov r9d, edx
0x24FE1B8: mov r10d, edx
```

### Watch-list imports
- **d3d12.dll**: D3D12SerializeRootSignature, #ord102, #ord101
- **dxgi.dll**: CreateDXGIFactory1, CreateDXGIFactory
- **winmm.dll**: timeGetTime, timeEndPeriod, timeBeginPeriod
- **steam_api64.dll**: SteamInternal_FindOrCreateUserInterface, SteamInternal_ContextInit, SteamAPI_GetHSteamUser, SteamAPI_RegisterCallback, SteamAPI_Init, SteamAPI_Shutdown, SteamAPI_RestartAppIfNecessary, SteamAPI_ReleaseCurrentThreadMemory, SteamAPI_RegisterCallResult, SteamAPI_UnregisterCallResult, SteamInternal_CreateInterface, SteamAPI_RunCallbacks, SteamAPI_UnregisterCallback
- **kernel32.dll**: IsDebuggerPresent, SetUnhandledExceptionFilter, LoadLibraryExA, GetModuleHandleA, QueryPerformanceCounter, Sleep, VirtualAlloc, OutputDebugStringW, GetProcAddress, LoadLibraryA, WaitForSingleObject, SetEvent, QueryPerformanceFrequency, OutputDebugStringA, CreateThread, GetModuleHandleW, LoadLibraryW, SleepEx, GetTickCount, VirtualProtect, CreateEventW, LoadLibraryExW, AddVectoredExceptionHandler
- **user32.dll**: DefWindowProcW, RegisterClassExW, CreateWindowExW, SetCursor, GetForegroundWindow, PeekMessageW, TranslateMessage, DispatchMessageW, MapVirtualKeyW, CallWindowProcW, SetWindowLongPtrW, GetKeyboardState, GetKeyState, SetFocus, SetWindowPos, ShowWindow, SetForegroundWindow, ClipCursor, ScreenToClient, ClientToScreen, GetCursorPos, SetCursorPos, ShowCursor, GetClientRect, GetMessageW
- **xinput1_4.dll**: #ord3, #ord2
- **dinput8.dll**: DirectInput8Create

### Proxy DLL viability evidence
| DLL | evidence | KnownDLL |
|---|---|---|
| dinput8.dll | static import; name string in image (ascii) | no |
| dxgi.dll | static import; name string in image (ascii) | no |
| d3d12.dll | static import; name string in image (ascii) | no |
| winmm.dll | static import; name string in image (ascii) | no |
| winhttp.dll | static import; name string in image (ascii) | no |
| xinput1_3.dll | name string in image (utf16) | no |
| xinput1_4.dll | static import; name string in image (ascii,utf16) | no |
| xinput9_1_0.dll | name string in image (utf16) | no |
| dbghelp.dll | name string in image (utf16) | no |
| steam_api64.dll | static import; name string in image (ascii) | no |
| bink2w64.dll | static import; name string in image (ascii) | no |
| amd_ags_x64.dll | static import; name string in image (ascii) | no |
| oo2core_6_win64.dll | static import; name string in image (ascii) | no |
| ws2_32.dll | static import; name string in image (ascii) | yes |
| setupapi.dll | static import; name string in image (ascii,utf16) | yes |
| crypt32.dll | static import; name string in image (ascii) | yes |
| xaudio2_7.dll | name string in image (ascii) | no |
| xaudio2_9.dll | name string in image (ascii) | no |
| nvapi64.dll | name string in image (utf16) | no |

### Full import table
- **d3d12.dll** (3): D3D12SerializeRootSignature, #ord102, #ord101
- **dxgi.dll** (2): CreateDXGIFactory1, CreateDXGIFactory
- **WINHTTP.dll** (14): WinHttpSetStatusCallback, WinHttpOpen, WinHttpCloseHandle, WinHttpConnect, WinHttpQueryOption, WinHttpSetOption, WinHttpOpenRequest, WinHttpCrackUrl, WinHttpWebSocketClose, WinHttpWebSocketReceive, WinHttpWebSocketSend, WinHttpSendRequest, WinHttpWebSocketCompleteUpgrade, WinHttpReceiveResponse
- **bink2w64.dll** (24): BinkPause, BinkGoto, BinkShouldSkip, BinkControlBackgroundIO, BinkDoFrameAsyncWait, BinkGetRealtime, BinkClose, BinkSetIOSize, BinkStartAsyncThread, BinkDoFrameAsyncMulti, BinkRequestStopAsyncThread, BinkWaitStopAsyncThread, BinkSetMemory, BinkSetVolume, BinkSetSpeakerVolumes, BinkWait, BinkNextFrame, BinkOpen, BinkSetSoundTrack, BinkAllocateFrameBuffers, BinkRegisterFrameBuffers, BinkGetFrameBuffersInfo, BinkOpenXAudio2, BinkSetSoundSystem2
- **amd_ags_x64.dll** (3): agsDeInit, agsSetDisplayMode, agsInit
- **WS2_32.dll** (41): gethostbyname, ntohs, recv, recvfrom, select, send, sendto, WSASocketW, ntohl, listen, htons, htonl, connect, closesocket, bind, accept, __WSAFDIsSet, freeaddrinfo, inet_ntop, getpeername, getsockopt, WSASetLastError, ioctlsocket, setsockopt, socket, gethostname, WSAStartup, WSACleanup, WSAGetLastError, inet_ntoa, getsockname, WSACloseEvent, WSACreateEvent, WSAEnumNetworkEvents, WSAEventSelect, WSAWaitForMultipleEvents, inet_addr, shutdown, inet_pton, getnameinfo, getaddrinfo
- **oo2core_6_win64.dll** (6): OodleLZ_GetCompressedStepForRawStep, OodleLZDecoder_MemorySizeNeeded, OodleLZ_Decompress, OodleLZ_GetCompressedBufferSizeNeeded, OodleLZ_CompressOptions_GetDefault, OodleLZ_Compress
- **WINMM.dll** (3): timeGetTime, timeEndPeriod, timeBeginPeriod
- **steam_api64.dll** (13): SteamInternal_FindOrCreateUserInterface, SteamInternal_ContextInit, SteamAPI_GetHSteamUser, SteamAPI_RegisterCallback, SteamAPI_Init, SteamAPI_Shutdown, SteamAPI_RestartAppIfNecessary, SteamAPI_ReleaseCurrentThreadMemory, SteamAPI_RegisterCallResult, SteamAPI_UnregisterCallResult, SteamInternal_CreateInterface, SteamAPI_RunCallbacks, SteamAPI_UnregisterCallback
- **CRYPT32.dll** (7): CertOpenStore, CertCloseStore, CertEnumCertificatesInStore, CertFindCertificateInStore, CertDuplicateCertificateContext, CertFreeCertificateContext, CertGetCertificateContextProperty
- **KERNEL32.dll** (208): CreateTimerQueueTimer, GetLogicalProcessorInformation, SwitchToThread, SignalObjectAndWait, CreateTimerQueue, GetProcessHeap, HeapFree, HeapAlloc, InitializeSListHead, GetStartupInfoW, IsDebuggerPresent, IsProcessorFeaturePresent, TerminateProcess, SetUnhandledExceptionFilter, ChangeTimerQueueTimer, RtlLookupFunctionEntry, RtlCaptureContext, GetCPInfo, GetLocaleInfoW, LCMapStringW, CompareStringW, DecodePointer, EncodePointer, GetStringTypeW, GetNativeSystemInfo, DuplicateHandle, LoadLibraryExA, GetNumaHighestNodeNumber, RegisterWaitForSingleObject, UnhandledExceptionFilter, DeleteTimerQueueTimer, UnregisterWait, GetThreadTimes, FreeLibraryAndExitThread, GetModuleHandleA, GetVersionExW, InterlockedPopEntrySList, InterlockedPushEntrySList, InterlockedFlushSList, QueryDepthSList, UnregisterWaitEx, RtlPcToFileHeader, RtlUnwindEx, ExitThread, SystemTimeToTzSpecificLocalTime, ExitProcess, SetFilePointerEx, SetConsoleCtrlHandler, HeapReAlloc, GetConsoleCP, HeapSize, HeapQueryInformation, GetDateFormatW, GetTimeFormatW, IsValidLocale, GetUserDefaultLCID, EnumSystemLocalesW, GetFileSizeEx, SetStdHandle, GetExitCodeProcess, GetFileAttributesExW, CreatePipe, IsValidCodePage, GetACP, GetOEMCP, FindFirstFileExW, GetCommandLineA, GetEnvironmentStringsW, FreeEnvironmentStringsW, TlsGetValue, QueryPerformanceCounter, DeleteFileW, MoveFileW, GetComputerNameW, GetTempPathW, EnterCriticalSection, LeaveCriticalSection, SetThreadPriority, GetCommandLineW, CloseHandle, GetLastError, CreateMutexW, GetCurrentThread, LocalFree, Sleep, VirtualAlloc, SetEnvironmentVariableW, OutputDebugStringW, OpenThread, SetThreadIdealProcessor, FreeLibrary, GetProcAddress, LoadLibraryA, WaitForSingleObject, CreateEventExA, GetTimeZoneInformation, SetEvent, ResetEvent, ReleaseSemaphore, ReleaseMutex, CreateMutexA, CreateEventA, GetCurrentThreadId, SuspendThread, ResumeThread, GetSystemInfo, SetThreadAffinityMask, CreateSemaphoreA, VirtualFree, InitializeCriticalSection, DeleteCriticalSection, SetErrorMode, QueryPerformanceFrequency, OutputDebugStringA, GetCurrentProcessId, TlsAlloc, TlsSetValue, TlsFree, InitializeCriticalSectionAndSpinCount, RaiseException, CreateThread, GetThreadPriority, GetExitCodeThread, GetModuleHandleW, CreateSemaphoreW, GetCurrentProcess, GetModuleFileNameA, LoadLibraryW, VirtualQuery, CreateDirectoryW, CreateFileW, FindClose, FindFirstFileW, FindNextFileW, GetFileAttributesW, GetFullPathNameW, RemoveDirectoryW, SetFileAttributesW, CancelIo, SleepEx, WaitForSingleObjectEx, QueueUserAPC, GetSystemTimeAsFileTime, CopyFileW, MoveFileExW, ReadDirectoryChangesW, VerSetConditionMask, SetHandleInformation, VerifyVersionInfoW, FlushFileBuffers, SetFilePointer, WriteFile, GlobalAlloc, GlobalLock, GlobalUnlock, SetLastError, GetTickCount, FormatMessageA, GetStdHandle, GetFileType, ReadFile, PeekNamedPipe, WaitForMultipleObjects, VirtualProtect, CreateEventW, LocalAlloc, GetSystemDirectoryW, LoadLibraryExW, CreateProcessW, GetSystemDefaultLangID, GetSystemDefaultLCID, FileTimeToLocalFileTime, LocalFileTimeToFileTime, GetLocalTime, FileTimeToSystemTime, SystemTimeToFileTime, TryEnterCriticalSection, PulseEvent, TerminateThread, GetProcessAffinityMask, SetProcessAffinityMask, GetTempFileNameW, GetUserDefaultLangID, GetDriveTypeW, GetModuleFileNameW, GetFileInformationByHandle, SetEndOfFile, GetOverlappedResult, FormatMessageW, CreateFileA, GetDiskFreeSpaceW, WideCharToMultiByte, MultiByteToWideChar, WaitForMultipleObjectsEx, GetModuleHandleExA, GetDiskFreeSpaceExW, DeleteFiber, ConvertFiberToThread, RtlVirtualUnwind, GlobalMemoryStatus, GetEnvironmentVariableW, GetConsoleMode, ReadConsoleA, ReadConsoleW, SetConsoleMode, WriteConsoleW, GetCurrentDirectoryW, GetModuleHandleExW
- **USER32.dll** (63): SetWindowLongW, LoadImageW, GetMessageExtraInfo, PostMessageW, DefWindowProcW, RegisterClassExW, CreateWindowExW, DestroyWindow, IsZoomed, SendInput, SetTimer, KillTimer, GetSystemMetrics, GetSystemMenu, DeleteMenu, UpdateWindow, GetWindowRect, SetCursor, GetDesktopWindow, LoadCursorW, LoadIconW, GetForegroundWindow, PeekMessageW, GetWindowLongW, TranslateMessage, DispatchMessageW, PostQuitMessage, OpenClipboard, EmptyClipboard, SetClipboardData, CloseClipboard, GetClipboardData, MapVirtualKeyW, RegisterDeviceNotificationW, GetActiveWindow, MessageBoxW, UnregisterDeviceNotification, CallWindowProcW, GetWindowLongPtrW, SetWindowLongPtrW, GetProcessWindowStation, GetUserObjectInformationW, ToAscii, GetKeyboardState, GetKeyState, GetKeyboardLayoutNameW, AdjustWindowRect, SetActiveWindow, SetFocus, IsIconic, SetWindowPos, ShowWindow, SetForegroundWindow, SendMessageW, ClipCursor, ScreenToClient, ClientToScreen, GetCursorPos, SetCursorPos, ShowCursor, GetClientRect, FindWindowW, GetMessageW
- **GDI32.dll** (2): CreateBitmap, DeleteObject
- **ADVAPI32.dll** (24): CryptAcquireContextA, CryptDestroyKey, CryptGenRandom, ReportEventW, RegisterEventSourceW, DeregisterEventSource, SystemFunction036, CryptAcquireContextW, CryptDestroyHash, CryptHashData, CryptCreateHash, CryptEnumProvidersW, CryptSignHashW, CryptDecrypt, CryptExportKey, CryptGetUserKey, CryptGetProvParam, InitializeSecurityDescriptor, SetSecurityDescriptorDacl, GetUserNameW, GetUserNameA, CryptGetHashParam, CryptReleaseContext, CryptSetHashParam
- **SHELL32.dll** (5): SHGetFolderPathW, DragQueryFileW, DragAcceptFiles, DragFinish, CommandLineToArgvW
- **ole32.dll** (9): PropVariantClear, CoInitializeEx, CoCreateInstance, CoSetProxyBlanket, CoUninitialize, CoTaskMemFree, CoCreateGuid, CoTaskMemAlloc, CoInitialize
- **OLEAUT32.dll** (3): VariantClear, SysFreeString, SysAllocString
- **IMM32.dll** (1): ImmDisableIME
- **SETUPAPI.dll** (5): SetupDiGetDeviceInstanceIdW, SetupDiGetClassDevsW, SetupDiGetDeviceInterfaceDetailW, SetupDiEnumDeviceInterfaces, SetupDiDestroyDeviceInfoList
- **WLDAP32.dll** (16): #ord79, #ord30, #ord200, #ord301, #ord35, #ord33, #ord32, #ord27, #ord26, #ord22, #ord41, #ord50, #ord60, #ord211, #ord46, #ord143
- **XINPUT1_4.dll** (2): #ord3, #ord2
- **DINPUT8.dll** (1): DirectInput8Create
- **KERNEL32.DLL** (6): GetThreadContext, AddVectoredExceptionHandler, RemoveVectoredExceptionHandler, HeapLock, HeapWalk, HeapUnlock

Delay imports:
- **EOSSDK-Win64-Shipping.dll** (32): EOS_ByteArray_ToString, EOS_ProductUserId_IsValid, EOS_ProductUserId_ToString, EOS_ProductUserId_FromString, EOS_Initialize, EOS_Shutdown, EOS_Platform_Create, EOS_Platform_Release, EOS_Connect_Login, EOS_Connect_CreateUser, EOS_Connect_GetLoginStatus, EOS_Connect_AddNotifyAuthExpiration, EOS_Connect_RemoveNotifyAuthExpiration, EOS_Connect_AddNotifyLoginStatusChanged, EOS_Connect_RemoveNotifyLoginStatusChanged, EOS_Platform_Tick, EOS_Platform_GetConnectInterface, EOS_Platform_GetAntiCheatClientInterface, EOS_Platform_GetReportsInterface, EOS_Logging_SetCallback, EOS_Logging_SetLogLevel, EOS_AntiCheatClient_AddNotifyMessageToPeer, EOS_AntiCheatClient_AddNotifyPeerActionRequired, EOS_AntiCheatClient_RemoveNotifyPeerActionRequired, EOS_AntiCheatClient_AddNotifyPeerAuthStatusChanged, EOS_AntiCheatClient_RemoveNotifyPeerAuthStatusChanged, EOS_AntiCheatClient_BeginSession, EOS_AntiCheatClient_EndSession, EOS_AntiCheatClient_PollStatus, EOS_AntiCheatClient_RegisterPeer, EOS_AntiCheatClient_UnregisterPeer, EOS_AntiCheatClient_ReceiveMessageFromPeer

Exports: 560: ??0BufferedFile@Scaleform@@IEAA@XZ, ??0BufferedFile@Scaleform@@QEAA@PEAVFile@1@@Z, ??0Event@Scaleform@@QEAA@_N0@Z, ??0Mutex@Scaleform@@QEAA@_N0@Z, ??0ReadBytesMem@AK@@QEAA@PEBXJ@Z, ??0ReadBytesMem@AK@@QEAA@XZ, ??0ReadBytesSkip@AK@@QEAA@PEBXJ@Z, ??0ReadBytesSkip@AK@@QEAA@XZ, ??0Semaphore@Scaleform@@QEAA@H_N@Z, ??0SysAllocPagedMalloc@Scaleform@@QEAA@_K@Z, ??0SysAllocStatic@Scaleform@@QEAA@PEAX_K010101@Z, ??0SysAllocStatic@Scaleform@@QEAA@_K@Z, ??0SysAllocWinAPI@Scaleform@@QEAA@_K0@Z, ??0SysFile@Scaleform@@QEAA@AEBVString@1@HH@Z, ??0SysFile@Scaleform@@QEAA@XZ, ??0System@Scaleform@@QEAA@AEBUHeapDesc@MemoryHeap@1@PEAVSysAllocBase@1@@Z, ??0System@Scaleform@@QEAA@PEAVSysAllocBase@1@@Z, ??0Thread@Scaleform@@QEAA@AEBUCreateParams@01@@Z, ??0Thread@Scaleform@@QEAA@P6AHPEAV01@PEAX@Z1_KHW4ThreadState@01@@Z, ??0Thread@Scaleform@@QEAA@_KH@Z, ??0WaitCondition@Scaleform@@QEAA@XZ, ??0WriteBytesCount@AK@@QEAA@XZ, ??0WriteBytesMem@AK@@QEAA@XZ, ??1AcquireInterface@Scaleform@@UEAA@XZ, ??1BufferedFile@Scaleform@@UEAA@XZ, ??1CAkEmitter@@UEAA@XZ, ??1CAkGameObject@@UEAA@XZ, ??1CAkRegisteredObj@@UEAA@XZ, ??1DefaultAcquireInterface@Scaleform@@UEAA@XZ, ??1Event@Scaleform@@UEAA@XZ, ??1Loader@GFx@Scaleform@@UEAA@XZ, ??1Mutex@Scaleform@@UEAA@XZ, ??1ReadBytesMem@AK@@UEAA@XZ, ??1ReadBytesSkip@AK@@UEAA@XZ, ??1RefCountImplCore@Scaleform@@UEAA@XZ, ??1RefCountNTSImplCore@Scaleform@@UEAA@XZ, ??1RefCountWeakSupportImpl@Scaleform@@UEAA@XZ, ??1Semaphore@Scaleform@@UEAA@XZ, ??1System@Scaleform@@QEAA@XZ, ??1Thread@Scaleform@@UEAA@XZ, ??1WaitCondition@Scaleform@@QEAA@XZ, ??1WriteBytesCount@AK@@UEAA@XZ, ??1WriteBytesMem@AK@@UEAA@XZ, ??ESemaphore@Scaleform@@QEAAHH@Z, ??FSemaphore@Scaleform@@QEAAHH@Z, ??YSemaphore@Scaleform@@QEAAHH@Z, ??ZSemaphore@Scaleform@@QEAAHH@Z, ??_FEvent@Scaleform@@QEAAXXZ, ??_FMutex@Scaleform@@QEAAXXZ, ??_FSemaphore@Scaleform@@QEAAXXZ

### DLL names referenced as strings
advapi32.dll[ascii/utf16], amd_ags_x64.dll[ascii], api-ms-win-core-registry-l1-1-0.dll[utf16], bink2w64.dll[ascii], combase.dll[utf16], crypt32.dll[ascii], d3d12.dll[ascii], dbghelp.dll[utf16], dinput8.dll[ascii], dxgi.dll[ascii], eossdk-win64-shipping.dll[ascii], gdi32.dll[ascii], hkcompatformats.dll[ascii], imm32.dll[ascii], kernel32.dll[ascii], libscepad_x64.dll[utf16], mscoree.dll[utf16], mspdb140.dll[utf16], nvapi64.dll[utf16], nvpowerapi.dll[utf16], ole32.dll[ascii], oleaut32.dll[ascii], oo2core_6_win64.dll[ascii], setupapi.dll[ascii/utf16], shell32.dll[ascii/utf16], steam_api64.dll[ascii], user32.dll[ascii], winhttp.dll[ascii], winmm.dll[ascii], wldap32.dll[ascii], ws2_32.dll[ascii], xaudio2_7.dll[ascii], xaudio2_8.dll[ascii], xaudio2_9.dll[ascii], xinput1_3.dll[utf16], xinput1_4.dll[ascii/utf16], xinput9_1_0.dll[utf16]
