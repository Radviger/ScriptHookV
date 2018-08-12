#include "ScriptManager.h"
#include "ScriptThread.h"
#include "NativeInvoker.h"
#include "..\Utility\Log.h"
#include "..\Utility\General.h"
#include "..\ASI Loader\ASILoader.h"
#include "..\Input\InputHook.h"
#include "..\DirectX\D3d11Hook.h"
#include "..\Hooking\Hooking.h"
#include "Pools.h"
	
#include <StackWalker.h>
#pragma comment(lib, "StackWalker.lib")
// Specialized stackwalker-output classes
// Console (printf):
class StackWalkerToConsole : public StackWalker
{
protected:
	virtual void OnOutput(LPCSTR szText)
	{
		std::ofstream LOG;

		std::string fileName = Utility::GetOurModuleFolder() + "\\" + "StackTrace" + ".txt";

		LOG.open(fileName, std::ofstream::out | std::ofstream::app);

		LOG << szText << std::endl;

		LOG.close();
	}
};

LONG WINAPI ExpFilter(EXCEPTION_POINTERS* pExp, DWORD /*dwExpCode*/)
{
	//StackWalker sw;  // output to default (Debug-Window)
	StackWalkerToConsole sw; // output to the console
	sw.ShowCallstack(GetCurrentThread(), pExp->ContextRecord);
	return EXCEPTION_EXECUTE_HANDLER;
}

#define DLL_EXPORT __declspec( dllexport )

using namespace NativeInvoker::Helper;

ScriptThread	g_ScriptThread;
ScriptThread	g_AdditionalThread;
HANDLE          g_MainFiber;
Script*			g_CurrentScript;

/* ####################### SCRIPT #######################*/

void Script::Tick() 
{
	if (timeGetTime() < wakeAt)
	{
		if (GetCurrentFiber() != g_MainFiber) SwitchToFiber(g_MainFiber); return;
	}

	else if (scriptFiber)
	{
		g_CurrentScript = this;
		SwitchToFiber(scriptFiber);
		g_CurrentScript = nullptr;
	}

	else
	{
		scriptFiber = CreateFiber(NULL, [](LPVOID handler) {reinterpret_cast<Script*>(handler)->Run(); }, this);
	}

	SwitchToFiber(g_MainFiber);
}

void Script::Run() 
{
	__try
	{
		callbackFunction();
	}
	__except (ExpFilter(GetExceptionInformation(), GetExceptionCode()))
	{
		g_AdditionalThread.RemoveScript(this->GetCallbackFunction());
		g_ScriptThread.RemoveScript(this->GetCallbackFunction());
	}
}

void Script::Wait( uint32_t time ) 
{
    wakeAt = timeGetTime() + time;
	if (g_MainFiber) SwitchToFiber(g_MainFiber);
}

void ScriptThread::DoRun() 
{
	for (auto & pair : m_scripts)
	{
		pair.second->Tick();
	}
}

void ScriptThread::Reset() 
{
	g_AdditionalThread.RemoveAllScripts();
    g_ScriptThread.RemoveAllScripts();
    ASILoader::Initialize();
}

void ScriptThread::AddScript( HMODULE module, void( *fn )( ) ) 
{
    const std::string moduleName = Utility::GetModuleNameWithoutExtension( module);

	if (m_scripts.find(module) != m_scripts.end()) 
	{
		LOG_ERROR("Script '%s' is already registered", moduleName.c_str()); return;
	}	
	else
	{
		ScriptEngine::Notification(FMT("Loaded '%s'", moduleName.c_str()));
		LOG_PRINT("Registering script '%s' (0x%p)", moduleName.c_str(), fn);
		m_scripts[module] = std::make_shared<Script>(fn);
	}
}

void ScriptThread::RemoveScript(HMODULE module)
{
	const std::string name = Utility::GetModuleNameWithoutExtension(module);

	if (m_scripts.size())
	{
		auto foundIter = m_scripts.find(module);
		if (foundIter != m_scripts.end())
		{
			m_scripts.erase(foundIter);
			FreeLibrary(module);
			ScriptEngine::Notification(FMT("Removed '%s'", name.c_str()));
			LOG_PRINT("Unregistered script '%s'", name.c_str());
		}
	}
}

void ScriptThread::RemoveScript(void(*fn)()) 
{
    for ( auto it = m_scripts.begin(); it != m_scripts.end(); it++ ) 
	{
        auto pair = *it;

		if ( pair.second->GetCallbackFunction() == fn ) 
		{
            RemoveScript(pair.first);
        }
    }
}

void ScriptThread::RemoveAllScripts()
{
	if (g_MainFiber != nullptr && GetCurrentFiber() != g_MainFiber)
		SwitchToFiber(g_MainFiber);
	
	if (std::size(m_scripts) > 0)
	{
		for (auto & pair : m_scripts)
		{
			RemoveScript(pair.first);
		}	m_scripts.clear();
		Utility::playwindowsSound("Windows Default.wav");
	}
}

/* ####################### SCRIPTMANAGER #######################*/

void ScriptManager::WndProc(HWND /*hwnd*/, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	for (auto & function : g_WndProcCb) function(uMsg, wParam, lParam);

	if (uMsg == WM_KEYDOWN || uMsg == WM_KEYUP || uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP)
	{
		for (auto & function : g_keyboardFunctions) function((DWORD)wParam, lParam & 0xFFFF, (lParam >> 16) & 0xFF, (lParam >> 24) & 1, (uMsg == WM_SYSKEYDOWN || uMsg == WM_SYSKEYUP), (lParam >> 30) & 1, (uMsg == WM_SYSKEYUP || uMsg == WM_KEYUP));
	}
}

void ScriptManager::MainFiber()
{
	g_MainFiber = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(nullptr);

	if (g_MainFiber)
	{
		static scrThread* target_thread = nullptr;

		scrThread* current_thread = GetActiveThread();

		// do this while script::wait
		if (target_thread && current_thread->m_ctx.State == ThreadStateIdle)
		{
			if (current_thread->m_ctx.ScriptHash != g_ThreadHash)
			{
				SetActiveThread(target_thread);
				g_AdditionalThread.DoRun();
				SetActiveThread(current_thread);
			}
		}
		else if (current_thread->m_ctx.State == ThreadStateRunning)
		{
			if (current_thread->m_ctx.ScriptHash == g_ThreadHash)
			{
				if (!target_thread) target_thread = current_thread;
				g_ScriptThread.DoRun();
			}
		}

		static bool RemoveAllScriptsBool = false; const uint32_t RemoveAllScriptsKey = VK_NEXT; //Page Down
		static bool ReloadAllScriptsBool = false; const uint32_t ReloadAllScriptsKey = VK_PRIOR;//Page Up
		static bool RemoveScriptHookBool = false; const uint32_t RemoveScriptHookKey = VK_END;

		if (isKeyPressedOnce(RemoveAllScriptsBool, RemoveAllScriptsKey))
		{
			g_AdditionalThread.RemoveAllScripts();
			g_ScriptThread.RemoveAllScripts();
		}

		if (isKeyPressedOnce(ReloadAllScriptsBool, ReloadAllScriptsKey))
		{
			g_AdditionalThread.Reset();
			g_ScriptThread.Reset();
		}

		if (isKeyPressedOnce(RemoveScriptHookBool, RemoveScriptHookKey))
		{
			g_HookState = HookStateExiting;
		}

		while (!g_Stack.empty())
		{
			g_Stack.front();
			g_Stack.pop_front();
		}
	}
}

DWORD WINAPI ExitGtaVThread(LPVOID/*lpParameter*/)
{
	InputHook::Remove();
	g_D3DHook.ReleaseDevices();
	Hooking::RemoveAllDetours();
	FreeLibraryAndExitThread(Utility::GetOurModuleHandle(), ERROR_SUCCESS);
	g_HookState = HookStateUnknown;
	return ERROR_SUCCESS;
}

void ScriptManager::UnloadHook()
{
	LOG_DEBUG("Exiting GTA5.exe Process");

	g_AdditionalThread.RemoveAllScripts();

	g_ScriptThread.RemoveAllScripts();

	if (ConvertFiberToThread())
	{
		CloseHandle(g_MainFiber);
		Utility::CreateElevatedThread(ExitGtaVThread);
	}
}

/* ####################### EXPORTS #######################*/

/*Input*/
DLL_EXPORT void WndProcHandlerRegister(TWndProcFn function) 
{
    g_WndProcCb.insert(function);
}

DLL_EXPORT void WndProcHandlerUnregister(TWndProcFn function) 
{
    g_WndProcCb.erase(function);
}

/* keyboard */
DLL_EXPORT void keyboardHandlerRegister(KeyboardHandler function) 
{
    g_keyboardFunctions.insert(function);
}

DLL_EXPORT void keyboardHandlerUnregister(KeyboardHandler function) 
{
    g_keyboardFunctions.erase(function);
}

/* D3d SwapChain */
DLL_EXPORT void presentCallbackRegister(PresentCallback cb) 
{
    g_D3DHook.AddCallback(cb);
}

DLL_EXPORT void presentCallbackUnregister(PresentCallback cb) 
{
    g_D3DHook.RemoveCallback(cb);
}

/* textures */
DLL_EXPORT int createTexture(const char *texFileName) 
{
	return g_D3DHook.CreateTexture(texFileName);
}

DLL_EXPORT void drawTexture(int id, int index, int level, int time, float sizeX, float sizeY, float centerX, float centerY, float posX, float posY, float rotation, float screenHeightScaleFactor, float r, float g, float b, float a) 
{
	g_D3DHook.DrawTexture(id, index, level, time, sizeX, sizeY, centerX, centerY, posX, posY, rotation, screenHeightScaleFactor, r, g, b, a);
}

/* scripts */
DLL_EXPORT void changeScriptThread(UINT32 hash)
{
	if (g_ThreadHash != hash)
		g_ThreadHash = hash;
}

DLL_EXPORT void scriptWait(DWORD time) 
{
	g_CurrentScript->Wait(time);
}

DLL_EXPORT void scriptRegister(HMODULE module, void(*function)())
{
    g_ScriptThread.AddScript(module, function);
}

DLL_EXPORT void scriptRegisterAdditionalThread(HMODULE module, void(*function)()) 
{
	g_AdditionalThread.AddScript(module, function);
}

DLL_EXPORT void scriptUnregister(HMODULE module)
{
	g_AdditionalThread.RemoveScript(module);
    g_ScriptThread.RemoveScript(module);
}

DLL_EXPORT void scriptUnregister(void(*function)()) 
{ 
    // deprecated
	g_AdditionalThread.RemoveScript(function);
    g_ScriptThread.RemoveScript(function);
}

/* natives */
DLL_EXPORT void nativeInit(UINT64 hash) 
{
	g_hash = hash;

	g_context.Reset();
}

DLL_EXPORT void nativePush64(UINT64 val) 
{
	g_context.Push(val);
}

DLL_EXPORT uint64_t* nativeCall() 
{
	NativeInvoker::Helper::CallNative(&g_context, g_hash);
	return g_Returns.getRawPtr();
}

/* global variables */
DLL_EXPORT UINT64 *getGlobalPtr(int globalId) 
{
    return ScriptEngine::getGlobal(globalId);
}

/* world pools */
DLL_EXPORT int worldGetAllPeds(int *arr, int arrSize) 
{
	return rage::GetAllWorld(PoolTypePed, arrSize, arr);
}

DLL_EXPORT int worldGetAllVehicles(int *arr, int arrSize) 
{
	return rage::GetAllWorld(PoolTypeVehicle, arrSize, arr);
}

DLL_EXPORT int worldGetAllObjects(int *arr, int arrSize) 
{
	return rage::GetAllWorld(PoolTypeObject, arrSize, arr);
}

DLL_EXPORT int worldGetAllPickups(int *arr, int arrSize) 
{
	return rage::GetAllWorld(PoolTypePickup, arrSize, arr);
}

/* game version */
DLL_EXPORT eGameVersion getGameVersion() 
{
	return static_cast<eGameVersion>(g_GameVersion);
}

/* misc */
DLL_EXPORT BYTE* getScriptHandleBaseAddress(int handle) 
{
    return (BYTE*)rage::GetEntityAddress(handle);
}

DLL_EXPORT UINT32 registerRawStreamingFile(const char* fileName, const char* registerAs, bool errorIfFailed)
{
	UINT32 textureID;
	return rage::FileRegister(&textureID, fileName, true, registerAs, errorIfFailed) ? textureID : 0;
}

DLL_EXPORT PVOID createDetour(PVOID* pTarget, PVOID pHandler, const char* name = nullptr)
{
	return Hooking::CreateDetour(pTarget, pHandler, name);
}

DLL_EXPORT void removeDetour(PVOID* ppTarget, PVOID pHandler)
{
	Hooking::RemoveDetour(ppTarget, pHandler);
}

















