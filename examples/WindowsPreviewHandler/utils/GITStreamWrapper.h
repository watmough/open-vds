/****************************************************************************
** Copyright 2019 The Open Group
** Copyright 2019 Bluware, Inc.
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
****************************************************************************/

#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objidl.h>
#include <new>
#include <fstream>
#include <string>

// Debug logging for GIT wrapper
static void LogGIT(const char* msg)
{
    char expandedPath[MAX_PATH];
    DWORD result = ExpandEnvironmentStringsA("%USERPROFILE%\\AppData\\LocalLow\\Temp\\openvds-git.log", expandedPath, MAX_PATH);
    if (result == 0 || result > MAX_PATH)
        return;

    std::ofstream logFile(expandedPath, std::ios::app);
    if (logFile.is_open())
    {
        SYSTEMTIME st;
        GetLocalTime(&st);
        char timestamp[64];
        sprintf_s(timestamp, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        logFile << timestamp << msg << "\n";
        logFile.flush();
    }
}

// GITStreamWrapper: Wraps an IStream using the Global Interface Table (GIT)
// to allow cross-thread access. This is necessary because:
//
// 1. Preview handlers run in prevhost.exe (separate process from explorer.exe)
// 2. The shell marshals the IStream to the preview handler's thread
// 3. OpenVDS uses a thread pool for data requests (VolumeDataRequestProcessor)
// 4. Worker threads cannot directly call the marshaled IStream (COM apartment rules)
//
// The GIT solves this by:
// - Registering the original IStream in a global table during Initialize()
// - Each thread that calls IStream methods gets a thread-local proxy from GIT
// - COM handles all the cross-apartment marshaling automatically

class GITStreamWrapper : public IStream
{
public:
    // Create a GIT-wrapped stream from the original stream
    // Returns S_OK on success, error code on failure
    // On success, *ppWrapper receives the wrapped stream (caller owns reference)
    static HRESULT Create(IStream* pStream, IStream** ppWrapper)
    {
        char logBuf[128];
        sprintf_s(logBuf, "Create: pStream=%p, thread=%lu", pStream, GetCurrentThreadId());
        LogGIT(logBuf);

        if (!pStream || !ppWrapper)
        {
            LogGIT("Create: E_INVALIDARG");
            return E_INVALIDARG;
        }

        *ppWrapper = nullptr;

        // Create the Global Interface Table
        IGlobalInterfaceTable* pGIT = nullptr;
        HRESULT hr = CoCreateInstance(
            CLSID_StdGlobalInterfaceTable,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_IGlobalInterfaceTable,
            reinterpret_cast<void**>(&pGIT));

        if (FAILED(hr) || !pGIT)
        {
            sprintf_s(logBuf, "Create: CoCreateInstance failed hr=0x%08X", hr);
            LogGIT(logBuf);
            return hr;
        }

        LogGIT("Create: GIT created");

        // Register the stream in the GIT
        DWORD cookie = 0;
        hr = pGIT->RegisterInterfaceInGlobal(pStream, IID_IStream, &cookie);
        if (FAILED(hr))
        {
            sprintf_s(logBuf, "Create: RegisterInterfaceInGlobal failed hr=0x%08X", hr);
            LogGIT(logBuf);
            pGIT->Release();
            return hr;
        }

        sprintf_s(logBuf, "Create: Registered, cookie=%lu", cookie);
        LogGIT(logBuf);

        // Create the wrapper
        *ppWrapper = new (std::nothrow) GITStreamWrapper(pGIT, cookie);
        if (!*ppWrapper)
        {
            LogGIT("Create: E_OUTOFMEMORY");
            pGIT->RevokeInterfaceFromGlobal(cookie);
            pGIT->Release();
            return E_OUTOFMEMORY;
        }

        LogGIT("Create: Success");
        return S_OK;
    }

    // IUnknown methods
    STDMETHODIMP QueryInterface(REFIID riid, void** ppvObject) override
    {
        if (!ppvObject)
            return E_INVALIDARG;

        if (riid == IID_IUnknown || riid == IID_IStream || riid == IID_ISequentialStream)
        {
            *ppvObject = static_cast<IStream*>(this);
            AddRef();
            return S_OK;
        }

        *ppvObject = nullptr;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override
    {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override
    {
        ULONG refCount = InterlockedDecrement(&m_refCount);
        if (refCount == 0)
        {
            delete this;
        }
        return refCount;
    }

    // ISequentialStream methods
    STDMETHODIMP Read(void* pv, ULONG cb, ULONG* pcbRead) override
    {
        char logBuf[128];
        sprintf_s(logBuf, "Read: cb=%lu, thread=%lu", cb, GetCurrentThreadId());
        LogGIT(logBuf);

        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
        {
            sprintf_s(logBuf, "Read: GetStreamProxy failed hr=0x%08X", hr);
            LogGIT(logBuf);
            return hr;
        }

        LogGIT("Read: calling pStream->Read...");
        hr = pStream->Read(pv, cb, pcbRead);
        sprintf_s(logBuf, "Read: done hr=0x%08X, read=%lu", hr, pcbRead ? *pcbRead : 0);
        LogGIT(logBuf);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP Write(const void* pv, ULONG cb, ULONG* pcbWritten) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->Write(pv, cb, pcbWritten);
        pStream->Release();
        return hr;
    }

    // IStream methods
    STDMETHODIMP Seek(LARGE_INTEGER dlibMove, DWORD dwOrigin, ULARGE_INTEGER* plibNewPosition) override
    {
        char logBuf[128];
        sprintf_s(logBuf, "Seek: move=%lld, origin=%lu, thread=%lu", dlibMove.QuadPart, dwOrigin, GetCurrentThreadId());
        LogGIT(logBuf);

        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
        {
            sprintf_s(logBuf, "Seek: GetStreamProxy failed hr=0x%08X", hr);
            LogGIT(logBuf);
            return hr;
        }

        LogGIT("Seek: calling pStream->Seek...");
        hr = pStream->Seek(dlibMove, dwOrigin, plibNewPosition);
        sprintf_s(logBuf, "Seek: done hr=0x%08X, pos=%llu", hr, plibNewPosition ? plibNewPosition->QuadPart : 0);
        LogGIT(logBuf);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP SetSize(ULARGE_INTEGER libNewSize) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->SetSize(libNewSize);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP CopyTo(IStream* pstm, ULARGE_INTEGER cb, ULARGE_INTEGER* pcbRead, ULARGE_INTEGER* pcbWritten) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->CopyTo(pstm, cb, pcbRead, pcbWritten);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP Commit(DWORD grfCommitFlags) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->Commit(grfCommitFlags);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP Revert() override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->Revert();
        pStream->Release();
        return hr;
    }

    STDMETHODIMP LockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->LockRegion(libOffset, cb, dwLockType);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP UnlockRegion(ULARGE_INTEGER libOffset, ULARGE_INTEGER cb, DWORD dwLockType) override
    {
        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
            return hr;

        hr = pStream->UnlockRegion(libOffset, cb, dwLockType);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP Stat(STATSTG* pstatstg, DWORD grfStatFlag) override
    {
        char logBuf[128];
        sprintf_s(logBuf, "Stat: thread=%lu", GetCurrentThreadId());
        LogGIT(logBuf);

        IStream* pStream = nullptr;
        HRESULT hr = GetStreamProxy(&pStream);
        if (FAILED(hr))
        {
            sprintf_s(logBuf, "Stat: GetStreamProxy failed hr=0x%08X", hr);
            LogGIT(logBuf);
            return hr;
        }

        LogGIT("Stat: calling pStream->Stat...");
        hr = pStream->Stat(pstatstg, grfStatFlag);
        sprintf_s(logBuf, "Stat: done hr=0x%08X", hr);
        LogGIT(logBuf);
        pStream->Release();
        return hr;
    }

    STDMETHODIMP Clone(IStream** ppstm) override
    {
        // Clone is complex - the cloned stream would also need to be wrapped
        // For now, return E_NOTIMPL as OpenVDS doesn't use Clone
        return E_NOTIMPL;
    }

private:
    GITStreamWrapper(IGlobalInterfaceTable* pGIT, DWORD cookie)
        : m_pGIT(pGIT)
        , m_cookie(cookie)
        , m_refCount(1)
    {
    }

    ~GITStreamWrapper()
    {
        if (m_pGIT)
        {
            m_pGIT->RevokeInterfaceFromGlobal(m_cookie);
            m_pGIT->Release();
            m_pGIT = nullptr;
        }
    }

    // Get a thread-local proxy to the original stream from the GIT
    HRESULT GetStreamProxy(IStream** ppStream)
    {
        if (!ppStream)
            return E_INVALIDARG;

        *ppStream = nullptr;

        if (!m_pGIT)
            return E_UNEXPECTED;

        return m_pGIT->GetInterfaceFromGlobal(m_cookie, IID_IStream, reinterpret_cast<void**>(ppStream));
    }

    IGlobalInterfaceTable* m_pGIT;
    DWORD m_cookie;
    LONG m_refCount;
};
