# FlowDrop for Windows

Local-first Windows image converter: an Electron app plus a native Win32/OLE radial wheel.

## UI redesign (not built, run, or tested)
- Frameless window with native Windows controls overlay and Mica backdrop (Windows 11; solid fallback elsewhere).
- Token-based design system, light/dark/system theme (Settings), consistent motion and focus states, reduced-motion support.
- Convert view: adaptive dropzone, per-file status and progress, keyboard-navigable format picker.
- Settings view: theme, default format/quality, reveal-in-folder toggle (saved locally).
- Fix: dropped file paths now use `webUtils.getPathForFile` (`File.path` is removed in current Electron).

## Prerequisites
- Windows 10/11, Node.js 20+
- Native wheel only: Visual Studio Build Tools with the Desktop C++ workload (Developer PowerShell)

## Run
```cmd
cd /d path\to\flowdrop
npm install
npm run dev
```

## Build the Windows installer
```powershell
npm run dist
```

## Build and launch the native wheel
```powershell
.\native\build.ps1
.\native\FlowDropOlePoc.exe
```
Press and hold the left mouse button on a file, then press Shift once to show
the wheel. It remains visible until the file is released or Escape is pressed.
Release over a supported output petal to convert. The source extension is
detected from the real `IDataObject`, the same-format output is omitted, and
the center identifies the source format (for example, `JPEG ->`).

## Build a Windows installer
```powershell
npm run dist
```
The installer should be emitted under `release/`.

## Explorer integration

On Windows, FlowDrop registers a persistent per-user **Convert with FlowDrop**
entry in File Explorer's file context menu. Selecting it launches (or wakes)
the background app and passes the selected file paths into the existing
conversion surface. The app uses a single-instance lock, so repeated
selections are routed to the existing window instead of opening duplicate
processes. Closing the window hides it to the notification area; use **Quit**
from the tray menu to exit. The menu remains registered after quitting and is
removed when the application is uninstalled.

Windows does not provide a supported ordinary-app API for observing another
application's in-progress drag and conditionally opening a menu on Shift-drag.
Electron cannot register the required native OLE drag/drop target or global
mouse hook by itself. A native Explorer shell extension has the same boundary:
it can add context-menu verbs and receive a drop when Explorer targets that
extension, but it is not notified when another Explorer window has already
started an OLE drag. This prototype therefore does not claim to implement
Tangerine's exact Shift-drag gesture. The shell context-menu route is the
reliable, local equivalent currently implemented.

## Native OLE drop-target proof of concept

The repository also contains a standalone Win32 proof of concept in
[`native/FlowDropOlePoc.cpp`](./native/FlowDropOlePoc.cpp). It uses
`OleInitialize`, `RegisterDragDrop`, and a real `IDropTarget` implementation.
The target extracts Explorer files from `IDataObject` using `CF_HDROP` and
`DragQueryFileW`, logs `DragEnter`, `DragOver`, `DragLeave`, and `Drop`, reads
`MK_SHIFT`/`MK_ALT`/`MK_CONTROL`, and converts dropped images to JPEG, BMP, or
TIFF with GDI+ encoders that are available on the machine.
It returns `DROPEFFECT_COPY`, preserving the source file. Document conversion
uses a bundled or configured LibreOffice headless runtime for PDF, DOCX, XLSX,
PPTX, and TXT.

Build and test from a Visual Studio Developer PowerShell:

```powershell
.\native\build.ps1
.\native\FlowDropOlePoc.exe
```

Then open File Explorer, press and hold the left mouse button on an image, and
press Shift once. The wheel remains available for the entire active drag;
holding Shift continuously is not required. The console should log
`Input left=1 shift=1` and `Wheel shown`. The compact wheel is initially
positioned at the cursor and stays anchored after OLE `DragEnter` so a petal
can be selected. It uses shared, rounded petal paths; only output formats supported by
the native GDI+ conversion engine are shown for the dragged image, and the
source format itself is not offered as a redundant conversion. Release over a
highlighted petal to write a uniquely named converted copy beside the original.
Release outside a petal to receive `DROPEFFECT_NONE`. The wheel is clamped to
the current work area when the cursor is near a screen edge.

Document conversion supports PDF, DOCX, XLSX, PPTX, and TXT as source and
destination formats when LibreOffice is available. Place the portable runtime
at `native\libreoffice\program\soffice.exe` beside the native executable, or
set `FLOWDROP_LIBREOFFICE` to an installed `soffice.exe` path. The target
launches LibreOffice headlessly, writes a uniquely named `-converted` copy,
and never overwrites the original or an existing output. If the runtime is not
available, document petals remain unavailable and the drop returns
`DROPEFFECT_NONE`.

The implementation follows Microsoft's [RegisterDragDrop
documentation](https://learn.microsoft.com/en-us/windows/win32/api/ole2/nf-ole2-registerdragdrop),
[IDropTarget documentation](https://learn.microsoft.com/en-us/windows/win32/api/oleidl/nn-oleidl-idroptarget),
and [Shell clipboard format documentation](https://learn.microsoft.com/en-us/windows/win32/shell/clipboard).

The POC now uses a hybrid arrangement: low-level input is used only to show and
position the wheel after left mouse plus Shift activates the gesture. The actual
file list and conversion still come only from the registered OLE target's
`IDataObject`; the input observer never fabricates paths or performs a drop.
This makes the wheel follow the cursor without replacing the OLE data contract.

This POC intentionally keeps the wheel as a registered drop target only where
the HWND is visible. Windows calls `IDropTarget::DragEnter` only after the
cursor enters that HWND; it does not notify the POC when a drag starts
elsewhere during an Explorer drag. Consequently, this proves real OLE data
receipt and conversion, but does not and cannot make the wheel materialize
around an arbitrary cursor location without a global input hook or modifying
the drag source.

## Current scope and limitations
- This is the first MVP, not a complete Tangerine clone.
- Drag/drop currently works when dropping files onto the FlowDrop window, and
  selected files can also be sent from Explorer through the context menu.
- Image processing is local using `sharp`; no file contents are uploaded.
- Conversion output is written as `<original-name>-converted.<format>` beside the original and never overwrites the source.
- Animated GIF output is not enabled in this milestone.
- The UI uses system fonts only; the app works fully offline.

## Next engineering milestones
1. A separately packaged native Explorer extension, if a future Windows API
   provides access to another process's active drag data.
2. Shift+Alt advanced tools menu.
3. FFmpeg-based audio/video conversion and compression with a locally bundled binary.
4. PDF tools and document conversions, then metadata editing/redaction.
5. Bundle all assets and processing binaries; verify network-offline behavior and sign the installer.
