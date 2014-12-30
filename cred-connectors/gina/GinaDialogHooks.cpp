/**
 * Copyright Flexible Software Solutions S.L. 2014
 **/

#include "FlexVDIProto.h"
#include "GinaDialogHooks.hpp"
#include "util.hpp"

using namespace flexvm;

template <typename T>
class WinlogonProxy : public GinaDialogHooks::BaseWinlogonProxy {
public:
    WinlogonProxy(PVOID winlogonFunctions) {
        functions = (T)winlogonFunctions;
        origWlxDialogBoxParam = functions->WlxDialogBoxParam;
        functions->WlxDialogBoxParam = getWlxDialogBoxParamFunc();
    }

    virtual int WINAPI WlxDialogBoxParam(HANDLE hWlx, HANDLE hInst,
                                         LPWSTR lpszTemplate, HWND hwndOwner,
                                         DLGPROC dlgprc, LPARAM dwInitParam) {
        return origWlxDialogBoxParam(hWlx, hInst, lpszTemplate,
                                     hwndOwner, dlgprc, dwInitParam);
    }

    virtual void sendCtrlAltDel(HANDLE hWlx) {
        functions->WlxSasNotify(hWlx, WLX_SAS_TYPE_CTRL_ALT_DEL);
    }

private:
    T functions;
    PWLX_DIALOG_BOX_PARAM origWlxDialogBoxParam;
};


void GinaDialogHooks::hookWinlogonFunctions(PVOID pWinlogonFunctions, DWORD dwWlxVersion, HANDLE a_hWlx) {
    hWlx = a_hWlx;
    wlxVersion = dwWlxVersion;
    switch (dwWlxVersion) {
        case WLX_VERSION_1_0:
            winlogon.reset(new WinlogonProxy<PWLX_DISPATCH_VERSION_1_0>(pWinlogonFunctions));
            break;
        case WLX_VERSION_1_1:
            winlogon.reset(new WinlogonProxy<PWLX_DISPATCH_VERSION_1_1>(pWinlogonFunctions));
            break;
        case WLX_VERSION_1_2:
            winlogon.reset(new WinlogonProxy<PWLX_DISPATCH_VERSION_1_2>(pWinlogonFunctions));
            break;
        case WLX_VERSION_1_3:
            winlogon.reset(new WinlogonProxy<PWLX_DISPATCH_VERSION_1_3>(pWinlogonFunctions));
            break;
        case WLX_VERSION_1_4:
        default:
            winlogon.reset(new WinlogonProxy<PWLX_DISPATCH_VERSION_1_4>(pWinlogonFunctions));
            break;
    }
}


int WINAPI GinaDialogHooks::WlxDialogBoxParam(HANDLE hWlx, HANDLE hInst, LPWSTR lpszTemplate,
                                  HWND hwndOwner, DLGPROC dlgprc, LPARAM dwInitParam) {
    GinaDialogHooks & s = GinaDialogHooks::singleton();
    s.currentDlgProc = dlgprc;
    int res = s.winlogon->WlxDialogBoxParam(hWlx, hInst, lpszTemplate,
                                            hwndOwner, PassDlgProc, dwInitParam);
    s.thread.stop();
    Log(L_DEBUG) << "<--WlxDialogBoxParam (" << LOWORD(lpszTemplate) << ")";
    return res;
}


INT_PTR CALLBACK GinaDialogHooks::PassDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    GinaDialogHooks & s = GinaDialogHooks::singleton();
    s.creds.setCurrentDialog(hwndDlg);
    BOOL result = s.LogonDlgProc(hwndDlg, uMsg, wParam, lParam);
    if (uMsg == WM_INITDIALOG && s.passwordIDC == 0) {
        // This may be the annoying dialog askig for Ctrl+Alt+Del
        // This is a bit useless, because it can just be disabled.
        s.sendCtrlAltDel();
    }
    return result;
}


BOOL GinaDialogHooks::LogonDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    if (uMsg == WM_INITDIALOG) {
        findCredentialControls(hwndDlg);
        if (passwordIDC != 0)
            thread.start();
    }

    if (uMsg == WM_USER + 5) {
        std::string username, password, domain;
        creds.getCredentials(username, password, domain);
        SetDlgItemTextA(hwndDlg, usernameIDC, username.c_str());
        SetDlgItemTextA(hwndDlg, passwordIDC, password.c_str());
        SetDlgItemTextA(hwndDlg, domainIDC, domain.c_str());
        uMsg = WM_COMMAND;
        wParam = IDOK;
        lParam = 0;
    }

    return currentDlgProc(hwndDlg, uMsg, wParam, lParam);
}


void GinaDialogHooks::findCredentialControls(HWND hwndDlg) {
    // Assume there are two edit controls, one of them with password style,
    // and a combobox for the domain
    HWND hwnd = NULL;
    passwordIDC = usernameIDC = domainIDC = 0;
    for (int i = 0; i < 2; ++i) {
        WINDOWINFO info;
        hwnd = FindWindowExA(hwndDlg, hwnd, "EDIT", NULL);
        if (hwnd && GetWindowInfo(hwnd, &info) && (info.dwStyle & ES_PASSWORD)) {
            passwordIDC = GetDlgCtrlID(hwnd);
            Log(L_DEBUG) << "This is the password control: " << passwordIDC;
        } else if (hwnd) {
            usernameIDC = GetDlgCtrlID(hwnd);
        }
    }
    hwnd = FindWindowExA(hwndDlg, NULL, "COMBOBOX", NULL);
    if (hwnd) {
        domainIDC = GetDlgCtrlID(hwnd);
    }
}