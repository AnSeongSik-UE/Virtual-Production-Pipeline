#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include <thread>
#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <objbase.h>
#include <shobjidl.h>
#include "Windows/HideWindowsPlatformTypes.h"
#ifdef LoadLibrary
#undef LoadLibrary
#endif
#endif

/** Owns a native dialog thread; no UObject or game-thread callbacks run here. */
class FVPAvatarFileDialog
{
public:
	FVPAvatarFileDialog() : Worker([this]() { Run(); }) {}
	~FVPAvatarFileDialog()
	{
		Cancelled.store(true);
		if (Worker.joinable()) { Worker.join(); }
	}
	void RequestForeground() { ForegroundRequested.store(true); }
	bool IsComplete() const { return Complete.load(); }
	const FString& GetPath() const { check(IsComplete()); return SelectedPath; }
	uint32 GetError() const { check(IsComplete()); return Error; }

private:
	std::atomic<bool> Cancelled{false};
	std::atomic<bool> Complete{false};
	std::atomic<bool> ForegroundRequested{false};
	FString SelectedPath;
	uint32 Error = 0;
	std::thread Worker;
#if PLATFORM_WINDOWS
	IFileOpenDialog* ActiveDialog = nullptr;

	static FVPAvatarFileDialog*& ThreadDialogState()
	{
		static thread_local FVPAvatarFileDialog* State = nullptr;
		return State;
	}

	static BOOL CALLBACK FindVisibleDialogWindow(HWND Window, LPARAM Parameter)
	{
		if (IsWindowVisible(Window))
		{
			*reinterpret_cast<HWND*>(Parameter) = Window;
			return 0;
		}
		return 1;
	}

	static VOID CALLBACK PollDialog(HWND, UINT, UINT_PTR, DWORD)
	{
		FVPAvatarFileDialog* State = ThreadDialogState();
		if (!State || !State->ActiveDialog)
		{
			return;
		}
		if (State->Cancelled.load())
		{
			State->ActiveDialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
			return;
		}
		if (State->ForegroundRequested.load())
		{
			HWND DialogWindow = nullptr;
			EnumThreadWindows(GetCurrentThreadId(), &FindVisibleDialogWindow, reinterpret_cast<LPARAM>(&DialogWindow));
			if (DialogWindow)
			{
				if (IsIconic(DialogWindow)) { ShowWindow(DialogWindow, SW_RESTORE); }
				BringWindowToTop(DialogWindow);
				SetForegroundWindow(DialogWindow);
				State->ForegroundRequested.store(false);
			}
		}
	}
#endif
	void Run()
	{
#if PLATFORM_WINDOWS
		const HRESULT InitializeResult = CoInitializeEx(
			nullptr,
			COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
		if (SUCCEEDED(InitializeResult) && !Cancelled.load())
		{
			HRESULT DialogResult = CoCreateInstance(
				CLSID_FileOpenDialog,
				nullptr,
				CLSCTX_INPROC_SERVER,
				IID_PPV_ARGS(&ActiveDialog));
			if (SUCCEEDED(DialogResult))
			{
				DWORD Options = 0;
				DialogResult = ActiveDialog->GetOptions(&Options);
				if (SUCCEEDED(DialogResult))
				{
					DialogResult = ActiveDialog->SetOptions(
						Options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
						FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR);
				}
				const COMDLG_FILTERSPEC Filter = {L"VRM (*.vrm)", L"*.vrm"};
				if (SUCCEEDED(DialogResult)) { DialogResult = ActiveDialog->SetFileTypes(1, &Filter); }
				if (SUCCEEDED(DialogResult)) { DialogResult = ActiveDialog->SetDefaultExtension(L"vrm"); }
				if (SUCCEEDED(DialogResult)) { DialogResult = ActiveDialog->SetTitle(L"VRM 추가"); }

				UINT_PTR TimerId = 0;
				if (SUCCEEDED(DialogResult) && !Cancelled.load())
				{
					ThreadDialogState() = this;
					TimerId = SetTimer(nullptr, 0, 100, &PollDialog);
					DialogResult = TimerId != 0
						? ActiveDialog->Show(nullptr)
						: HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
				}
				if (TimerId != 0) { KillTimer(nullptr, TimerId); }
				ThreadDialogState() = nullptr;

				if (SUCCEEDED(DialogResult) && !Cancelled.load())
				{
					IShellItem* SelectedItem = nullptr;
					DialogResult = ActiveDialog->GetResult(&SelectedItem);
					if (SUCCEEDED(DialogResult) && SelectedItem)
					{
						PWSTR FilePath = nullptr;
						DialogResult = SelectedItem->GetDisplayName(SIGDN_FILESYSPATH, &FilePath);
						if (SUCCEEDED(DialogResult) && FilePath)
						{
							SelectedPath = FilePath;
							CoTaskMemFree(FilePath);
						}
						SelectedItem->Release();
					}
				}
				if (FAILED(DialogResult) && DialogResult != HRESULT_FROM_WIN32(ERROR_CANCELLED) && !Cancelled.load())
				{
					Error = static_cast<uint32>(DialogResult);
				}
				ActiveDialog->Release();
				ActiveDialog = nullptr;
			}
			else if (!Cancelled.load()) { Error = static_cast<uint32>(DialogResult); }
		}
		else if (FAILED(InitializeResult) && !Cancelled.load())
		{
			Error = static_cast<uint32>(InitializeResult);
		}
		if (SUCCEEDED(InitializeResult)) { CoUninitialize(); }
#endif
		Complete.store(true);
	}
};
