/*
 * aurora-desktop.exe: deja listo el escritorio de Wine (explorer.exe) antes de
 * abrir una aplicacion y lo mantiene vivo un rato.
 *
 * La primera app de una sesion de Wine arranca explorer.exe y, en cuanto este
 * parece "ocioso", le pregunta por el driver grafico (la propiedad
 * __wine_display_device_guid del escritorio). Si explorer aun esta cargando
 * (Mesa/llvmpipe, Vulkan...) la propiedad no esta, y la app falla con
 * "The explorer process failed to start": no sale ninguna ventana.
 *
 * Causa de fondo (Wine 11, win32u get_desktop_window): la app arranca explorer y
 * llama a WaitForInputIdle(explorer), pero el evento "ocioso" de un proceso
 * solo existe cuando este termina de inicializarse. Si explorer tarda en
 * cargar (AuroraOS, QEMU), WaitForInputIdle vuelve al instante con error, la
 * app crea ella misma la ventana del escritorio y la de explorer queda
 * rechazada (WM_NCCREATE): el escritorio se queda sin driver toda la sesion.
 *
 * Este programa (de consola, sin ventanas) arranca explorer.exe /desktop el
 * primero y, sin tocar el escritorio, repite WaitForInputIdle hasta que
 * explorer de verdad esta esperando mensajes. Despues comprueba la propiedad,
 * escribe "LISTO" y se queda HOLD segundos para que el escritorio no se cierre
 * mientras arranca la app.
 * Se compila con clang + lld sin CRT (installer/wine/desktop-wait/build.sh).
 */
typedef void *HANDLE;
typedef void *HWND;
typedef unsigned long DWORD;
typedef int BOOL;
typedef unsigned short WCHAR;

__declspec(dllimport) HWND __stdcall GetDesktopWindow(void);
__declspec(dllimport) HANDLE __stdcall GetPropW(HWND, const WCHAR *);
__declspec(dllimport) void __stdcall Sleep(DWORD);
__declspec(dllimport) DWORD __stdcall GetTickCount(void);
__declspec(dllimport) HANDLE __stdcall GetStdHandle(DWORD);
__declspec(dllimport) BOOL __stdcall WriteFile(HANDLE, const void *, DWORD, DWORD *, void *);
__declspec(dllimport) void __stdcall ExitProcess(unsigned);
__declspec(dllimport) DWORD __stdcall WaitForInputIdle(HANDLE, DWORD);
__declspec(dllimport) BOOL __stdcall CreateProcessW(const WCHAR *, WCHAR *, void *, void *, BOOL,
                                                    DWORD, void *, const WCHAR *, void *, void *);
__declspec(dllimport) DWORD __stdcall WaitForSingleObject(HANDLE, DWORD);

typedef struct {
  DWORD cb; WCHAR *lpReserved, *lpDesktop, *lpTitle;
  DWORD dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
  unsigned short wShowWindow, cbReserved2; unsigned char *lpReserved2;
  HANDLE hStdInput, hStdOutput, hStdError;
} STARTUPINFOW;
typedef struct { HANDLE hProcess, hThread; DWORD dwProcessId, dwThreadId; } PROCESS_INFORMATION;

#define WAIT_MS 120000u
#define HOLD_MS 60000u

static void say(const char *s, DWORD n) {
  DWORD w;
  WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0);
}

void start(void) {
  static const WCHAR prop[] = {'_','_','w','i','n','e','_','d','i','s','p','l','a','y','_',
                               'd','e','v','i','c','e','_','g','u','i','d',0};
  static WCHAR cmd[] = {'C',':','\\','w','i','n','d','o','w','s','\\','s','y','s','t','e','m',
                        '3','2','\\','e','x','p','l','o','r','e','r','.','e','x','e',' ',
                        '/','d','e','s','k','t','o','p',0};
  STARTUPINFOW si;
  PROCESS_INFORMATION pi;
  unsigned char *p = (unsigned char *)&si;
  for (unsigned i = 0; i < sizeof(si); i++) p[i] = 0;
  si.cb = sizeof(si);
  DWORD t0 = GetTickCount();
  if (CreateProcessW(0, cmd, 0, 0, 0, 0, 0, 0, &si, &pi)) {
    /* WAIT_FAILED (sin evento aun) mientras explorer se inicializa. */
    for (;;) {
      DWORD r = WaitForInputIdle(pi.hProcess, 1000);
      if (r == 0) break;
      if (WaitForSingleObject(pi.hProcess, 0) == 0) { say("EXPLORER-SALIO\n", 15); ExitProcess(2); }
      if (GetTickCount() - t0 > WAIT_MS) { say("TIEMPO\n", 7); ExitProcess(1); }
      if (r != 0x102 /* WAIT_TIMEOUT */) Sleep(200);
    }
  }
  HWND desk = GetDesktopWindow();
  while (!GetPropW(desk, prop)) {
    if (GetTickCount() - t0 > WAIT_MS) {
      say("TIEMPO\n", 7);
      ExitProcess(1);
    }
    Sleep(200);
  }
  say("LISTO\n", 6);
  Sleep(HOLD_MS);
  ExitProcess(0);
}
