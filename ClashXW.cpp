/*
 * Copyright (C) 2020 Richard Yu <yurichard3839@gmail.com>
 *
 * This file is part of ClashXW.
 *
 * ClashXW is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * ClashXW is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with ClashXW.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "pch.h"
#include "ClashXW.h"

LRESULT CALLBACK    WndProc(HWND, UINT, WPARAM, LPARAM);
INT_PTR CALLBACK    About(HWND, UINT, WPARAM, LPARAM);

int APIENTRY wWinMain(_In_ HINSTANCE hInstance,
	_In_opt_ [[maybe_unused]] HINSTANCE hPrevInstance,
	_In_ LPWSTR    lpCmdLine,
	_In_ [[maybe_unused]] int       nCmdShow)
{
	g_hInst = hInstance;

	winrt::init_apartment(winrt::apartment_type::single_threaded);

	g_exePath = GetModuleFsPath(hInstance).remove_filename();
	SetCurrentDirectoryW(g_exePath.c_str());

	PortableModeUtil::CheckAndSetDataPath();

	yi18n::LoadTranslateDataFromResource(hInstance);

	if (!PortableModeUtil::CheckOnlyOneInstance())
	{
		constexpr std::wstring_view pmPrefix = L"--pm=";
		constexpr std::wstring_view urlPrefix = L"--url=";
		std::wstring_view cmdLine(lpCmdLine);
		if (cmdLine.starts_with(pmPrefix))
			return ProcessManager::SubProcess(cmdLine.substr(pmPrefix.size()));
		else if (cmdLine.starts_with(urlPrefix))
			return URLProtocolUtil::HandleURL(cmdLine.substr(urlPrefix.size()));
		return EXIT_FAILURE;
	}

	PortableModeUtil::AskForPortableMode();
	CreateDirectoryIgnoreExist(g_dataPath.c_str());
	PortableModeUtil::CheckOnlyOneInstance();
	PortableModeUtil::SetAppId();

	try
	{
		URLProtocolUtil::CreateURLProtocols();
	}
	CATCH_LOG();

	InitDarkMode();
	InitDPIAPI();
	LoadSettings();

	WNDCLASSEXW wcex = {
		.cbSize = sizeof(wcex),
		.lpfnWndProc = WndProc,
		.hInstance = hInstance,
		.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CLASHXW)),
		.hCursor = LoadCursorW(nullptr, IDC_ARROW),
		.lpszClassName = L"ClashXW",
		.hIconSm = LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_CLASHXW))
	};

	RegisterClassExW(&wcex);

	auto hWnd = CreateWindowExW(WS_EX_TOPMOST, L"ClashXW", g_portableMode ? nullptr : L"Global", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hInstance, nullptr);
	if (!hWnd)
		return EXIT_FAILURE;

	MSG msg;
	while (GetMessageW(&msg, nullptr, 0, 0))
	{
		TranslateMessage(&msg);
		DispatchMessageW(&msg);
	}

	URLProtocolUtil::RemoveURLProtocols();

	return static_cast<int>(msg.wParam);
}

static NOTIFYICONDATAW g_nid = {
	.cbSize = sizeof(g_nid),
	.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP,
	.uCallbackMessage = WM_NOTIFYICON,
	.uVersion = NOTIFYICON_VERSION_4
};
static struct {
	HICON normal;
	HICON disabled;
} NotifyIconLight, NotifyIconDark;
static bool systemUsesLightTheme = true;

HICON SelectNotifyIcon()
{
	auto NotifyIcon = &NotifyIconLight;
	if (!systemUsesLightTheme)
		NotifyIcon = &NotifyIconDark;
	return g_settings.systemProxy ? NotifyIcon->normal : NotifyIcon->disabled;
}

void ShowBalloon(const wchar_t* info, const wchar_t* title, DWORD flag = NIIF_INFO)
{
	NOTIFYICONDATAW nid = {
		.cbSize = sizeof(nid),
		.hWnd = g_hWnd,
		.uFlags = NIF_INFO | NIF_SHOWTIP,
		.dwInfoFlags = flag | NIIF_RESPECT_QUIET_TIME
	};
	if (title)
		wcscpy_s(nid.szInfoTitle, title);
	if (info)
		wcscpy_s(nid.szInfo, info);
	LOG_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_MODIFY, &nid));
}

void ShowConsoleWindow(bool show)
{
	CheckMenuItem(g_hContextMenu, IDM_EXPERIMENTAL_SHOWCLASHCONSOLE, MF_BYCOMMAND | (show ? MF_CHECKED : MF_UNCHECKED));
	const HWND hWnd = ProcessManager::GetConsoleWindow();
	if (hWnd)
	{
		ShowWindow(hWnd, show ? SW_SHOW : SW_HIDE);
		if (show)
			SetForegroundWindow(hWnd);
	}
}

winrt::fire_and_forget StartClash();

IAsyncAction ProcessMonitor()
{
	HANDLE hSubProcess = ProcessManager::GetSubProcessInfo().hProcess,
		hClashProcess = ProcessManager::GetClashProcessInfo().hProcess;

	co_await winrt::resume_on_signal(hClashProcess);

	g_clashRunning = false;

	if (ProcessManager::IsRunning() != ProcessManager::State::Running) // Manually stop
		co_return;

	DWORD exitCode = 0;
	THROW_IF_WIN32_BOOL_FALSE(GetExitCodeProcess(hClashProcess, &exitCode));

	LOG_HR_MSG(E_FAIL, "Clash exited with code: %ul", exitCode);

	FILETIME creationTime = {}, exitTime = {}, kernelTime, userTime;
	THROW_IF_WIN32_BOOL_FALSE(GetProcessTimes(hClashProcess, &creationTime, &exitTime, &kernelTime, &userTime));

	auto creation = winrt::clock::from_FILETIME(creationTime);
	auto exit = winrt::clock::from_FILETIME(exitTime);
	if (exit - creation < 5s)
	{
		g_balloonClickAction = BalloonClickAction::ShowConsoleWindow;
		ShowBalloon(_(L"Clash process was alive less than 5 seconds\nTap to view console log"), _(L"Error"), NIIF_ERROR);
		co_await winrt::resume_on_signal(hSubProcess);
	}

	if (ProcessManager::IsRunning() != ProcessManager::State::Running) // Manually stop
		co_return;

	ProcessManager::Stop();
	ShowConsoleWindow(false);
	StartClash();
}

winrt::fire_and_forget StartClash()
{
	if (ProcessManager::Start())
	{
		g_clashRunning = true;
		g_processMonitor = ProcessMonitor();
		for (size_t i = 0; i < 5; ++i)
		{
			if (!g_clashRunning)
				co_return;

			co_await winrt::resume_after(1s);
			try
			{
				g_clashVersion = g_clashApi->GetVersion();
				g_clashConfig = g_clashApi->GetConfig();
				EnableSystemProxy(g_settings.systemProxy);
				g_nid.hIcon = SelectNotifyIcon();
				Shell_NotifyIconW(NIM_MODIFY, &g_nid);
			}
			catch (...)
			{
				LOG_CAUGHT_EXCEPTION();
				continue;
			}
			g_clashOnline = true;
			co_return;
		}
		g_balloonClickAction = BalloonClickAction::ShowConsoleWindow;
		ShowBalloon(_(L"Clash started, but failed to communiacte\nTap to view console log"), _(L"Error"), NIIF_ERROR);
	}
	else
	{
		ShowBalloon(_(L"Failed to start clash."), _(L"Error"), NIIF_ERROR);
	}
}

winrt::fire_and_forget UpdateConfigFile(fs::path name, bool showSuccess = false)
{
	co_await winrt::resume_background();
	std::optional<std::wstring> errorDesp;
	try
	{
		if (name.empty())
			errorDesp = g_clashApi->RequestConfigUpdate(g_configPath / g_settings.configFile);
		else
		{
			auto config = g_configPath / name;
			errorDesp = g_clashApi->RequestConfigUpdate(config);
			if (!errorDesp.has_value())
			{
				g_settings.configFile = name.native();
				ProcessManager::SetConfigFile(config);
			}
		}
	}
	catch (...)
	{
		LOG_CAUGHT_EXCEPTION();
		co_return;
	}
	co_await ResumeForeground();
	if (errorDesp.has_value())
		ShowBalloon(errorDesp->c_str(), _(L"Reload Config Fail"), NIIF_ERROR);
	else if (showSuccess)
		ShowBalloon(_(L"Success"), _(L"Reload Config Succeed"));
}

LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam)
{
	static UINT WM_TASKBAR_CREATED = 0;

	switch (message)
	{
	case WM_CREATE:
	{
		g_hWnd = hWnd;

		MenuUtil::SetupMenu();

		auto hdc = wil::GetDC(hWnd);
		auto bitmap = LoadPNGFromResource();
		NotifyIconLight.normal = CreateIconFromWICBitmap(hdc.get(), bitmap.get(), 255, false);
		NotifyIconLight.disabled = CreateIconFromWICBitmap(hdc.get(), bitmap.get(), 90, false);
		if (g_darkModeSupported)
		{
			NotifyIconDark.normal = CreateIconFromWICBitmap(hdc.get(), bitmap.get(), 255, true);
			NotifyIconDark.disabled = CreateIconFromWICBitmap(hdc.get(), bitmap.get(), 180, true);

			systemUsesLightTheme = ShouldSystemUseLightTheme();
		}

		g_nid.hWnd = hWnd;
		g_nid.hIcon = SelectNotifyIcon();
		wcscpy_s(g_nid.szTip, _(L"ClashXW"));

		if (Shell_NotifyIconW(NIM_ADD, &g_nid))
		{
			FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
		}
		else
		{
			LOG_LAST_ERROR();
		}

		WM_TASKBAR_CREATED = RegisterWindowMessageW(L"TaskbarCreated");
		LOG_LAST_ERROR_IF(WM_TASKBAR_CREATED == 0);

		auto assetsDir = g_exePath / CLASH_ASSETS_DIR_NAME;
		ProcessManager::SetArgs(assetsDir / CLASH_EXE_NAME, assetsDir, g_configPath / g_settings.configFile, assetsDir / CLASH_DASHBOARD_DIR_NAME, CLASH_CTL_ADDR, CLASH_CTL_SECRET);
		g_clashApi = std::make_unique<ClashApi>(L"127.0.0.1", static_cast<INTERNET_PORT>(9090));

		SetupDataDirectory();
		StartClash();

		CheckMenuItem(g_hContextMenu, IDM_REMOTECONFIG_AUTOUPDATE, MF_BYCOMMAND | (g_settings.configAutoUpdate ? MF_CHECKED : MF_UNCHECKED));
		CheckMenuItem(g_hContextMenu, IDM_EXPERIMENTAL_OPENDASHBOARDINBROWSER, MF_BYCOMMAND | (g_settings.openDashboardInBrowser ? MF_CHECKED : MF_UNCHECKED));
		//EnableMenuItem(g_hContextMenu, IDM_STARTATLOGIN, MF_BYCOMMAND | (g_portableMode ? MF_DISABLED : MF_ENABLED));
		EnableMenuItem(g_hContextMenu, IDM_STARTATLOGIN, MF_BYCOMMAND | MF_ENABLED);

		WatchConfigFile();
	}
	break;
	case WM_COMMAND:
	{
		WORD wmId = LOWORD(wParam);
		switch (wmId)
		{
		case IDM_MODE_GLOBAL:
		case IDM_MODE_RULE:
		case IDM_MODE_DIRECT:
		{
			ClashProxyMode mode = static_cast<ClashProxyMode>(wmId - IDM_MODE_GLOBAL + 1);
			[mode]() -> winrt::fire_and_forget {
				co_await winrt::resume_background();
				try
				{
					if (!g_clashApi->UpdateProxyMode(mode))
						co_return;
					g_clashConfig = g_clashApi->GetConfig();
				}
				catch (...)
				{
					LOG_CAUGHT_EXCEPTION();
					co_return;
				}
				co_await ResumeForeground();
				MenuUtil::UpdateMenus();
			}();
		}
		break;
		case IDM_ALLOWFROMLAN:
		{
			[]() -> winrt::fire_and_forget {
				co_await winrt::resume_background();
				try
				{
					if (!g_clashApi->UpdateAllowLan(!g_clashConfig.allowLan))
						co_return;
					g_clashConfig = g_clashApi->GetConfig();
				}
				catch (...)
				{
					LOG_CAUGHT_EXCEPTION();
					co_return;
				}
				co_await ResumeForeground();
				MenuUtil::UpdateMenus();
			}();
		}
		break;
		case IDM_LOGLEVEL_ERROR:
		case IDM_LOGLEVEL_WARNING:
		case IDM_LOGLEVEL_INFO:
		case IDM_LOGLEVEL_DEBUG:
		case IDM_LOGLEVEL_SILENT:
		{
			ClashLogLevel level = static_cast<ClashLogLevel>(wmId - IDM_LOGLEVEL_ERROR + 1);
			[level]() -> winrt::fire_and_forget {
				co_await winrt::resume_background();
				try
				{
					if (!g_clashApi->UpdateLogLevel(level))
						co_return;
					g_clashConfig = g_clashApi->GetConfig();
				}
				catch (...)
				{
					LOG_CAUGHT_EXCEPTION();
					co_return;
				}
				co_await ResumeForeground();
				MenuUtil::UpdateMenus();
			}();
		}
		break;
		case IDM_SYSTEMPROXY:
			EnableSystemProxy(!g_settings.systemProxy);
			g_nid.hIcon = SelectNotifyIcon();
			Shell_NotifyIconW(NIM_MODIFY, &g_nid);
			break;
		case IDM_STARTATLOGIN:
			StartAtLogin::SetEnable(!StartAtLogin::IsEnabled());
			break;
		case IDM_HELP_ABOUT:
		{
			static bool opened = false;
			if (!opened)
			{
				opened = true;
				DialogBoxW(g_hInst, MAKEINTRESOURCE(IDD_ABOUTBOX), hWnd, About);
				opened = false;
			}
		}
		break;
		case IDM_DASHBOARD:
			if (g_settings.openDashboardInBrowser)
				ShellExecuteW(hWnd, nullptr, L"http://127.0.0.1:9090/ui/", nullptr, nullptr, SW_SHOWNORMAL);
			else
			{
				if (EdgeWebView2::GetCount() == 0)
					EdgeWebView2::Create();
			}
			break;
		case IDM_CONFIG_OPENFOLDER:
			ShellExecuteW(hWnd, nullptr, g_configPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			break;
		case IDM_CONFIG_RELOAD:
			UpdateConfigFile({});
			break;
		case IDM_REMOTECONFIG_MANAGE:
			ShowRemoteConfigDialog(hWnd);
			break;
		case IDM_REMOTECONFIG_UPDATE:
			[]() -> winrt::fire_and_forget {
				co_await winrt::resume_background();
				RemoteConfigManager::CheckUpdate(true);
			}();
			break;
		case IDM_REMOTECONFIG_AUTOUPDATE:
			g_settings.configAutoUpdate = !g_settings.configAutoUpdate;
			CheckMenuItem(g_hContextMenu, IDM_REMOTECONFIG_AUTOUPDATE, MF_BYCOMMAND | (g_settings.configAutoUpdate ? MF_CHECKED : MF_UNCHECKED));
			break;
		case IDM_EXPERIMENTAL_SETBYPASSRULES:
		{
			auto bypassRules = g_settings.bypassRules;
			int button = 0;
			auto hr = TaskDialogInput(hWnd, g_hInst, _(L"Set system proxy bypass rules"), nullptr, _(L"Set system proxy bypass rules"), TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, MAKEINTRESOURCEW(IDI_CLASHXW), &button, bypassRules);
			if (SUCCEEDED(hr) && button == IDOK)
				g_settings.bypassRules = bypassRules;
		}
		break;
		case IDM_EXPERIMENTAL_SETBENCHURL:
		{
			auto benchmarkUrl = g_settings.benchmarkUrl;
			int button = 0;
			auto hr = TaskDialogInput(hWnd, g_hInst, _(L"Set benchmark url"), nullptr, _(L"Set benchmark url"), TDCBF_OK_BUTTON | TDCBF_CANCEL_BUTTON, MAKEINTRESOURCEW(IDI_CLASHXW), &button, benchmarkUrl);
			if (SUCCEEDED(hr) && button == IDOK)
			{
				if (IsUrlVaild(benchmarkUrl.c_str()))
					g_settings.benchmarkUrl = benchmarkUrl;
				else
					TaskDialog(hWnd, nullptr, _(L"Warning"), nullptr, _(L"URL is not valid"), TDCBF_OK_BUTTON, TD_WARNING_ICON, nullptr);
			}
		}
		break;
		case IDM_EXPERIMENTAL_OPENDASHBOARDINBROWSER:
			g_settings.openDashboardInBrowser = !g_settings.openDashboardInBrowser;
			CheckMenuItem(g_hContextMenu, IDM_EXPERIMENTAL_OPENDASHBOARDINBROWSER, MF_BYCOMMAND | (g_settings.openDashboardInBrowser ? MF_CHECKED : MF_UNCHECKED));
			break;
		case IDM_EXPERIMENTAL_SHOWCLASHCONSOLE:
		{
			const HWND hWndConsole = ProcessManager::GetConsoleWindow();
			if (hWndConsole)
			{
				ShowConsoleWindow(!IsWindowVisible(hWndConsole));
			}
			else
				ShowConsoleWindow(false);
		}
		break;
		case IDM_QUIT:
			DestroyWindow(hWnd);
			break;
		default:
			return DefWindowProcW(hWnd, message, wParam, lParam);
		}
	}
	break;
	case WM_MENUCOMMAND:
	{
		size_t i = static_cast<size_t>(wParam);
		HMENU hMenu = reinterpret_cast<HMENU>(lParam);

		if (hMenu == g_hTopMenu || hMenu == g_hPortsMenu) {}
		else if (hMenu == g_hContextMenu ||
			hMenu == g_hProxyModeMenu ||
			hMenu == g_hLogLevelMenu)
		{
			MenuUtil::PostCommandByMenuIndex(hWnd, hMenu, i);
		}
		else if (hMenu == g_hConfigMenu)
		{
			if (i < g_configFilesList.size())
			{
				UpdateConfigFile(g_configFilesList[i]);
			}
			else
			{
				MenuUtil::PostCommandByMenuIndex(hWnd, hMenu, i);
			}
		}
		else if (!MenuUtil::HandleProxyGroupsItemChoose(hMenu, i))
		{
			MenuUtil::PostCommandByMenuIndex(hWnd, hMenu, i);
		}
	}
	break;
	case WM_DESTROY:
		SetSystemProxy(nullptr);
		ProcessManager::SendStopSignal();
		StopWatchConfigFile();
		if (g_processMonitor)
			g_processMonitor.Cancel();
		SaveSettings();
		WaitForSingleObject(ProcessManager::GetClashProcessInfo().hProcess, 3000);
		ProcessManager::Stop();
		Shell_NotifyIconW(NIM_DELETE, &g_nid);
		PostQuitMessage(0);
		break;
	case WM_ENTERIDLE:
		if (wParam == MSGF_MENU)
			MenuUtil::OnMenuEnterIdle(reinterpret_cast<HWND>(lParam));
		break;
	case WM_SETTINGCHANGE:
		if (g_darkModeSupported && IsColorSchemeChangeMessage(lParam))
		{
			systemUsesLightTheme = ShouldSystemUseLightTheme();
			g_nid.hIcon = SelectNotifyIcon();
			Shell_NotifyIconW(NIM_MODIFY, &g_nid);
		}
		break;
	case WM_NOTIFYICON:
		switch (LOWORD(lParam))
		{
		case NIN_SELECT:
		case NIN_KEYSELECT:
		case WM_CONTEXTMENU:
			[](HWND hWnd, WPARAM wParam) -> winrt::fire_and_forget {
				co_await winrt::resume_background();
				[]() -> IAsyncAction {
					co_await winrt::resume_background();
					try
					{
						g_clashConfig = g_clashApi->GetConfig();
						g_clashProxies = g_clashApi->GetProxies();
					}
					CATCH_LOG();
#ifdef _DEBUG
				}().wait_for(1000ms);
#else
				}().wait_for(200ms);
#endif
				co_await ResumeForeground();
				MenuUtil::UpdateMenus();
				MenuUtil::ShowContextMenu(hWnd, GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam));
			}(hWnd, wParam);
			break;
		case NIN_BALLOONUSERCLICK:
			switch (g_balloonClickAction)
			{
			case BalloonClickAction::ReloadConfig:
				UpdateConfigFile({}, true);
				break;
			case BalloonClickAction::ShowConsoleWindow:
				ShowConsoleWindow(true);
				break;
			}
			[[fallthrough]];
		case NIN_BALLOONTIMEOUT:
			g_balloonClickAction = BalloonClickAction::None;
			break;
		}
		break;
	case WM_RESUMECORO:
		std::coroutine_handle<>::from_address(reinterpret_cast<void*>(wParam)).resume();
		break;
	case WM_CONFIGCHANGEDETECT:
		if (g_balloonClickAction == BalloonClickAction::None)
		{
			g_balloonClickAction = BalloonClickAction::ReloadConfig;
			ShowBalloon(_(L"Tap to reload config"), _(L"Config file have been changed"));
		}
		break;
	case WM_ADDREMOTECONFIG:
	try
	{
		wil::unique_handle hFileMapping(OpenFileMappingW(FILE_MAP_READ, FALSE, CLASHXW_URL_FILEMAP_NAME));
		THROW_LAST_ERROR_IF_NULL(hFileMapping);

		wil::unique_mapview_ptr<char> buffer(reinterpret_cast<char*>(MapViewOfFile(hFileMapping.get(), FILE_MAP_READ, 0, 0, 0)));
		THROW_LAST_ERROR_IF_NULL(buffer);

		std::string_view u8Url = buffer.get();
		std::string_view u8Name = buffer.get() + u8Url.size() + 1;

		[](std::wstring url, std::wstring name) -> winrt::fire_and_forget {
			co_await ResumeForeground();
			OpenEditConfigDialog(g_hWnd, url, name);
		}(Utf8ToUtf16(u8Url), Utf8ToUtf16(u8Name));
	}
	CATCH_LOG();
	break;
	default:
		if (WM_TASKBAR_CREATED && message == WM_TASKBAR_CREATED)
		{
			if (g_darkModeSupported)
				systemUsesLightTheme = ShouldSystemUseLightTheme();
			g_nid.hIcon = SelectNotifyIcon();
			if (!Shell_NotifyIconW(NIM_MODIFY, &g_nid))
			{
				FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_ADD, &g_nid));
				FAIL_FAST_IF_WIN32_BOOL_FALSE(Shell_NotifyIconW(NIM_SETVERSION, &g_nid));
			}
		}
		return DefWindowProcW(hWnd, message, wParam, lParam);
	}
	return 0;
}

// Message handler for about box.
INT_PTR CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	static wil::unique_hbrush hDarkBrush;
	switch (message)
	{
	case WM_INITDIALOG:
		if (g_darkModeSupported)
		{
			AllowDarkModeForWindow(hDlg, true);
			RefreshTitleBarThemeColor(hDlg);
		}
		return (INT_PTR)TRUE;
	case WM_COMMAND:
		if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL)
		{
			EndDialog(hDlg, LOWORD(wParam));
			return (INT_PTR)TRUE;
		}
		break;
	case WM_DESTROY:
		hDarkBrush.reset();
		break;
	case WM_NOTIFY:
	{
		auto nml = reinterpret_cast<PNMLINK>(lParam);
		if (nml->hdr.code == NM_CLICK)
		{
			if (*nml->item.szUrl)
			{
				ShellExecuteW(hDlg, nullptr, nml->item.szUrl, nullptr, nullptr, SW_SHOWNORMAL);
			}
			else if (!wcscmp(nml->item.szID, L"license"))
			{
				ShowRichEditDialog(hDlg, _(L"ClashXW License"), 1);
			}
			else if (!wcscmp(nml->item.szID, L"os_license"))
			{
				ShowOpenSourceLicensesDialog(hDlg);
			}
		}
	}
	break;
	case WM_CTLCOLORDLG:
	case WM_CTLCOLORSTATIC:
	{
		bool darkModeEnabled = ShouldAppsUseDarkMode() && !IsHighContrast();
		if (message == WM_CTLCOLORSTATIC)
		{
			HDC hdc = reinterpret_cast<HDC>(wParam);
			COLORREF textColor = DarkWindowTextColor;
			COLORREF bkColor = DarkWindowBkColor;

			if (!darkModeEnabled)
			{
				textColor = GetSysColor(COLOR_WINDOWTEXT);
				bkColor = GetSysColor(COLOR_WINDOW);
			}

			SetTextColor(hdc, textColor);
			SetBkColor(hdc, bkColor);
			SetBkMode(hdc, TRANSPARENT);
		}
		if (darkModeEnabled)
		{
			if (!hDarkBrush)
				hDarkBrush.reset(CreateSolidBrush(DarkWindowBkColor));
			return reinterpret_cast<INT_PTR>(hDarkBrush.get());
		}
		return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
	}
	case WM_SETTINGCHANGE:
	{
		if (g_darkModeSupported && IsColorSchemeChangeMessage(lParam))
		{
			RefreshTitleBarThemeColor(hDlg);
			RedrawWindow(hDlg, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
		}
	}
	break;
	}
	return (INT_PTR)FALSE;
}
