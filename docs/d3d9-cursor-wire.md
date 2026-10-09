# Cursor method transport

CURSOR_CALL30 carries a fixed 32-byte request for device methods10 SetCursorProperties,11 SetCursorPosition and12 ShowCursor. Method10 carries unsigned hotspot coordinates and a balanced same-device surface ID/generation pair; zero/zero represents a native null surface so the backend decides its HRESULT. Method11 preserves signed coordinate bits and all flags. Method12 preserves input BOOL bits. No HWND or native pointer crosses the wire.

The 16-byte reply preserves the actual SetCursorProperties HRESULT and ShowCursor BOOL bits. The void position call and value-return show call use internal S_OK after actual dispatch; the frontend exposes their original void/BOOL ABI. Transport failures have no value and must mark void/value calls sticky through the failure callback. Reserved fields and method-specific unused fields are rejected.

Focused host and ASan/UBSan codec checks pass, including signed minima, noncanonical true BOOL bits, typed reference shape, HRESULT preservation, truncation and atomic malformed-input rejection. The native and frontend adapters are separate.
