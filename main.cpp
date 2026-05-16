#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <atomic>

// Link required libraries (for MSVC compiler)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// Macro for safe COM release
#define SAFE_RELEASE(punk) if ((punk) != NULL) { (punk)->Release(); (punk) = NULL; }

// Global UI Handles
HWND hComboTarget, hStartBtn, hStatusLabel;
HFONT hFont;

// Threading and State
std::atomic<bool> g_bIsRouting{ false };
std::thread g_AudioThread;

// Store device IDs so we can activate them later based on combobox selection
std::vector<std::wstring> g_DeviceIds;

const CLSID CLSID_MMDeviceEnumerator = __uuidof(MMDeviceEnumerator);
const IID IID_IMMDeviceEnumerator = __uuidof(IMMDeviceEnumerator);
const IID IID_IAudioClient = __uuidof(IAudioClient);
const IID IID_IAudioCaptureClient = __uuidof(IAudioCaptureClient);
const IID IID_IAudioRenderClient = __uuidof(IAudioRenderClient);

void PopulateDeviceComboBoxes(HWND hCombo) {
    HRESULT hr;
    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDeviceCollection* pEndpoints = NULL;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);
    if (FAILED(hr)) return;

    hr = pEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &pEndpoints);
    if (FAILED(hr)) {
        SAFE_RELEASE(pEnumerator);
        return;
    }

    UINT count = 0;
    pEndpoints->GetCount(&count);

    for (UINT i = 0; i < count; i++) {
        IMMDevice* pDevice = NULL;
        hr = pEndpoints->Item(i, &pDevice);
        if (SUCCEEDED(hr)) {
            IPropertyStore* pProps = NULL;
            hr = pDevice->OpenPropertyStore(STGM_READ, &pProps);
            if (SUCCEEDED(hr)) {
                PROPVARIANT varName;
                PropVariantInit(&varName);

                hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
                if (SUCCEEDED(hr) && varName.pwszVal != NULL) {
                    // Add string to combobox
                    SendMessageW(hCombo, CB_ADDSTRING, 0, (LPARAM)varName.pwszVal);

                    // Populate the ID array
                    LPWSTR pwszID = NULL;
                    hr = pDevice->GetId(&pwszID);
                    if (SUCCEEDED(hr)) {
                        g_DeviceIds.push_back(pwszID);
                        CoTaskMemFree(pwszID);
                    }
                }
                PropVariantClear(&varName);
                SAFE_RELEASE(pProps);
            }
            SAFE_RELEASE(pDevice);
        }
    }
    SAFE_RELEASE(pEndpoints);
    SAFE_RELEASE(pEnumerator);

    // Select the first item by default
    SendMessage(hCombo, CB_SETCURSEL, 0, 0);
}

void AudioRoutingThread(std::wstring targetDeviceId) {
    HRESULT hr;
    // Must initialize COM for this background thread
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    DWORD renderFlags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    IMMDeviceEnumerator* pEnumerator = NULL;
    IMMDevice* pLoopbackDevice = NULL, * pRenderDev = NULL;
    IAudioClient* pCapClient = NULL, * pRenClient = NULL;
    IAudioCaptureClient* pCaptureClient = NULL;
    IAudioRenderClient* pRenderClient = NULL;
    WAVEFORMATEX* pwfx = NULL;

    CoCreateInstance(CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void**)&pEnumerator);

    // 1. Setup Loopback Capture on the Default Output Device
    pEnumerator->GetDefaultAudioEndpoint(eRender, eConsole, &pLoopbackDevice);
    pLoopbackDevice->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pCapClient);
    pCapClient->GetMixFormat(&pwfx);

    hr = pCapClient->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0, pwfx, NULL);
    if (FAILED(hr)) goto Cleanup;
    pCapClient->GetService(IID_IAudioCaptureClient, (void**)&pCaptureClient);

    // 2. Setup Render Device with Auto-Convert
    pEnumerator->GetDevice(targetDeviceId.c_str(), &pRenderDev);
    pRenderDev->Activate(IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&pRenClient);
    hr = pRenClient->Initialize(AUDCLNT_SHAREMODE_SHARED, renderFlags, 0, 0, pwfx, NULL);
    if (FAILED(hr)) goto Cleanup;
    pRenClient->GetService(IID_IAudioRenderClient, (void**)&pRenderClient);

    // 3. Start the Streams
    pCapClient->Start();
    pRenClient->Start();

    while (g_bIsRouting) {
        Sleep(10); // Wait for buffers to fill (approx 10ms)
        UINT32 packetLength = 0;
        pCaptureClient->GetNextPacketSize(&packetLength);

        while (packetLength != 0 && g_bIsRouting) {
            BYTE* pData;
            UINT32 numFramesAvailable;
            DWORD flags;
            hr = pCaptureClient->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);

            if (SUCCEEDED(hr)) {
                UINT32 bytesToCopy = numFramesAvailable * pwfx->nBlockAlign;

                // Push to Output Device
                BYTE* pRenderData;
                if (SUCCEEDED(pRenderClient->GetBuffer(numFramesAvailable, &pRenderData))) {
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && pData != NULL) {
                        memcpy(pRenderData, pData, bytesToCopy);
                        pRenderClient->ReleaseBuffer(numFramesAvailable, 0); // 0 = Valid Data
                    }
                    else {
                        // Tell the Render client this buffer is intentionally silent
                        pRenderClient->ReleaseBuffer(numFramesAvailable, AUDCLNT_BUFFERFLAGS_SILENT);
                    }
                }

                pCaptureClient->ReleaseBuffer(numFramesAvailable);
            }
            pCaptureClient->GetNextPacketSize(&packetLength);
        }
    }

    // Stop streams
    pCapClient->Stop();
    pRenClient->Stop();

Cleanup:
    if (pwfx) CoTaskMemFree(pwfx);
    SAFE_RELEASE(pCaptureClient); SAFE_RELEASE(pRenderClient);
    SAFE_RELEASE(pCapClient); SAFE_RELEASE(pRenClient);
    SAFE_RELEASE(pLoopbackDevice); SAFE_RELEASE(pRenderDev);
    SAFE_RELEASE(pEnumerator);

    CoUninitialize();
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
    case WM_CREATE: {
        // Create Font
        hFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS, "Segoe UI");

        // Create Target Label
        HWND hLblTarget = CreateWindowA("STATIC", "Duplicate audio to:", WS_VISIBLE | WS_CHILD, 20, 20, 340, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hLblTarget, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Create Target ComboBox
        hComboTarget = CreateWindowA("COMBOBOX", "", CBS_DROPDOWNLIST | WS_CHILD | WS_VISIBLE | WS_VSCROLL, 20, 40, 340, 200, hwnd, NULL, NULL, NULL);
        SendMessage(hComboTarget, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Create Button (Moved up slightly)
        hStartBtn = CreateWindowA("BUTTON", "Start Duplication", WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON, 20, 90, 340, 40, hwnd, (HMENU)1, NULL, NULL);
        SendMessage(hStartBtn, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Create Status Label (Moved up slightly)
        hStatusLabel = CreateWindowA("STATIC", "Status: Ready", WS_VISIBLE | WS_CHILD, 20, 140, 340, 20, hwnd, NULL, NULL, NULL);
        SendMessage(hStatusLabel, WM_SETFONT, (WPARAM)hFont, TRUE);

        // Initialize COM for the Main UI Thread and populate UI
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        PopulateDeviceComboBoxes(hComboTarget);
        break;
    }

    case WM_COMMAND: {
        if (LOWORD(wParam) == 1) { // Start/Stop Button Clicked
            if (!g_bIsRouting) {
                int sel = SendMessage(hComboTarget, CB_GETCURSEL, 0, 0);

                if (sel != CB_ERR && sel < g_DeviceIds.size()) {
                    g_bIsRouting = true;
                    SetWindowTextA(hStartBtn, "Stop Duplication");
                    SetWindowTextA(hStatusLabel, "Status: Running...");

                    // Launch Audio pump on background thread targeting single device
                    g_AudioThread = std::thread(AudioRoutingThread, g_DeviceIds[sel]);
                }
            }
            else {
                g_bIsRouting = false;
                SetWindowTextA(hStartBtn, "Start Duplication");
                SetWindowTextA(hStatusLabel, "Status: Stopping...");

                // Wait for the background thread to safely exit
                if (g_AudioThread.joinable()) {
                    g_AudioThread.join();
                }

                // Update the UI from the Main Thread after the background thread has safely closed
                SetWindowTextA(hStatusLabel, "Status: Stopped.");
            }
        }
        break;
    }
    case WM_DESTROY: {
        if (g_bIsRouting) {
            g_bIsRouting = false;
            if (g_AudioThread.joinable()) g_AudioThread.join();
        }
        DeleteObject(hFont);
        CoUninitialize();
        PostQuitMessage(0);
        break;
    }
    default:
        return DefWindowProcA(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    const char CLASS_NAME[] = "AudioDuplicatorClass";

    WNDCLASSA wc = { };
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClassA(&wc);

    // Create the fixed-size window (Height reduced from 260 to 210)
    HWND hwnd = CreateWindowExA(
        0, CLASS_NAME, "Audio Duplicator",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, // Non-resizable
        CW_USEDEFAULT, CW_USEDEFAULT, 400, 210,
        NULL, NULL, hInstance, NULL
    );

    if (hwnd == NULL) return 0;

    ShowWindow(hwnd, nCmdShow);

    // Run the message loop
    MSG msg = { };
    while (GetMessageA(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    return 0;
}
