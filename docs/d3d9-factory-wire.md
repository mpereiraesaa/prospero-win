# D3D9 factory payload codec

This portable codec covers IDirect3D9 slots4–14 for the approved bridge. It
contains no backend calls, pointer transport, success stubs, or game proxy.
IUnknown ownership belongs to the local proxy/object registry. RegisterSoftwareDevice
(slot3), GetAdapterMonitor (15), and CreateDevice (16) explicitly return codec
UNSUPPORTED; their pointer/window/device lifetimes require separate adapters.

All fields are little endian. A request starts with four u32 words: version1,
method, argument count, zero reserved. Exactly the following arguments follow;
there is no implicit padding or trailing data:

| Slot | Method | Arguments in wire order |
| --- | --- | --- |
| 4 | GetAdapterCount | none |
| 5 | GetAdapterIdentifier | adapter, flags |
| 6 | GetAdapterModeCount | adapter, format |
| 7 | EnumAdapterModes | adapter, format, mode |
| 8 | GetAdapterDisplayMode | adapter |
| 9 | CheckDeviceType | adapter, device_type, format, format2, windowed |
| 10 | CheckDeviceFormat | adapter, device_type, format, usage, resource_type, format2 |
| 11 | CheckDeviceMultiSampleType | adapter, device_type, format, windowed, multisample |
| 12 | CheckDepthStencilMatch | adapter, device_type, format, format2, format3 |
| 13 | CheckDeviceFormatConversion | adapter, device_type, format, format2 |
| 14 | GetDeviceCaps | adapter, device_type |

Formats/enums remain exact u32 values for the real backend to validate. Normalize
BOOL input to0/1 in the PE adapter. A reply header is version, method, HRESULT
bits, output byte count. The outer transport HRESULT must equal the decoded
HRESULT; the adapter validates this. Failed HRESULTs carry no output. Native
count-returning methods4/6 require HRESULT0 and carry one u32 count; bridge
transport failures must be handled separately without inventing a backend result.
Method11 carries one quality-level count on success. Other check methods have
no output. Modes are width, height, refresh rate, format (four u32 values).

Adapter identifiers are encoded field by field: Driver512, Description512,
DeviceName32, DriverVersion64, VendorId/DeviceId/SubSysId/Revision32 each,
GUID Data1/2/3/Data4 as32/16/16/8bytes, WHQLLevel32. Total1100 bytes; native ABI
struct padding is excluded. Strings require a NUL inside their fixed bounds;
encoding zeroes the unused tail and decoding rejects nonzero tails. Initialize
backend output structs before calling DXVK and copy only declared fields.

Caps use76 named scalar words in PW_D3D9_CAP_FIELDS order. U32 fields are unsigned,
I32 fields preserve signed two's-complement bits, and F32 fields preserve IEEE754
binary32 bits (including NaN payloads) without conversion through double. The
macro also names each corresponding native D3DCAPS9 member for explicit adapters;
never copy a native struct wholesale. DTO structs are local API storage, not wire
layouts. Target object ID/generation and device/session identity remain in the
outer channel header.

Encoders compute exact sizes, leave wire storage untouched on failure, and never
allocate or call Wine. Decoders build a temporary DTO and publish only on success.
Caller input/output objects must not overlap. These helpers alone do not validate
backend ownership or acquire object references.

The host fixture covers every supported method, exact little-endian scalar bytes,
all truncation lengths, trailing bytes, count/HRESULT rules, canonical strings,
unsupported methods, BOOL normalization,64-bit versions/GUIDs, signed caps and
float payload bits. The same fixture cross-compiles for PE32/PE64; on Windows it
also checks all76 named native caps members against the pinned header layout.
