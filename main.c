/*
 *  OpenVPN-GUI -- A Windows GUI for OpenVPN.
 *
 *  Copyright (C) 2004 Mathias Sundman <mathias@nilings.se>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#if !defined (UNICODE)
#error UNICODE and _UNICODE must be defined. This version only supports unicode builds.
#endif

#include <windows.h>
#include <versionhelpers.h>
#include <shlwapi.h>
#include <wtsapi32.h>
#include <prsht.h>
#include <commdlg.h>

#include "tray.h"
#include "openvpn.h"
#include "openvpn_config.h"
#include "viewlog.h"
#include "service.h"
#include "main.h"
#include "options.h"
#include "passphrase.h"
#include "proxy.h"
#include "registry.h"
#include "openvpn-gui-res.h"
#include "localization.h"
#include "manage.h"
#include "misc.h"
#include "save_pass.h"
#include "echo.h"
#include "as.h"

#ifndef DISABLE_CHANGE_PASSWORD
#include <openssl/crypto.h>
#endif

#define OVPN_EXITCODE_ERROR      1
#define OVPN_EXITCODE_TIMEOUT    2
#define OVPN_EXITCODE_NOTREADY   3

/*  Declare Windows procedure  */
LRESULT CALLBACK WindowProcedure (HWND, UINT, WPARAM, LPARAM);
static void ShowSettingsDialog();
void CloseApplication(HWND hwnd);
void ImportConfigFileFromDisk();
void ImportConfigFromAS();

/*  Class name and window title  */
TCHAR szClassName[ ] = _T("OpenVPN-GUI");
TCHAR szTitleText[ ] = _T("OpenVPN");

/* Options structure */
options_t o;

/* Workaround for ASLR on Windows */
__declspec(dllexport) char aslr_workaround;

static int
VerifyAutoConnections()
{
    int i;

    for (i = 0; i < o.num_auto_connect; i++)
    {
        if (GetConnByName(o.auto_connect[i]) == NULL)
        {
            /* autostart config not found */
            ShowLocalizedMsg(IDS_ERR_AUTOSTART_CONF, o.auto_connect[i]);
            return FALSE;
        }
    }

    return TRUE;
}

/*
 * Send a copydata message corresponding to any --command action option specified
 * to the running instance and return success or error.
 */
static int
NotifyRunningInstance()
{
    /* Check if a previous instance has a window initialized
     * Even if we are not the first instance this may return null
     * if the previous instance has not fully started up
     */
    HANDLE hwnd_master = FindWindow (szClassName, NULL);
    int exit_code = 0;
    if (hwnd_master)
    {
        /* GUI up and running -- send a message if any action is pecified,
           else show the balloon */
        COPYDATASTRUCT config_data = {0};
        int timeout = 30*1000;             /* 30 seconds */
        if (!o.action)
        {
            o.action = WM_OVPN_NOTIFY;
            o.action_arg = LoadLocalizedString(IDS_NFO_CLICK_HERE_TO_START);
        }
        config_data.dwData = o.action;
        if (o.action_arg)
        {
            config_data.cbData = (wcslen(o.action_arg)+1)*sizeof(o.action_arg[0]);
            config_data.lpData = (void *) o.action_arg;
        }
        PrintDebug(L"Instance 2: called with action %d : %ls", o.action, o.action_arg);
        if (!SendMessageTimeout (hwnd_master, WM_COPYDATA, 0,
                                 (LPARAM) &config_data, 0, timeout, NULL))
        {
            DWORD error = GetLastError();
            if (error == ERROR_TIMEOUT)
            {
                exit_code = OVPN_EXITCODE_TIMEOUT;
                MsgToEventLog(EVENTLOG_ERROR_TYPE, L"Sending command to running instance timed out.");
            }
            else
            {
                exit_code = OVPN_EXITCODE_ERROR;
                MsgToEventLog(EVENTLOG_ERROR_TYPE, L"Sending command to running instance failed (error = %lu).",
                              error);
            }
        }
    }
    else
    {
        /* An instance is already running but its main window not yet initialized */
        exit_code = OVPN_EXITCODE_NOTREADY;
        MsgToEventLog(EVENTLOG_ERROR_TYPE, L"Previous instance not yet ready to accept commands. "
                      "Try again later.");
    }

    return exit_code;
}

int WINAPI _tWinMain (HINSTANCE hThisInstance,
                    UNUSED HINSTANCE hPrevInstance,
                    UNUSED LPTSTR lpszArgument,
                    UNUSED int nCmdShow)
{
  MSG messages;            /* Here messages to the application are saved */
  WNDCLASSEX wincl;        /* Data structure for the windowclass */
  DWORD shell32_version;
  BOOL first_instance = TRUE;
  /* a session local semaphore to detect second instance */
  HANDLE session_semaphore = InitSemaphore(L"Local\\"PACKAGE_NAME);

  srand(time(NULL));
  /* try to lock the semaphore, else we are not the first instance */
  if (session_semaphore &&
      WaitForSingleObject(session_semaphore, 200) != WAIT_OBJECT_0)
  {
      first_instance = FALSE;
  }

  /* Initialize handlers for manangement interface notifications */
  mgmt_rtmsg_handler handler[] = {
      { ready_,    OnReady },
      { hold_,     OnHold },
      { log_,      OnLogLine },
      { state_,    OnStateChange },
      { password_, OnPassword },
      { proxy_,    OnProxy },
      { stop_,     OnStop },
      { needok_,   OnNeedOk },
      { needstr_,  OnNeedStr },
      { echo_,     OnEcho },
      { bytecount_,OnByteCount },
      { infomsg_,  OnInfoMsg },
      { timeout_,  OnTimeout },
      { 0,        NULL }
  };
  InitManagement(handler);

  /* initialize options to default state */
  InitOptions(&o);
  if (first_instance)
      o.session_semaphore = session_semaphore;

#ifdef DEBUG
  /* Open debug file for output */
  if (!(o.debug_fp = _wfopen(DEBUG_FILE, L"a+,ccs=UTF-8")))
    {
      /* can't open debug file */
      ShowLocalizedMsg(IDS_ERR_OPEN_DEBUG_FILE, DEBUG_FILE);
      exit(1);
    }
  PrintDebug(_T("Starting OpenVPN GUI v%hs"), PACKAGE_VERSION);
#endif


  o.hInstance = hThisInstance;

  if(!GetModuleHandle(_T("RICHED20.DLL")))
    {
      LoadLibrary(_T("RICHED20.DLL"));
    }
  else
    {
      /* can't load riched20.dll */
      ShowLocalizedMsg(IDS_ERR_LOAD_RICHED20);
      exit(1);
    }

  /* Check version of shell32.dll */
  shell32_version=GetDllVersion(_T("shell32.dll"));
  if (shell32_version < PACKVERSION(5,0))
    {
      /* shell32.dll version to low */
      ShowLocalizedMsg(IDS_ERR_SHELL_DLL_VERSION, shell32_version);
      exit(1);
    }
#ifdef DEBUG
  PrintDebug(_T("Shell32.dll version: 0x%lx"), shell32_version);
#endif

  if (first_instance)
      UpdateRegistry(); /* Checks version change and update keys/values */

  GetRegistryKeys();
  /* Parse command-line options */
  ProcessCommandLine(&o, GetCommandLine());

  EnsureDirExists(o.config_dir);

  if (!first_instance)
  {
      int res = NotifyRunningInstance();
      exit(res);
  }
  else if (o.action == WM_OVPN_START)
  {
      PrintDebug(L"Instance 1: Called with --command connect xxx. Treating it as --connect xxx");
  }
  else if (o.action == WM_OVPN_IMPORT)
  {
     ; /* pass -- import is handled after Window initialization */
  }
  else if (o.action)
  {
      MsgToEventLog(EVENTLOG_ERROR_TYPE, L"Called with --command when no previous instance available");
      exit(OVPN_EXITCODE_ERROR);
  }

  if (!CheckVersion()) {
    exit(1);
  }

  if (!EnsureDirExists(o.log_dir))
  {
    ShowLocalizedMsg(IDS_ERR_CREATE_PATH, _T("log_dir"), o.log_dir);
    exit(1);
  }

  BOOL use_iservice = (o.iservice_admin && IsWindows7OrGreater()) || !IsUserAdmin();
  if (use_iservice && strtod(o.ovpn_version, NULL) > 2.3 && !o.silent_connection)
    CheckIServiceStatus(TRUE);

  CheckServiceStatus();	/* Check if automatic service is running or not */
  BuildFileList();

  if (!VerifyAutoConnections()) {
    exit(1);
  }

  GetProxyRegistrySettings();

#ifndef DISABLE_CHANGE_PASSWORD
  /* Initialize OpenSSL */
  set_openssl_env_vars();
  OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CONFIG, NULL);
#endif /* DISABLE_CHANGE_PASSWORD */

  /* The Window structure */
  wincl.hInstance = hThisInstance;
  wincl.lpszClassName = szClassName;
  wincl.lpfnWndProc = WindowProcedure;      /* This function is called by windows */
  wincl.style = CS_DBLCLKS;                 /* Catch double-clicks */
  wincl.cbSize = sizeof (WNDCLASSEX);

  /* Use default icon and mouse-pointer */
  wincl.hIcon = LoadLocalizedIcon(ID_ICO_APP);
  wincl.hIconSm = LoadLocalizedIcon(ID_ICO_APP);
  wincl.hCursor = LoadCursor (NULL, IDC_ARROW);
  wincl.lpszMenuName = NULL;                 /* No menu */
  wincl.cbClsExtra = 0;                      /* No extra bytes after the window class */
  wincl.cbWndExtra = 0;                      /* structure or the window instance */
  /* Use Windows's default color as the background of the window */
  wincl.hbrBackground = (HBRUSH) COLOR_3DSHADOW; //COLOR_BACKGROUND;

  /* Register the window class, and if it fails quit the program */
  if (!RegisterClassEx (&wincl))
    return 1;

  /* The class is registered, let's create the program*/
  CreateWindowEx (
           0,                   /* Extended possibilites for variation */
           szClassName,         /* Classname */
           szTitleText,         /* Title Text */
           WS_OVERLAPPEDWINDOW, /* default window */
           (int)CW_USEDEFAULT,  /* Windows decides the position */
           (int)CW_USEDEFAULT,  /* where the window ends up on the screen */
           230,                 /* The programs width */
           200,                 /* and height in pixels */
           HWND_DESKTOP,        /* The window is a child-window to desktop */
           NULL,                /* No menu */
           hThisInstance,       /* Program Instance handler */
           NULL                 /* No Window Creation data */
           );


  /* Run the message loop. It will run until GetMessage() returns 0 */
  while (GetMessage (&messages, NULL, 0, 0))
  {
    TranslateMessage(&messages);
    DispatchMessage(&messages);
  }

  CloseSemaphore(o.session_semaphore);
  o.session_semaphore = NULL;   /* though we're going to die.. */
  if (o.event_log)
      DeregisterEventSource(o.event_log);

  /* The program return-value is 0 - The value that PostQuitMessage() gave */
  return messages.wParam;
}


static void
StopAllOpenVPN()
{
    int i;

    /* Stop all connections started by us -- we leave persistent ones
     * at their current state. Use the disconnect menu to put them into
     * hold state before exit, if desired.
     */
    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].state != disconnected && o.conn[i].state != detached)
        {
            if (o.conn[i].flags & FLAG_DAEMON_PERSISTENT)
            {
                DetachOpenVPN(&o.conn[i]);
            }
            else
            {
                StopOpenVPN(&o.conn[i]);
            }
        }
    }

    /* Wait for all connections to terminate (Max 5 sec) */
    for (i = 0; i < 20; i++, Sleep(250))
    {
        if (CountConnState(disconnected) + CountConnState(detached) == o.num_configs)
            break;
    }
}


static int
AutoStartConnections()
{
    int i;

    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].auto_connect)
            StartOpenVPN(&o.conn[i]);
    }

    return TRUE;
}


static void
ResumeConnections()
{
    int i;
    for (i = 0; i < o.num_configs; i++) {
        /* Restart suspend connections */
        if (o.conn[i].state == suspended)
            StartOpenVPN(&o.conn[i]);

        /* If some connection never reached SUSPENDED state */
        if (o.conn[i].state == suspending)
            StopOpenVPN(&o.conn[i]);
    }
}

/*
 * Get dpi of the system and set the scale factor.
 * The system dpi may be different from the per monitor dpi on
 * Win 8.1 later. We set dpi awareness to system-dpi level in the
 * manifest, and let Windows automatically re-scale windows
 * if/when dpi changes dynamically.
 */
static void
dpi_initialize(void)
{
    UINT dpix = 0;
    HDC hdc = GetDC(NULL);

    if (hdc)
    {
        dpix = GetDeviceCaps(hdc, LOGPIXELSX);
        ReleaseDC(NULL, hdc);
        PrintDebug(L"System DPI: dpix = %u", dpix);
    }
    else
    {
        PrintDebug(L"GetDC failed, using default dpi = 96 (error = %lu)", GetLastError());
        dpix = 96;
    }

    DpiSetScale(&o, dpix);
}

static int
HandleCopyDataMessage(const COPYDATASTRUCT *copy_data)
{
    WCHAR *str = NULL;
    connection_t *c = NULL;
    PrintDebug (L"WM_COPYDATA message received. (dwData: %lu, cbData: %lu, lpData: %ls)",
                copy_data->dwData, copy_data->cbData, copy_data->lpData);
    if (copy_data->cbData >= sizeof(WCHAR) && copy_data->lpData)
    {
        str = (WCHAR*) copy_data->lpData;
        str[copy_data->cbData/sizeof(WCHAR)-1] = L'\0'; /* in case not nul-terminated */
        c = GetConnByName(str);
    }
    if(copy_data->dwData == WM_OVPN_START && c)
    {
        if (!o.silent_connection)
            ForceForegroundWindow(o.hWnd);
        StartOpenVPN(c);
    }
    else if(copy_data->dwData == WM_OVPN_STOP && c)
        StopOpenVPN(c);
    else if(copy_data->dwData == WM_OVPN_RESTART && c)
    {
        if (!o.silent_connection)
            ForceForegroundWindow(o.hWnd);
        RestartOpenVPN(c);
    }
    else if(copy_data->dwData == WM_OVPN_SHOWSTATUS && c && c->hwndStatus)
    {
        ForceForegroundWindow(o.hWnd);
        ShowWindow(c->hwndStatus, SW_SHOW);
    }
    else if(copy_data->dwData == WM_OVPN_STOPALL)
        StopAllOpenVPN();
    else if(copy_data->dwData == WM_OVPN_SILENT && str)
    {
        if (_wtoi(str) == 0)
            o.silent_connection = 0;
        else
            o.silent_connection = 1;
    }
    else if(copy_data->dwData == WM_OVPN_EXIT)
    {
        CloseApplication(o.hWnd);
    }
    else if(copy_data->dwData == WM_OVPN_IMPORT && str)
    {
        ImportConfigFile(str, true); /* prompt user */
    }
    else if (copy_data->dwData == WM_OVPN_NOTIFY)
    {
        ShowTrayBalloon(L"", copy_data->lpData);
    }
    else if (copy_data->dwData == WM_OVPN_RESCAN)
    {
        OnNotifyTray(WM_OVPN_RESCAN);
    }
    else
    {
        MsgToEventLog(EVENTLOG_ERROR_TYPE,
                      L"Unknown WM_COPYDATA message ignored. (dwData: %lu, cbData: %lu, lpData: %ls)",
                      copy_data->dwData, copy_data->cbData, copy_data->lpData);
        return FALSE;
    }
    return TRUE; /* indicate we handled the message */
}

/* If automatic service is running, check whether we are
 * attached to the management i/f of persistent daemons
 * and re-attach if necessary. The timer is reset to
 * call this routine again after a delay. Periodic check
 * is required as we get detached if the daemon gets restarted
 * by the service or we do a disconnect.
 */
static void CALLBACK
ManagePersistent(HWND hwnd, UINT UNUSED msg, UINT_PTR id, DWORD UNUSED now)
{
    CheckServiceStatus(false);
    if (o.service_state == service_connected)
    {
        for (int i = 0; i < o.num_configs; i++)
        {
            if (o.conn[i].flags & FLAG_DAEMON_PERSISTENT
                && o.conn[i].auto_connect
                && (o.conn[i].state == disconnected || o.conn[i].state == detached))
            {
                /* disable auto-connect to avoid repeated re-connect
                 * after unrecoverable errors. Re-enabled on successful
                 * connect.
                 */
                o.conn[i].auto_connect = false;
                StartOpenVPN(&o.conn[i]); /* attach to the management i/f */
            }
        }
    }
    /* schedule to call again after 10 sec */
    SetTimer(hwnd, id, 10000, ManagePersistent);
}

/*  This function is called by the Windows function DispatchMessage()  */
LRESULT CALLBACK WindowProcedure (HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
  static UINT s_uTaskbarRestart;
  int conn_id = 0;
  MENUINFO minfo = {.cbSize = sizeof(MENUINFO)};

  switch (message) {
    case WM_CREATE:

      /* Save Window Handle */
      o.hWnd = hwnd;
      dpi_initialize();

      s_uTaskbarRestart = RegisterWindowMessage(TEXT("TaskbarCreated"));

      WTSRegisterSessionNotification(hwnd, NOTIFY_FOR_THIS_SESSION);

      /* Load application icon */
      HICON hIcon = LoadLocalizedIcon(ID_ICO_APP);
      if (hIcon) {
        SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_SMALL), (LPARAM) (hIcon));
        SendMessage(hwnd, WM_SETICON, (WPARAM) (ICON_BIG), (LPARAM) (hIcon));
      }

      /* Enable next line to accept WM_COPYDATA messages from lower level processes */
#if 0
      ChangeWindowMessageFilterEx(hwnd, WM_COPYDATA, MSGFLT_ALLOW, NULL);
#endif

      echo_msg_init();

      CreatePopupMenus();	/* Create popup menus */
      ShowTrayIcon();

      /* if '--import' was specified, do it now */
      if (o.action == WM_OVPN_IMPORT && o.action_arg)
      {
        ImportConfigFile(o.action_arg, true); /* prompt user */
      }

      if (!AutoStartConnections()) {
        SendMessage(hwnd, WM_CLOSE, 0, 0);
        break;
      }
      /* A timer to periodically tend to persistent connections */
      SetTimer(hwnd, 0, 10000, ManagePersistent);

      break;

    case WM_NOTIFYICONTRAY:
      OnNotifyTray(lParam); 	// Manages message from tray
      break;

    case WM_COPYDATA:   // custom messages with data from other processes
      HandleCopyDataMessage((COPYDATASTRUCT*) lParam);
      return TRUE; /* lets the sender free copy_data */

    case WM_MENUCOMMAND:
      /* Get the menu item id and save it in wParam for use below */
      wParam = GetMenuItemID((HMENU) lParam, wParam);

      /* we first check global menu items which do not require a connnection index */
      if (LOWORD(wParam) == IDM_IMPORT_FILE) {
        ImportConfigFileFromDisk();
      }
      else if (LOWORD(wParam) == IDM_IMPORT_AS) {
        ImportConfigFromAS();
      }
      else if (LOWORD(wParam) == IDM_IMPORT_URL) {
        ImportConfigFromURL();
      }
      else if (LOWORD(wParam) == IDM_SETTINGS) {
        ShowSettingsDialog();
      }
      else if (LOWORD(wParam) == IDM_CLOSE) {
        CloseApplication(hwnd);
      }
      /* rest of the handlers require a connection id */
      else {
        minfo.fMask = MIM_MENUDATA;
        GetMenuInfo((HMENU) lParam, &minfo);
        conn_id = (INT) minfo.dwMenuData;
        if (conn_id < 0 || conn_id >= o.num_configs) break; /* ignore invalid connection id */
      }

      /* reach here only if the command did not match any global items and a valid connection id is available */

      if (LOWORD(wParam) == IDM_CONNECTMENU) {
        StartOpenVPN(&o.conn[conn_id]);
      }
      else if (LOWORD(wParam) == IDM_DISCONNECTMENU) {
        StopOpenVPN(&o.conn[conn_id]);
      }
      else if (LOWORD(wParam) == IDM_RECONNECTMENU) {
        RestartOpenVPN(&o.conn[conn_id]);
      }
      else if (LOWORD(wParam) == IDM_STATUSMENU) {
        ShowWindow(o.conn[conn_id].hwndStatus, SW_SHOW);
      }
      else if (LOWORD(wParam) == IDM_VIEWLOGMENU) {
        ViewLog(conn_id);
      }
      else if (LOWORD(wParam) == IDM_EDITMENU) {
        EditConfig(conn_id);
      }
      else if (LOWORD(wParam) == IDM_CLEARPASSMENU) {
        ResetSavePasswords(&o.conn[conn_id]);
      }
#ifndef DISABLE_CHANGE_PASSWORD
      else if (LOWORD(wParam) == IDM_PASSPHRASEMENU) {
        ShowChangePassphraseDialog(&o.conn[conn_id]);
      }
#endif
      break;

    case WM_CLOSE:
      CloseApplication(hwnd);
      break;

    case WM_DESTROY:
      WTSUnRegisterSessionNotification(hwnd);
      StopAllOpenVPN();
      OnDestroyTray();          /* Remove Tray Icon and destroy menus */
      PostQuitMessage (0);	/* Send a WM_QUIT to the message queue */
      break;

    case WM_QUERYENDSESSION:
      return(TRUE);

    case WM_ENDSESSION:
      StopAllOpenVPN();
      OnDestroyTray();
      break;

    case WM_WTSSESSION_CHANGE:
      switch (wParam) {
        case WTS_SESSION_LOCK:
          o.session_locked = TRUE;
          break;
        case WTS_SESSION_UNLOCK:
          o.session_locked = FALSE;
          if (CountConnState(suspended) != 0)
            ResumeConnections();
          break;
      }
      break;

    default:			/* for messages that we don't deal with */
      if (message == s_uTaskbarRestart)
        {
          /* Explorer has restarted, re-register the tray icon. */
          ShowTrayIcon();
          CheckAndSetTrayIcon();
          break;
        }
      return DefWindowProc (hwnd, message, wParam, lParam);
  }

  return 0;
}


static INT_PTR CALLBACK
AboutDialogFunc(UNUSED HWND hDlg, UINT msg, UNUSED WPARAM wParam, LPARAM lParam)
{
  LPPSHNOTIFY psn;
  wchar_t tmp1[256], tmp2[256];
  switch (msg) {
    case WM_INITDIALOG:
      if (GetDlgItemText(hDlg, ID_TXT_VERSION, tmp1, _countof(tmp1))) {
        _sntprintf_0(tmp2, tmp1, TEXT(PACKAGE_VERSION_RESOURCE_STR));
        SetDlgItemText(hDlg, ID_TXT_VERSION, tmp2);
      }
      break;
    case WM_NOTIFY:
      psn = (LPPSHNOTIFY) lParam;
      if (psn->hdr.code == (UINT) PSN_APPLY)
        return TRUE;
  }
  return FALSE;
}


static void
ShowSettingsDialog()
{
  PROPSHEETPAGE psp[4];
  int page_number = 0;

  /* General tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_GENERAL);
  psp[page_number].pfnDlgProc = GeneralSettingsDlgProc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  /* Proxy tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_PROXY);
  psp[page_number].pfnDlgProc = ProxySettingsDialogFunc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  /* Advanced tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_ADVANCED);
  psp[page_number].pfnDlgProc = AdvancedSettingsDlgProc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  /* About tab */
  psp[page_number].dwSize = sizeof(PROPSHEETPAGE);
  psp[page_number].dwFlags = PSP_DLGINDIRECT;
  psp[page_number].hInstance = o.hInstance;
  psp[page_number].pResource = LocalizedDialogResource(ID_DLG_ABOUT);
  psp[page_number].pfnDlgProc = AboutDialogFunc;
  psp[page_number].lParam = 0;
  psp[page_number].pfnCallback = NULL;
  ++page_number;

  PROPSHEETHEADER psh;
  psh.dwSize = sizeof(PROPSHEETHEADER);
  psh.dwFlags = PSH_USEHICON | PSH_PROPSHEETPAGE | PSH_NOAPPLYNOW | PSH_NOCONTEXTHELP;
  psh.hwndParent = o.hWnd;
  psh.hInstance = o.hInstance;
  psh.hIcon = LoadLocalizedIcon(ID_ICO_APP);
  psh.pszCaption = LoadLocalizedString(IDS_SETTINGS_CAPTION);
  psh.nPages = page_number;
  psh.nStartPage = 0;
  psh.ppsp = (LPCPROPSHEETPAGE) &psp;
  psh.pfnCallback = NULL;

  PropertySheet(&psh);
}


void
CloseApplication(HWND hwnd)
{
    int i;

    /* Show a message if any non-persistent connections are active */
    for (i = 0; i < o.num_configs; i++)
    {
        if (o.conn[i].state == disconnected
            || o.conn[i].flags & FLAG_DAEMON_PERSISTENT)
        {
            continue;
        }

        /* Ask for confirmation if still connected */
        if (ShowLocalizedMsgEx(MB_YESNO, NULL, _T("Exit OpenVPN"), IDS_NFO_ACTIVE_CONN_EXIT) == IDNO)
        {
            return;
        }
        break; /* show the above message box only once */
    }

    DestroyWindow(hwnd);
}

void
ImportConfigFileFromDisk()
{
    TCHAR filter[2*_countof(o.ext_string)+5];

    _sntprintf_0(filter, _T("*.%ls%lc*.%ls%lc"), o.ext_string, _T('\0'), o.ext_string, _T('\0'));

    OPENFILENAME fn;
    TCHAR source[MAX_PATH] = _T("");

    fn.lStructSize = sizeof(OPENFILENAME);
    fn.hwndOwner = NULL;
    fn.lpstrFilter = filter;
    fn.lpstrCustomFilter = NULL;
    fn.nFilterIndex = 1;
    fn.lpstrFile = source;
    fn.nMaxFile = MAX_PATH;
    fn.lpstrFileTitle = NULL;
    fn.lpstrInitialDir = NULL;
    fn.lpstrTitle = NULL;
    fn.Flags = OFN_DONTADDTORECENT | OFN_FILEMUSTEXIST;
    fn.lpstrDefExt = NULL;

    if (GetOpenFileName(&fn))
    {
        ImportConfigFile(source, false); /* do not prompt user */
    }
}

#ifdef DEBUG
void PrintDebugMsg(TCHAR *msg)
{
  time_t log_time;
  struct tm *time_struct;
  TCHAR date[30];

  log_time = time(NULL);
  time_struct = localtime(&log_time);
  _sntprintf(date, _countof(date), _T("%d-%.2d-%.2d %.2d:%.2d:%.2d"),
                 time_struct->tm_year + 1900,
                 time_struct->tm_mon + 1,
                 time_struct->tm_mday,
                 time_struct->tm_hour,
                 time_struct->tm_min,
                 time_struct->tm_sec);

  _ftprintf(o.debug_fp, _T("%ls %ls\n"), date, msg);
  fflush(o.debug_fp);
}
#endif

#define PACKVERSION(major,minor) MAKELONG(minor,major)
DWORD GetDllVersion(LPCTSTR lpszDllName)
{
    HINSTANCE hinstDll;
    DWORD dwVersion = 0;

    /* For security purposes, LoadLibrary should be provided with a
       fully-qualified path to the DLL. The lpszDllName variable should be
       tested to ensure that it is a fully qualified path before it is used. */
    hinstDll = LoadLibrary(lpszDllName);

    if(hinstDll)
    {
        DLLGETVERSIONPROC pDllGetVersion;
        pDllGetVersion = (DLLGETVERSIONPROC)GetProcAddress(hinstDll,
                          "DllGetVersion");

        /* Because some DLLs might not implement this function, you
        must test for it explicitly. Depending on the particular
        DLL, the lack of a DllGetVersion function can be a useful
        indicator of the version. */

        if(pDllGetVersion)
        {
            DLLVERSIONINFO dvi;
            HRESULT hr;

            ZeroMemory(&dvi, sizeof(dvi));
            dvi.cbSize = sizeof(dvi);

            hr = (*pDllGetVersion)(&dvi);

            if(SUCCEEDED(hr))
            {
               dwVersion = PACKVERSION(dvi.dwMajorVersion, dvi.dwMinorVersion);
            }
        }

        FreeLibrary(hinstDll);
    }
    return dwVersion;
}

void
MsgToEventLog(WORD type, wchar_t *format, ...)
{
    const wchar_t *msg[2];
    wchar_t buf[256];
    int size = _countof(buf);

    if (!o.event_log)
    {
        o.event_log = RegisterEventSource(NULL, TEXT(PACKAGE_NAME));
        if (!o.event_log)
            return;
    }

    va_list args;
    va_start(args, format);
    int nchar = vswprintf(buf, size-1, format, args);
    va_end(args);

    if (nchar == -1) return;

    buf[size - 1] = '\0';

    msg[0] = TEXT(PACKAGE_NAME);
    msg[1] = buf;
    ReportEventW(o.event_log, type, 0, 0, NULL, 2, 0, msg, NULL);
}

void
ErrorExit(int exit_code, const wchar_t *msg)
{
    if (msg)
        MessageBoxExW(NULL, msg, TEXT(PACKAGE_NAME),
                      MB_OK | MB_SETFOREGROUND|MB_ICONERROR, GetGUILanguage());
    if (o.hWnd)
    {
        StopAllOpenVPN();
        PostQuitMessage(exit_code);
    }
    else
    {
        exit(exit_code);
    }
}
