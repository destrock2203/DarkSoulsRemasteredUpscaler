#include "upscaler/signature.h"

#include "core/config.h"
#include "core/log.h"
#include "core/paths.h"

#include <Windows.h>
#include <SoftPub.h>
#include <wintrust.h>

#include <cwchar>

namespace dsru::upscaler {

bool VerifySignature(const std::filesystem::path& file, const wchar_t* signer) {
    WINTRUST_FILE_INFO fileInfo{};
    fileInfo.cbStruct = sizeof(fileInfo);
    fileInfo.pcwszFilePath = file.c_str();

    WINTRUST_DATA data{};
    data.cbStruct = sizeof(data);
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;  // no network access at game start
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &fileInfo;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    data.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;

    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG status = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    // The signing certificate's name, while the verification state is still open.
    wchar_t signerName[256] = L"";
    if (status == ERROR_SUCCESS && signer) {
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* sgnr = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
                sgnr && sgnr->csCertChain && sgnr->pasCertChain)
                CertGetNameStringW(sgnr->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr,
                                   signerName, static_cast<DWORD>(std::size(signerName)));
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    if (status != ERROR_SUCCESS) {
        log::Warn("Signature check failed for '{}' (0x{:08X})", file.string(), static_cast<unsigned long>(status));
        return false;
    }
    if (signer && !std::wcsstr(signerName, signer)) {
        std::string name;
        for (const wchar_t* c = signerName; *c; ++c) name.push_back(*c < 0x80 ? static_cast<char>(*c) : '?');
        log::Warn("'{}' is signed by '{}', not the expected publisher", file.string(), name);
        return false;
    }
    return true;
}

void* LoadSignedModule(const char* feature, const wchar_t* fileName) {
    const std::filesystem::path path = paths::ModuleDirectory() / "DSRUpscaler" / fileName;
    if (!std::filesystem::exists(path)) {
        log::Warn("{}: '{}' not found - unavailable", feature, path.string());
        return nullptr;
    }
    if (!VerifySignature(path)) {
        if (!config::Get().allowUnsignedDlls) {
            log::Error("{}: '{}' is not validly signed - not loading it (AllowUnsignedDlls=0)", feature,
                       path.string());
            return nullptr;
        }
        log::Warn("{}: '{}' is not validly signed - loading it anyway (AllowUnsignedDlls=1)", feature, path.string());
    }
    HMODULE module = LoadLibraryW(path.c_str());
    if (!module) log::Error("{}: failed to load '{}' (error {})", feature, path.string(), GetLastError());
    return module;
}

}  // namespace dsru::upscaler
